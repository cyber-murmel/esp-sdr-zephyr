/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Gapless receive on the ESP32-C6 (esp_sdr_gapless.h): the dump engine's ring
 * switched between SRAM blocks 1 and 2 (upstream ring_capture.c, after
 * h0m3us3r/eSpDR capture.c), the IQ estimator's windows deciding what is
 * kept, kept windows copied raw into a staging ring while their block can be
 * read and packed to int8 into the caller's sink from there, and two servos
 * (threshold, gain) keeping headroom on the link and at the converter.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_gapless.h>
#include <esp_sdr/esp_sdr_paced.h>
#include <esp_sdr/esp_sdr_rx.h>

#include "esp_sdr_priv.h"

extern uint32_t __esp_sdr_bank2_start[];

#define RING_MASK   (ESP_SDR_GAPLESS_RING - 1U)
#define THRESHOLD   ESP_SDR_GAPLESS_UNIT
/*
 * Marker windows: from a unit's predicted start and end on, in the block it
 * will be written to. The switch may come up to LATE_LIMIT samples (5 us)
 * after the threshold; with the run in IRAM it comes within 30. Half
 * upstream's C6 values: the markers cost half as much.
 */
#define START_GUARD     512U
#define END_GUARD       1024U
#define LATE_LIMIT      400U
#define UNIT0_GUARD     3072U
#define UNIT0_END_GUARD 1024U
#define MIN_PAIRS       THRESHOLD
#define MAX_PAIRS       (THRESHOLD + LATE_LIMIT + 64U)
#define MARK            0xA5C33C5AU
#define NOT_FOUND       0xffffffffU

BUILD_ASSERT(START_GUARD + THRESHOLD <= ESP_SDR_GAPLESS_RING, "start window overlaps");
BUILD_ASSERT(END_GUARD + THRESHOLD <= ESP_SDR_GAPLESS_RING, "end window overlaps unit start");
BUILD_ASSERT(2U * LATE_LIMIT + 64U < END_GUARD && LATE_LIMIT + 64U < START_GUARD, "guards");

/* DUMP_CTRL_REG circular mode: bit 17 with the count field's 16384 (bit 14). */
#define DUMP_CTRL_CIRCULAR (BIT(17) | BIT(14))
/* Block usage: one-hot in bits 11:8 (block 1 = bit 9) and bit 16, as upstream clears them. */
#define USAGE_CLEAR        0x10f00U
#define USAGE_BLOCK1       9U
#define IQE_POW_REG        0x600a0490U

#define CPP         (ESP_SDR_CPU_HZ / ESP_SDR_GAPLESS_RATE_HZ)
#define SETTLE_CYC  (16U * CPP)
#define MAX_AGE_CYC ((ESP_SDR_GAPLESS_RING - 128U) * CPP)
#define WINDOW_CYC  (ESP_SDR_GAPLESS_WINDOW_US * (ESP_SDR_CPU_HZ / 1000000U))
/* The estimator restarts every 10 windows: its sums stay below 2^31 at full scale. */
#define RESTART_CYC (WINDOW_CYC * 10U)
/* Marker preparation of a block: 1536 stores, measured 2050 cycles. */
#define PREP_CYC    3000U
#define PREP_PAIRS  (PREP_CYC / CPP + 256U)
/*
 * A window's length comes from the ring's index, so a reading may wait for a slice to end:
 * slices run up to SLICE_LATE cycles past a window's due time, and no reading starts closer
 * to a deadline than WINDOW_GUARD_CYC.
 */
#define SLICE_CYC_MAX    WINDOW_CYC
#define SLICE_CYC_MIN    150U
#define SLICE_LATE       400U
#define WINDOW_GUARD_CYC 1200U
/* The sink's service() runs whenever nothing else waits, and at least this often. */
#define SERVICE_CYC      (4U * WINDOW_CYC)
/* What a service() call may take until measured. */
#define SERVICE_CYC_START 3000U

#define LEARN_WINDOWS   64U
#define RELEARN_WINDOWS 1000U
#define BASE_FOLLOW     256
#define DC_FOLLOW_SHIFT 4

/* Windows remembered for the keep decision; pre + post + 1 must fit. */
#define WIN_HIST 16U
#define PREPOST_MAX 4U
BUILD_ASSERT(2U * PREPOST_MAX + 2U <= WIN_HIST);
/* Kept runs waiting to be copied. */
#define JOBS 32U

/*
 * Samples kept per unit, at most: a unit can be read only until its block is prepared again,
 * one unit later, so what the CPU cannot copy in that time is lost anyway. Better to keep a
 * burst's start than to lose most of it: past the cap a unit's windows are dropped. The cap
 * shrinks by an eighth after a unit that lost samples, and grows while units end with free
 * time left.
 */
#define CAP_START    4000U
#define CAP_MIN      512U
#define CAP_MAX      12000U
#define CAP_STEP     64U
#define CAP_IDLE_CYC 3000U

/*
 * Threshold servo: every SERVO_UNITS units (2.5 ms) the bytes kept are held against the
 * budget; the threshold over the baseline steps in Q8: +1 dB (323 / 256), +2 dB (406 / 256)
 * when over the budget or when samples were lost, -0.5 dB (241 / 256).
 */
#define SERVO_UNITS   16U
#define THR_UP        323U
#define THR_UP_FAST   406U
#define THR_DOWN      241U
#define THR_MAX_Q8    (1U << 24)
/* Bytes a segment header costs on the link (esdr_seg_hdr). */
#define SEG_OVERHEAD  16U

/*
 * Gain servo: a window's peak is taken as its DC plus 4 rms per component (12 dB crest).
 * Down as soon as a window passes full scale less the low headroom (up to 6 steps by the
 * excess, at most once per GAIN_DOWN_UNITS: 0.6 ms); up one step (about 1 dB) only when for
 * GAIN_UP_UNITS no window came within the high headroom of full scale with a step's margin,
 * and only while the quiet baseline stays below NOISE_MAX (more gain would not lift the
 * signal over the receiver's own noise, it would only eat headroom).
 */
#define FULL_SCALE      511
#define CREST           4
#define GAIN_DOWN_UNITS 4U
/* About 150 ms: the gain follows a band's bursts down at once, its rests slowly. */
#define GAIN_UP_UNITS   1000U
/* 1 dB in amplitude, Q8 (287 / 256 = 1.12). */
#define GAIN_STEP_Q8    287
#define NOISE_MAX       64U
/* The gain applied before the servo had a say, if none was forced. */
#define GAIN_START      60
/* A gain step costs this many cycles until measured. */
#define GAIN_CYC_START  4000U

/* The staging ring, and the runs of it waiting to be packed. */
#define STAGE_WORDS (CONFIG_ESP_SDR_GAPLESS_STAGE_KB * 256U)
#define STAGED      64U
/* Copy and pack costs per sample until measured, Q4 cycles. */
#define COPY_CPP_Q4_START (3U * 16U)
#define PACK_CPP_Q4_START (14U * 16U)
/* A pack slice's fixed cost until measured. */
#define PACK_OVH_START    600U

struct unit {
	uint64_t abs;
	uint32_t first, count;
	bool valid;
};

struct window {
	uint64_t s, e;
	uint32_t power;
	bool loud;
};

/* A kept run on the 80 MS/s axis, waiting to be copied. */
struct job {
	uint64_t s;
	uint32_t left;
	uint8_t shift, flags, gain;
	int8_t dc_i, dc_q;
	int16_t dbfs;
};

/* A run of raw words in the staging ring, waiting to be packed. */
struct staged {
	uint64_t index;
	uint32_t at, pairs;
	uint8_t shift, flags, gain;
	int8_t dc_i, dc_q;
	int16_t dbfs;
};

static uint32_t stage[STAGE_WORDS] __aligned(16) __attribute__((section(".esp_sdr_high")));

/* Run state, in one place so that the hot loop reaches it with one base register. */
static struct {
	const struct esp_sdr_gapless_cfg *cfg;
	const struct esp_sdr_gapless_sink *sink;
	struct esp_sdr_gapless_stats st;
	struct unit u[2];
	/* Windows: hist[n % WIN_HIST] is window n; nwin counted, ndec decided. */
	struct window hist[WIN_HIST];
	uint32_t nwin, ndec;
	/* Loud bits of the latest windows, bit 0 = window nwin - 1. */
	uint32_t loud_bits;
	/* End of the decided windows: kept or not, nothing before it is waited for. */
	uint64_t decided_to;
	struct job jobs[JOBS];
	uint32_t job_head, job_tail;
	/* Samples the queued jobs still hold. */
	uint32_t job_pairs;
	uint8_t pending_flags;
	/* DC in raw units, Q4. */
	int32_t dc_i, dc_q;
	/* Keep threshold and quiet threshold, window power per sample. */
	uint32_t thr, quiet;
	/* Threshold over the baseline (Q8), its floor, bytes kept and samples lost this period. */
	uint32_t thr_q8, thr_floor_q8;
	uint64_t servo_bytes, servo_pairs, servo_lost;
	uint32_t servo_units;
	/* Peak limits per component: the low headroom's, and the high headroom's less a step. */
	int32_t lim_hot, lim_warm;
	/* Gain servo: the gain, its range, units since a warm window or a step, a step due. */
	int gain, gain_lo, gain_hi;
	uint32_t since_warm, since_step;
	int gain_due;
	uint32_t gain_cyc;
	/* Windows to leave out of the baseline after a gain step. */
	uint32_t settle_windows;
	/*
	 * The per unit cap; what the unit being written and the one before kept (by where the
	 * window lies, not when it was decided), the start of the unit being written; whether
	 * this unit hit the cap or lost samples.
	 */
	uint32_t cap, kept_cur, kept_prev;
	uint64_t cur_abs;
	bool unit_capped, unit_lost;
	/* Staging ring: runs, write and read positions, words in use. */
	struct staged sq[STAGED];
	uint32_t sq_head, sq_tail;
	uint32_t st_head, st_tail, st_used;
	/*
	 * Copy cost per sample and pack cost per sample by shift (0 and 1 share a path), Q4 cycles;
	 * a pack slice's fixed cost (the sink's reserve() and commit()), cycles.
	 */
	uint32_t copy_cpp_q4, pack_cpp_q4[2], pack_ovh;
	/* This unit's cycles by activity; the longest window reading and service() call. */
	uint32_t ucyc[ESP_SDR_GAPLESS_ACTIVITIES];
	uint32_t window_cyc_max, service_cyc;
	/* The last step: its activity, when it began, the cycles it had before the deadline. */
	uint32_t last_activity, last_t, last_room;
} rs;

/* Quiet baseline, window power per sample, Q8 (a plain integer IIR stalls at small values). */
static uint32_t baseline_q8;
static uint32_t learnt;
static uint32_t loud_run;
static uint32_t learn_pw[LEARN_WINDOWS];

static ALWAYS_INLINE uint32_t cycles(void)
{
	return esp_sdr_paced_cycles();
}

static ALWAYS_INLINE uint32_t *bank_ptr(unsigned int b)
{
	return b == 0U ? (uint32_t *)__esp_sdr_bank_start : __esp_sdr_bank2_start;
}

static ALWAYS_INLINE uint32_t write_index(void)
{
	return sys_read32(DUMP_MODE_REG) & RING_MASK;
}

static ALWAYS_INLINE void select_blocks(uint32_t saved, unsigned int mask)
{
	sys_write32((saved & ~USAGE_CLEAR) | (mask << USAGE_BLOCK1), DUMP_USAGE_REG);
	/* The switch must land before the next store or poll. */
	__asm__ volatile("fence rw, rw" ::: "memory");
	(void)sys_read32(DUMP_USAGE_REG);
}

/* ---- markers ---- */

static ESP_SDR_GAPLESS_HOT void fill_marks(unsigned int b, uint32_t at, uint32_t count)
{
	at &= RING_MASK;
	while (count != 0U) {
		uint32_t n = MIN(ESP_SDR_GAPLESS_RING - at, count);
		volatile uint32_t *p = bank_ptr(b) + at;

		count -= n;
		at = 0;
		for (; n >= 8U; n -= 8U, p += 8) {
			p[0] = MARK;
			p[1] = MARK;
			p[2] = MARK;
			p[3] = MARK;
			p[4] = MARK;
			p[5] = MARK;
			p[6] = MARK;
			p[7] = MARK;
		}
		for (; n != 0U; n--) {
			*p++ = MARK;
		}
	}
	__asm__ volatile("fence rw, rw" ::: "memory");
}

/* The first written sample in [origin, origin + guard): markers up to the unit's start, data on. */
static ESP_SDR_GAPLESS_HOT uint32_t find_first(unsigned int b, uint32_t origin, uint32_t guard)
{
	const volatile uint32_t *p = bank_ptr(b);
	uint32_t lo = 0, hi = guard - 1U;

	if (p[origin & RING_MASK] != MARK || p[(origin + guard - 1U) & RING_MASK] == MARK) {
		return NOT_FOUND;
	}
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2U;

		if (p[(origin + mid) & RING_MASK] == MARK) {
			lo = mid + 1U;
		} else {
			hi = mid;
		}
	}
	return (origin + lo) & RING_MASK;
}

/* One past the last written sample in [origin, origin + guard), from the last index seen on. */
static ESP_SDR_GAPLESS_HOT uint32_t find_end(unsigned int b, uint32_t origin, uint32_t wi,
					     uint32_t guard)
{
	const volatile uint32_t *p = bank_ptr(b);
	uint32_t lo = (wi - origin) & RING_MASK, hi = guard - 1U;

	if (lo >= guard || p[(origin + guard - 1U) & RING_MASK] != MARK) {
		return NOT_FOUND;
	}
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2U;

		if (p[(origin + mid) & RING_MASK] == MARK) {
			hi = mid;
		} else {
			lo = mid + 1U;
		}
	}
	return (origin + lo) & RING_MASK;
}

/* Markers into block @p b where the unit after the one from @p expected on starts and ends. */
static ESP_SDR_GAPLESS_HOT void prepare(unsigned int b, uint32_t expected, uint32_t *start_probe,
					uint32_t *end_probe)
{
	rs.u[b].valid = false;
	start_probe[b] = (expected + THRESHOLD) & RING_MASK;
	end_probe[b] = (start_probe[b] + THRESHOLD) & RING_MASK;
	fill_marks(b, start_probe[b], START_GUARD);
	fill_marks(b, end_probe[b], END_GUARD);
}

/* ---- estimator ---- */

struct est {
	uint64_t abs;
	uint32_t p, si, sq;
};

static ESP_SDR_GAPLESS_HOT void est_restart(uint32_t ctrl1)
{
	sys_write32(ctrl1, IQE_CTRL1_REG);
	sys_write32(ctrl1 | IQE_CTRL1_EN, IQE_CTRL1_REG);
	sys_write32(ctrl1 | IQE_CTRL1_EN | IQE_CTRL1_START, IQE_CTRL1_REG);
}

/* The sums and where the ring is: @p ref is a ring index whose absolute index is @p ref_abs. */
static ALWAYS_INLINE void est_read(struct est *e, uint32_t ref, uint64_t ref_abs)
{
	e->si = sys_read32(IQE_SUM_I_REG);
	e->sq = sys_read32(IQE_SUM_Q_REG);
	e->p = sys_read32(IQE_POW_REG);
	e->abs = ref_abs + ((write_index() - ref) & RING_MASK);
}

/*
 * The power of the inputs between two readings about their mean (sum of I^2 + Q^2 less
 * (sum I)^2 + (sum Q)^2 over their count), per input; the count from the ring's index. One
 * hardware division (n's reciprocal, Q24), the rest products: with m = round(s / n),
 * s^2 / n = m (2 s - n m) + n d^2 where d = s / n - m, so the error is d^2 <= 1/4 per input
 * (a little more where the reciprocal rounds m the other way). A window's sums stay far
 * below 2^31 (up to 4096 inputs of 10 bits).
 */
static ESP_SDR_GAPLESS_HOT uint32_t est_power(const struct est *a, const struct est *b,
					      int32_t *mi, int32_t *mq)
{
	uint32_t n = MIN((uint32_t)(b->abs - a->abs), 4096U);
	int32_t si = (int32_t)(b->si - a->si) / 64;
	int32_t sq = (int32_t)(b->sq - a->sq) / 64;
	uint32_t p8 = (b->p - a->p) * 8U;
	uint32_t inv, sub;
	int32_t di, dq;

	if (n == 0U) {
		*mi = *mq = 0;
		return 0U;
	}
	inv = ((1U << 24) + n / 2U) / n;
	*mi = (int32_t)(((int64_t)si * (int32_t)inv + (1 << 23)) >> 24);
	*mq = (int32_t)(((int64_t)sq * (int32_t)inv + (1 << 23)) >> 24);
	di = MAX(*mi * (2 * si - (int32_t)n * *mi), 0);
	dq = MAX(*mq * (2 * sq - (int32_t)n * *mq), 0);
	sub = (uint32_t)di + (uint32_t)dq;
	return p8 > sub ? (uint32_t)(((uint64_t)(p8 - sub) * inv + (1U << 23)) >> 24) : 0U;
}

/* ---- numbers ---- */

/* Does DC @p dc plus CREST rms of a window of @p power per sample stay within @p lim? */
static ESP_SDR_GAPLESS_HOT bool peak_within(uint32_t power, int32_t dc, int32_t lim)
{
	int32_t room = lim - (dc < 0 ? -dc : dc);

	/* rms per component = sqrt(power / 2): CREST^2 * power / 2 <= room^2. */
	return room > 0 && (uint64_t)power * (CREST * CREST / 2) <= (uint64_t)room * room;
}

ESP_SDR_GAPLESS_HOT unsigned int esp_sdr_gapless_shift(uint32_t power, int32_t dc_i, int32_t dc_q)
{
	int32_t dc = MAX(dc_i < 0 ? -dc_i : dc_i, dc_q < 0 ? -dc_q : dc_q);

	for (unsigned int s = 0; s < 2U; s++) {
		if (peak_within(power, dc, 127 << s)) {
			return s;
		}
	}
	return 2U;
}

/* Index of the highest set bit of @p v > 0; __builtin_clz() would call libgcc. */
static ALWAYS_INLINE int32_t msb32(uint32_t v)
{
	int32_t m = 0;

	if (v >= 1U << 16) {
		v >>= 16;
		m += 16;
	}
	if (v >= 1U << 8) {
		v >>= 8;
		m += 8;
	}
	if (v >= 1U << 4) {
		v >>= 4;
		m += 4;
	}
	if (v >= 1U << 2) {
		v >>= 2;
		m += 2;
	}
	return m + (v >= 2U ? 1 : 0);
}

ESP_SDR_GAPLESS_HOT int16_t esp_sdr_gapless_dbfs_x10(uint32_t power)
{
	/* log2(1 + k / 16) in Q8. */
	static const DRAM_ATTR uint8_t frac[16] = {0, 22, 43, 63, 82, 101, 118, 135,
						   151, 167, 182, 196, 210, 224, 237, 249};
	int32_t msb, l2;

	if (power == 0U) {
		return -1000;
	}
	msb = msb32(power);
	l2 = msb * 256 + frac[msb >= 4 ? (power >> (msb - 4)) & 15U : (power << (4 - msb)) & 15U];
	/* Full scale per sample: 2 * 512^2 = 2^19; 10 log10(2) * 10 / 256 = 7706 / 65536. */
	return (int16_t)(((l2 - 19 * 256) * 7706) >> 16);
}

/* ---- packing ---- */

/* Shift 2 keeps a sample's top 8 bits, which always fit: I from bits 19:12, Q from 9:2. */
static ALWAYS_INLINE uint32_t pack_top8(uint32_t v)
{
	return ((v >> 12) & 0xffU) | ((v << 6) & 0xff00U);
}

/* Shifts 0 and 1 through tables of the saturated value of each 10-bit code, built once. */
static uint8_t sat_lut[2][1024];

static void sat_lut_init(void)
{
	if (sat_lut[1][1023] != 0U) {
		return;
	}
	for (unsigned int s = 0; s < 2U; s++) {
		for (int32_t c = 0; c < 1024; c++) {
			int32_t v = (c >= 512 ? c - 1024 : c) >> s;

			sat_lut[s][c] = (uint8_t)CLAMP(v, -128, 127);
		}
	}
}

static ALWAYS_INLINE uint32_t pack_lut(uint32_t v, const uint8_t *lut)
{
	return lut[(v >> 10) & 0x3ffU] | ((uint32_t)lut[v & 0x3ffU] << 8);
}

static ESP_SDR_GAPLESS_HOT __attribute__((optimize("O3"))) void
pack_run(const uint32_t *w, uint32_t n, uint16_t *o, unsigned int shift)
{
	uint32_t *o32;

	if (shift >= 2U) {
		/* Two samples per 32-bit store where the output is aligned for it. */
		if (((uintptr_t)o & 2U) != 0U && n > 0U) {
			*o++ = (uint16_t)pack_top8(*w++);
			n--;
		}
		o32 = (uint32_t *)o;
		for (; n >= 4U; n -= 4U, w += 4) {
			uint32_t a = w[0], b = w[1], c = w[2], d = w[3];

			o32[0] = pack_top8(a) | (pack_top8(b) << 16);
			o32[1] = pack_top8(c) | (pack_top8(d) << 16);
			o32 += 2;
		}
		o = (uint16_t *)o32;
		for (; n > 0U; n--) {
			*o++ = (uint16_t)pack_top8(*w++);
		}
		return;
	}
	const uint8_t *lut = sat_lut[shift];

	if (((uintptr_t)o & 2U) != 0U && n > 0U) {
		*o++ = (uint16_t)pack_lut(*w++, lut);
		n--;
	}
	o32 = (uint32_t *)o;
	for (; n >= 4U; n -= 4U, w += 4) {
		uint32_t a = w[0], b = w[1], c = w[2], d = w[3];

		o32[0] = pack_lut(a, lut) | (pack_lut(b, lut) << 16);
		o32[1] = pack_lut(c, lut) | (pack_lut(d, lut) << 16);
		o32 += 2;
	}
	o = (uint16_t *)o32;
	for (; n > 0U; n--) {
		*o++ = (uint16_t)pack_lut(*w++, lut);
	}
}

/* ---- servos ---- */

static ESP_SDR_GAPLESS_HOT void thr_update(void)
{
	if (rs.cfg->level != 0U) {
		rs.thr = rs.quiet = rs.cfg->level;
		return;
	}
	if (learnt < LEARN_WINDOWS) {
		rs.thr = rs.quiet = UINT32_MAX;
		return;
	}
	rs.thr = (uint32_t)MIN((uint64_t)baseline_q8 * rs.thr_q8 >> 16, UINT32_MAX);
	rs.quiet = (uint32_t)MIN((uint64_t)baseline_q8 * rs.thr_floor_q8 >> 16, UINT32_MAX);
}

static ESP_SDR_GAPLESS_HOT void baseline_update(uint32_t pw)
{
	if (rs.cfg->level != 0U) {
		return;
	}
	if (rs.settle_windows > 0U) {
		rs.settle_windows--;
		return;
	}
	if (learnt < LEARN_WINDOWS) {
		/* Kept sorted as it fills: the lower quartile is then one read. */
		uint32_t j = learnt++;

		for (; j > 0U && learn_pw[j - 1U] > pw; j--) {
			learn_pw[j] = learn_pw[j - 1U];
		}
		learn_pw[j] = pw;
		if (learnt == LEARN_WINDOWS) {
			baseline_q8 = MAX(learn_pw[LEARN_WINDOWS / 4U], 1U) << 8;
			thr_update();
		}
	} else if (pw <= rs.quiet) {
		loud_run = 0;
		baseline_q8 = (uint32_t)((int32_t)baseline_q8 +
					 ((int32_t)(MIN(pw, 0x7fffffU) << 8) - (int32_t)baseline_q8) /
						 BASE_FOLLOW);
		baseline_q8 = MAX(baseline_q8, 256U);
		thr_update();
	} else if (++loud_run >= RELEARN_WINDOWS) {
		learnt = 0;
		loud_run = 0;
		thr_update();
	}
}

/* Once per unit of @p count samples: the threshold against the link's budget. */
static ESP_SDR_GAPLESS_HOT void thr_servo(uint32_t count, uint32_t lo, uint32_t hi)
{
	uint32_t budget, bytes;

	rs.servo_pairs += count;
	if (++rs.servo_units < SERVO_UNITS) {
		return;
	}
	/* budget_bps * pairs / 80e6 in 32 bits: (bytes per ms) * (pairs per 80) / 1000. */
	budget = (rs.cfg->budget_bps / 1000U) * (uint32_t)(rs.servo_pairs / 80U) / 1000U;
	bytes = (uint32_t)MIN(rs.servo_bytes, UINT32_MAX / 100U);
	/*
	 * Over the budget or losing samples: up fast; past the low headroom: up; below the high
	 * headroom: down. Windows dropped for want of room in the staging ring do not count: a
	 * burst longer than the ring drops its tail whatever the threshold, and a band that
	 * bursts and rests would only push it up for the rests.
	 */
	if (rs.servo_lost > 0U || bytes > budget) {
		rs.thr_q8 = (uint32_t)MIN((uint64_t)rs.thr_q8 * THR_UP_FAST >> 8, THR_MAX_Q8);
	} else if (bytes * 100U > budget * (100U - lo)) {
		rs.thr_q8 = (uint32_t)MIN((uint64_t)rs.thr_q8 * THR_UP >> 8, THR_MAX_Q8);
	} else if (bytes * 100U < budget * (100U - hi)) {
		rs.thr_q8 = MAX((uint32_t)((uint64_t)rs.thr_q8 * THR_DOWN >> 8), rs.thr_floor_q8);
	}
	rs.servo_units = 0;
	rs.servo_pairs = 0;
	rs.servo_bytes = 0;
	rs.servo_lost = 0;
	thr_update();
}

/* A window's peak against the limits; the gain servo's input. */
static ESP_SDR_GAPLESS_HOT void gain_watch(uint32_t pw)
{
	int32_t dc = MAX(abs(rs.dc_i), abs(rs.dc_q)) >> 4;

	if (!peak_within(pw, dc, rs.lim_hot)) {
		int32_t room = MAX(rs.lim_hot - dc, 1);
		/* Excess in powers of 4 (6 dB): 1, 3 or 6 steps down. */
		uint64_t over = (uint64_t)pw * (CREST * CREST / 2);
		uint64_t r2 = (uint64_t)room * room;
		int steps = over > 16U * r2 ? 6 : over > 4U * r2 ? 3 : 1;

		rs.st.hot_windows++;
		rs.since_warm = 0;
		rs.gain_due = MIN(rs.gain_due, -steps);
	} else if (!peak_within(pw, dc, rs.lim_warm)) {
		rs.since_warm = 0;
	}
}

/* Once per unit: is a step up due? (Steps down come from gain_watch().) */
static ESP_SDR_GAPLESS_HOT void gain_servo(void)
{
	rs.since_warm++;
	rs.since_step++;
	if (rs.gain_due < 0) {
		if (rs.gain <= rs.gain_lo) {
			rs.gain_due = 0;
		}
		return;
	}
	if (rs.since_warm >= GAIN_UP_UNITS && rs.since_step >= GAIN_UP_UNITS &&
	    rs.gain < rs.gain_hi && learnt >= LEARN_WINDOWS && baseline_q8 < (NOISE_MAX << 8)) {
		rs.gain_due = 1;
	}
}

/* Apply a due step, if @p room cycles allow it. */
static ESP_SDR_GAPLESS_HOT bool gain_step(uint32_t room)
{
	int g;
	uint32_t dt;

	if (room < rs.gain_cyc || rs.since_step < GAIN_DOWN_UNITS) {
		return false;
	}
	g = CLAMP(rs.gain + rs.gain_due, rs.gain_lo, rs.gain_hi);
	rs.gain_due = 0;
	if (g == rs.gain) {
		return false;
	}
	dt = cycles();
	(void)esp_sdr_rx_gain_force_locked(g);
	dt = cycles() - dt;
	rs.gain_cyc = MAX(rs.gain_cyc, dt + dt / 4U);
	rs.st.gain_cycles_max = MAX(rs.st.gain_cycles_max, dt);
	/* The baseline follows the step (about 1 dB a step in power: 323 / 256 per step up). */
	for (int k = rs.gain; k < g; k++) {
		baseline_q8 = (uint32_t)MIN((uint64_t)baseline_q8 * 323U >> 8, 0x7fffffffU);
	}
	for (int k = g; k < rs.gain; k++) {
		baseline_q8 = MAX((uint32_t)((uint64_t)baseline_q8 * 203U >> 8), 256U);
	}
	thr_update();
	rs.gain = g;
	rs.since_step = 0;
	rs.since_warm = 0;
	rs.settle_windows = 2U;
	rs.st.gain_steps++;
	return true;
}

/* ---- keep decision ---- */

static ESP_SDR_GAPLESS_HOT void job_add(uint64_t s, uint64_t e, unsigned int shift, uint8_t flags,
					uint32_t power)
{
	struct job *last = &rs.jobs[(rs.job_tail - 1U) % JOBS];
	uint32_t n = (uint32_t)(e - s);
	uint32_t *kept = s >= rs.cur_abs ? &rs.kept_cur : &rs.kept_prev;

	/*
	 * Admitted only with room for it in the staging ring, counting what the queued jobs will
	 * copy: a burst longer than the ring keeps its start, and nothing kept is lost later.
	 * The cap is the other limit: what the CPU can copy before the block is needed again.
	 */
	if (rs.st_used + rs.job_pairs + n > STAGE_WORDS) {
		rs.st.dropped_pairs += n;
		rs.pending_flags |= ESP_SDR_GAPLESS_SEG_DROPPED;
		return;
	}
	if (*kept >= rs.cap || rs.job_tail - rs.job_head >= JOBS) {
		rs.unit_capped = true;
		rs.st.dropped_pairs += n;
		rs.pending_flags |= ESP_SDR_GAPLESS_SEG_DROPPED;
		return;
	}
	*kept += n;
	rs.job_pairs += n;
	rs.servo_bytes += n * 2U;
	if (rs.job_tail != rs.job_head && last->s + last->left == s && last->shift == shift &&
	    last->gain == (uint8_t)rs.gain) {
		last->left += n;
		return;
	}
	rs.servo_bytes += SEG_OVERHEAD;
	rs.jobs[rs.job_tail % JOBS] = (struct job){
		.s = s, .left = n, .shift = (uint8_t)shift, .flags = flags,
		.gain = (uint8_t)rs.gain,
		.dc_i = (int8_t)CLAMP((rs.dc_i + 8) >> 4, INT8_MIN, INT8_MAX),
		.dc_q = (int8_t)CLAMP((rs.dc_q + 8) >> 4, INT8_MIN, INT8_MAX),
		.dbfs = esp_sdr_gapless_dbfs_x10(power)};
	rs.job_tail++;
}

/* Decide window @p w (all windows up to w + pre are in), queueing it if kept. */
static ESP_SDR_GAPLESS_HOT void decide(uint32_t w, uint32_t pre, uint32_t span)
{
	/* Bit k of loud_bits is window nwin - 1 - k: window w is bit nwin - 1 - w. */
	uint32_t at = rs.nwin - 1U - w;
	uint32_t near = (rs.loud_bits >> (at - pre)) & BIT_MASK(span);
	const struct window *win = &rs.hist[w % WIN_HIST];
	uint32_t pmax = 0;

	rs.decided_to = win->e;
	if (near == 0U) {
		return;
	}
	/* The loudest window around sets the shift, so a burst and its edges share one. */
	for (uint32_t k = 0, idx = w + pre; k < span && idx + 1U > 0U; k++, idx--) {
		if (rs.nwin - idx <= WIN_HIST) {
			pmax = MAX(pmax, rs.hist[idx % WIN_HIST].power);
		}
	}
	rs.st.kept_windows++;
	job_add(win->s, win->e, esp_sdr_gapless_shift(pmax, rs.dc_i >> 4, rs.dc_q >> 4),
		win->loud ? ESP_SDR_GAPLESS_SEG_LOUD : 0U, win->power);
}

static ESP_SDR_GAPLESS_HOT void window_done(const struct est *a, const struct est *b)
{
	struct window *win = &rs.hist[rs.nwin % WIN_HIST];
	uint32_t pre = MIN(rs.cfg->pre, PREPOST_MAX);
	uint32_t span = pre + MIN(rs.cfg->post, PREPOST_MAX) + 1U;
	int32_t mi, mq;
	uint32_t pw = est_power(a, b, &mi, &mq);

	/* Windows tile the axis: one starts where the last ended (restarts leave a few samples out). */
	win->s = rs.nwin == 0U ? a->abs : rs.hist[(rs.nwin - 1U) % WIN_HIST].e;
	win->e = b->abs;
	win->power = pw;
	win->loud = pw > rs.thr;
	rs.dc_i += ((mi << 4) - rs.dc_i) >> DC_FOLLOW_SHIFT;
	rs.dc_q += ((mq << 4) - rs.dc_q) >> DC_FOLLOW_SHIFT;
	rs.loud_bits = (rs.loud_bits << 1) | (win->loud ? 1U : 0U);
	rs.nwin++;
	rs.st.windows++;
	rs.st.loud_windows += win->loud ? 1U : 0U;
	if (rs.cfg->gain_servo) {
		gain_watch(pw);
	}
	baseline_update(pw);
	while (rs.ndec + pre < rs.nwin) {
		decide(rs.ndec++, pre, span);
	}
}

/* ---- copy and pack ---- */

enum slice_result {
	SLICE_IDLE,
	SLICE_DONE,
	/* No room: in the staging ring (the samples wait in their block) or in the sink. */
	SLICE_FULL,
};

/* Contiguous free words at the staging ring's write position; runs never wrap. */
static ESP_SDR_GAPLESS_HOT uint32_t stage_contig(void)
{
	if (rs.st_used == 0U) {
		rs.st_head = rs.st_tail = 0;
		return STAGE_WORDS;
	}
	if (rs.st_used == STAGE_WORDS) {
		return 0U;
	}
	if (rs.st_head == STAGE_WORDS) {
		if (rs.st_tail == 0U) {
			return 0U;
		}
		rs.st_head = 0;
	}
	return rs.st_head >= rs.st_tail ? STAGE_WORDS - rs.st_head : rs.st_tail - rs.st_head;
}

static ESP_SDR_GAPLESS_HOT __attribute__((optimize("O3"))) void
copy_words(uint32_t *dst, const uint32_t *src, uint32_t n)
{
	for (; n >= 8U; n -= 8U, src += 8, dst += 8) {
		uint32_t a = src[0], b = src[1], c = src[2], d = src[3];
		uint32_t e = src[4], f = src[5], g = src[6], h = src[7];

		dst[0] = a;
		dst[1] = b;
		dst[2] = c;
		dst[3] = d;
		dst[4] = e;
		dst[5] = f;
		dst[6] = g;
		dst[7] = h;
	}
	for (; n > 0U; n--) {
		*dst++ = *src++;
	}
}

/*
 * Copy from the front job into the staging ring as far as @p budget cycles go; @p cur_abs is
 * the first sample of the unit being written, which cannot be read yet. This is the step
 * with a deadline: a unit's block is readable until it is prepared again.
 */
static ESP_SDR_GAPLESS_HOT enum slice_result copy_slice(uint32_t budget, uint64_t cur_abs)
{
	struct job *j;
	const struct unit *u = NULL;
	struct staged *last;
	uint32_t n, t0, dt, at, run;
	bool extend;

	if (rs.job_head == rs.job_tail) {
		return SLICE_IDLE;
	}
	j = &rs.jobs[rs.job_head % JOBS];
	for (unsigned int b = 0; b < 2U; b++) {
		const struct unit *c = &rs.u[b];

		if (c->valid && j->s >= c->abs && j->s < c->abs + c->count) {
			u = c;
		}
	}
	if (u == NULL) {
		uint64_t older = UINT64_MAX;

		if (j->s >= cur_abs) {
			/* Still being written. */
			return SLICE_IDLE;
		}
		/* Its block was needed again before it was copied: up to the oldest valid unit. */
		for (unsigned int b = 0; b < 2U; b++) {
			if (rs.u[b].valid && rs.u[b].abs > j->s) {
				older = MIN(older, rs.u[b].abs);
			}
		}
		n = (uint32_t)MIN((uint64_t)j->left, MIN(older, cur_abs) - j->s);
		rs.st.abandoned_pairs += n;
		rs.servo_lost += n;
		rs.unit_lost = true;
		rs.pending_flags |= ESP_SDR_GAPLESS_SEG_DROPPED;
		rs.job_pairs -= n;
		j->s += n;
		j->left -= n;
		if (j->left == 0U) {
			rs.job_head++;
		}
		return SLICE_DONE;
	}
	n = MIN(j->left, (uint32_t)(u->abs + u->count - j->s));
	n = MIN(n, MAX(budget * 14U / rs.copy_cpp_q4, 16U));
	n = MIN(n, stage_contig());
	last = &rs.sq[(rs.sq_tail - 1U) % STAGED];
	extend = rs.sq_tail != rs.sq_head && last->index + last->pairs == j->s &&
		 last->at + last->pairs == rs.st_head && last->shift == j->shift &&
		 last->gain == j->gain && rs.pending_flags == 0U;
	if (n == 0U || (!extend && rs.sq_tail - rs.sq_head >= STAGED)) {
		return SLICE_FULL;
	}
	at = (u->first + (uint32_t)(j->s - u->abs)) & RING_MASK;
	t0 = cycles();
	/* Up to the ring's end, then from its start. */
	run = MIN(n, ESP_SDR_GAPLESS_RING - at);
	copy_words(&stage[rs.st_head], bank_ptr((unsigned int)(u - rs.u)) + at, run);
	if (run < n) {
		copy_words(&stage[rs.st_head + run], bank_ptr((unsigned int)(u - rs.u)), n - run);
	}
	dt = cycles() - t0;
	if (n >= 64U) {
		uint32_t c = dt * 16U / n;

		rs.copy_cpp_q4 = c > rs.copy_cpp_q4 ? c : rs.copy_cpp_q4 - (rs.copy_cpp_q4 - c) / 8U;
		rs.copy_cpp_q4 = MAX(rs.copy_cpp_q4, 8U);
	}
	if (extend) {
		last->pairs += n;
	} else {
		rs.sq[rs.sq_tail % STAGED] = (struct staged){
			.index = j->s, .at = rs.st_head, .pairs = n, .shift = j->shift,
			.flags = j->flags | rs.pending_flags, .gain = j->gain, .dc_i = j->dc_i,
			.dc_q = j->dc_q, .dbfs = j->dbfs};
		rs.sq_tail++;
		rs.pending_flags = 0U;
		/* Only the first run of a job carries its flags. */
		j->flags &= ~ESP_SDR_GAPLESS_SEG_LOUD;
	}
	rs.st_head += n;
	rs.st_used += n;
	rs.job_pairs -= n;
	rs.st.stage_peak = MAX(rs.st.stage_peak, rs.st_used);
	j->s += n;
	j->left -= n;
	if (j->left == 0U) {
		rs.job_head++;
	}
	return SLICE_DONE;
}

/* Pack from the front staged run into the sink as far as @p budget cycles go: no deadline. */
static ESP_SDR_GAPLESS_HOT enum slice_result pack_slice(uint32_t budget)
{
	struct staged *e;
	struct esp_sdr_gapless_seg seg;
	uint32_t n, t_all, t0, dt, ovh, *cpp;
	int8_t *dst;

	if (rs.sq_head == rs.sq_tail) {
		return SLICE_IDLE;
	}
	e = &rs.sq[rs.sq_head % STAGED];
	cpp = &rs.pack_cpp_q4[e->shift >= 2U ? 1 : 0];
	/* Not even a few samples fit after the slice's fixed cost: not now. */
	if (budget <= rs.pack_ovh + (8U * *cpp >> 4)) {
		return SLICE_IDLE;
	}
	t_all = cycles();
	/* An eighth of what is left is margin: the estimate is an average. */
	seg = (struct esp_sdr_gapless_seg){
		.index = e->index,
		.pairs = MIN(e->pairs, MAX((budget - rs.pack_ovh) * 14U / *cpp, 8U)),
		.shift = e->shift,
		.flags = e->flags,
		.gain = e->gain,
		.dc_i = e->dc_i,
		.dc_q = e->dc_q,
		.power_dbfs_x10 = e->dbfs,
	};
	n = rs.sink->reserve(&seg, &dst, rs.sink->user);
	if (n == 0U) {
		return SLICE_FULL;
	}
	n = MIN(n, seg.pairs);
	t0 = cycles();
	pack_run(&stage[e->at], n, (uint16_t *)dst, e->shift);
	dt = cycles() - t0;
	rs.sink->commit(n, rs.sink->user);
	/* The fixed cost rises at once, falls slowly. */
	ovh = (cycles() - t_all) - dt;
	rs.pack_ovh = ovh > rs.pack_ovh ? ovh : rs.pack_ovh - (rs.pack_ovh - ovh) / 16U;
	rs.st.kept_pairs += n;
	rs.st.pack_cycles_max = MAX(rs.st.pack_cycles_max, dt);
	/* Running estimate of the cost per sample, for the budget; it rises at once. */
	if (n >= 32U) {
		uint32_t c = dt * 16U / n;

		*cpp = c > *cpp ? c : *cpp - (*cpp - c) / 8U;
		*cpp = MAX(*cpp, 16U);
	}
	/* The rest of the run is no segment start: no flags. */
	e->flags = 0U;
	e->index += n;
	e->at += n;
	e->pairs -= n;
	rs.st_tail = e->at;
	rs.st_used -= n;
	if (e->pairs == 0U) {
		rs.sq_head++;
	}
	return SLICE_DONE;
}

/* Nothing waits for unit @p u any more: no job and no undecided window reaches into it. */
static ESP_SDR_GAPLESS_HOT bool unit_done(const struct unit *u)
{
	uint64_t end = u->abs + u->count;

	if (!u->valid) {
		return true;
	}
	if (rs.decided_to < end) {
		return false;
	}
	return rs.job_head == rs.job_tail || rs.jobs[rs.job_head % JOBS].s >= end;
}

/* ---- run ---- */

void esp_sdr_gapless_reset(void)
{
	baseline_q8 = 0;
	learnt = 0;
	loud_run = 0;
}

static void run_init(const struct esp_sdr_gapless_cfg *cfg, const struct esp_sdr_gapless_sink *sink,
		     uint32_t lo, uint32_t hi)
{
	int gmax = esp_sdr_rx_gain_max();

	memset(&rs, 0, sizeof(rs));
	sat_lut_init();
	rs.cfg = cfg;
	rs.sink = sink;
	rs.service_cyc = SERVICE_CYC_START;
	rs.pack_ovh = PACK_OVH_START;
	rs.pack_cpp_q4[0] = PACK_CPP_Q4_START;
	rs.pack_cpp_q4[1] = PACK_CPP_Q4_START;
	rs.copy_cpp_q4 = COPY_CPP_Q4_START;
	rs.st.idle_cycles_min = UINT32_MAX;
	rs.cap = CAP_START;
	rs.thr_floor_q8 = (cfg->ratio_x16 != 0U ? cfg->ratio_x16 : 64U) * 16U;
	rs.thr_q8 = rs.thr_floor_q8;
	rs.lim_hot = FULL_SCALE * (int32_t)(100U - lo) / 100;
	/* The high headroom's limit, a step (1 dB) back: a step up must not cross it. */
	rs.lim_warm = (FULL_SCALE * (int32_t)(100U - hi) / 100) * 256 / GAIN_STEP_Q8;
	rs.gain_lo = cfg->gain_min;
	rs.gain_hi = cfg->gain_max != 0U ? MIN((int)cfg->gain_max, gmax) : gmax;
	rs.gain = esp_sdr_rx_get_gain();
	rs.gain_cyc = GAIN_CYC_START;
	rs.since_step = GAIN_UP_UNITS;
	if (cfg->gain_servo) {
		if (rs.gain < 0) {
			rs.gain = MIN(GAIN_START, rs.gain_hi);
		}
		rs.gain = CLAMP(rs.gain, rs.gain_lo, rs.gain_hi);
		(void)esp_sdr_rx_gain_force_locked(rs.gain);
	}
	/* Without the servo an automatic gain stays so: segments say 255. */
	thr_update();
}

/* All of the run is hot: with interrupts off a flash cache miss stalls the ring's deadlines. */
ESP_SDR_GAPLESS_HOT int esp_sdr_gapless_run(const struct esp_sdr_gapless_cfg *cfg,
					    const struct esp_sdr_gapless_sink *sink,
					    struct esp_sdr_gapless_stats *stats)
{
	uint32_t start_probe[2], end_probe[2];
	uint32_t usage, saved, c0, ctrl1, lo, hi;
	uint32_t epoch, w0, expected, wi = 0, written = 0, t_win, t_restart, t_unit, t_service;
	uint64_t base;
	unsigned int b = 0, key;
	bool prepared = false, stop = false;
	struct est e0, e1;
	int ret = 0;

	if (cfg == NULL || sink == NULL || sink->reserve == NULL || sink->commit == NULL ||
	    sink->service == NULL || sink->stop == NULL || cfg->pre > PREPOST_MAX ||
	    cfg->post > PREPOST_MAX || cfg->headroom_lo > cfg->headroom_hi ||
	    cfg->headroom_hi >= 50U) {
		return -EINVAL;
	}
	if (!esp_sdr_ready) {
		return -EAGAIN;
	}
	if (esp_sdr_bank_usage(&usage) != 0 ||
	    (uintptr_t)__esp_sdr_bank_start != DUMP_BANK0_ADDR + DUMP_BANK_SIZE) {
		return -EINVAL;
	}
	lo = cfg->headroom_lo != 0U || cfg->headroom_hi != 0U ? cfg->headroom_lo : 5U;
	hi = cfg->headroom_lo != 0U || cfg->headroom_hi != 0U ? cfg->headroom_hi : 10U;
	k_mutex_lock(&esp_sdr_lock, K_FOREVER);
	(void)esp_sdr_rx_gain_refresh();
	run_init(cfg, sink, lo, hi);
	/* The machine performance counter (mpccr) counts CPU cycles. */
	__asm__ volatile("csrw 0x7e0, %0" ::"r"(1));
	__asm__ volatile("csrw 0x7e1, %0" ::"r"(1));
	fill_marks(0, 0, ESP_SDR_GAPLESS_RING);
	fill_marks(1, 0, ESP_SDR_GAPLESS_RING);

	c0 = sys_read32(IQE_CTRL0_REG);
	sys_write32((c0 & ~IQE_CTRL0_MODE) | FIELD_PREP(IQE_CTRL0_MODE, 1), IQE_CTRL0_REG);
	ctrl1 = (sys_read32(IQE_CTRL1_REG) &
		 ~(IQE_CTRL1_SRC | IQE_CTRL1_COUNT | IQE_CTRL1_START | IQE_CTRL1_EN)) |
		FIELD_PREP(IQE_CTRL1_SRC, 2) | FIELD_PREP(IQE_CTRL1_COUNT, IQE_COUNT_MAX - 1U);

	key = irq_lock();
	saved = esp_sdr_dump_arm(usage);
	/* As upstream: a pulse of the done bit, then the ring runs on block 1. */
	sys_write32(DUMP_CTRL_CIRCULAR | DUMP_CTRL_DONE, DUMP_CTRL_REG);
	sys_write32(DUMP_CTRL_CIRCULAR, DUMP_CTRL_REG);
	select_blocks(saved, BIT(0));
	sys_write32(DUMP_CTRL_CIRCULAR | DUMP_CTRL_RUN, DUMP_CTRL_REG);
	epoch = t_unit = t_service = cycles();
	w0 = write_index();
	/* Unit 0 starts near w0, found exactly after the first switch; index 0 is w0 - 256. */
	expected = (w0 - 256U) & RING_MASK;
	base = 0;
	est_restart(ctrl1);
	t_win = t_restart = cycles();
	est_read(&e0, expected, base);

	for (uint32_t unit = 0;; unit++) {
		unsigned int next = b ^ 1U;
		bool stop_checked = false;
		uint32_t busy = 0;

		for (;;) {
			uint32_t now, limit, room, budget, t;
			enum slice_result r;

			wi = write_index();
			written = (wi - expected) & RING_MASK;
			now = cycles();
			rs.st.polls++;
			if (unlikely(written >= THRESHOLD + LATE_LIMIT / 2U) &&
			    rs.st.overrun_activity == 0U) {
				/* What ran last, how long, with how much room: a budget it overran. */
				rs.st.overrun_activity = rs.last_activity + 1U;
				rs.st.overrun_cycles = now - rs.last_t;
				rs.st.overrun_room = rs.last_room;
			}
			if (unlikely(now - epoch > MAX_AGE_CYC)) {
				rs.st.error = ESP_SDR_GAPLESS_ERR_AGE;
				rs.st.detail = now - epoch;
				goto out;
			}
			if (written >= THRESHOLD) {
				if (!prepared) {
					/* Forced: the old unit's leftovers are lost. */
					prepare(next, expected, start_probe, end_probe);
					prepared = true;
					wi = write_index();
					written = (wi - expected) & RING_MASK;
				}
				break;
			}
			if (!stop_checked && written >= THRESHOLD / 2U) {
				stop_checked = true;
				stop = sink->stop(sink->user);
			}
			/* The next deadline: the switch, or else the preparation before it. */
			limit = prepared ? THRESHOLD : THRESHOLD - PREP_PAIRS - LATE_LIMIT;
			if (!prepared && (written >= limit || unit_done(&rs.u[next]))) {
				t = cycles();
				rs.last_activity = ESP_SDR_GAPLESS_PREP;
				rs.last_t = t;
				rs.last_room = 0U;
				prepare(next, expected, start_probe, end_probe);
				prepared = true;
				t = cycles() - t;
				busy += t;
				rs.ucyc[ESP_SDR_GAPLESS_PREP] += t;
				continue;
			}
			if (written + 64U >= limit) {
				continue;
			}
			room = (limit - written - 64U) * CPP;
			if (now - t_win >= WINDOW_CYC && room > rs.window_cyc_max + WINDOW_GUARD_CYC / 2U) {
				t = cycles();
				rs.last_activity = ESP_SDR_GAPLESS_WINDOWS;
				rs.last_t = t;
				rs.last_room = room;
				est_read(&e1, expected, base);
				t_win = now;
				window_done(&e0, &e1);
				if (now - t_restart >= RESTART_CYC) {
					est_restart(ctrl1);
					t_restart = now;
					est_read(&e1, expected, base);
				}
				e0 = e1;
				t = cycles() - t;
				busy += t;
				rs.ucyc[ESP_SDR_GAPLESS_WINDOWS] += t;
				rs.window_cyc_max = MAX(rs.window_cyc_max, t);
				continue;
			}
			/* Slices end by the deadline and at most SLICE_LATE after the next window. */
			t = now - t_win;
			budget = t < WINDOW_CYC + SLICE_LATE ? WINDOW_CYC + SLICE_LATE - t : 0U;
			budget = MIN(MIN(budget, room), SLICE_CYC_MAX);
			if (budget < SLICE_CYC_MIN) {
				continue;
			}
			t = cycles();
			rs.last_t = t;
			rs.last_room = room;
			rs.last_activity = ESP_SDR_GAPLESS_SWITCH;
			if (rs.gain_due != 0 && rs.cfg->gain_servo && gain_step(room)) {
				t = cycles() - t;
				busy += t;
				rs.ucyc[ESP_SDR_GAPLESS_SWITCH] += t;
				continue;
			}
			/*
			 * Copying first (its samples expire with their block), then packing; the link
			 * gets a turn when neither has work, and at least every SERVICE_CYC.
			 */
			rs.last_activity = ESP_SDR_GAPLESS_COPY;
			r = copy_slice(budget, base);
			if (r == SLICE_DONE) {
				t = cycles() - t;
				busy += t;
				rs.ucyc[ESP_SDR_GAPLESS_COPY] += t;
				continue;
			}
			if (now - t_service < SERVICE_CYC) {
				rs.last_activity = ESP_SDR_GAPLESS_PACK;
				r = pack_slice(budget);
				if (r == SLICE_DONE) {
					t = cycles() - t;
					busy += t;
					rs.ucyc[ESP_SDR_GAPLESS_PACK] += t;
					continue;
				}
			}
			/*
			 * service() cannot stop half way: it runs only where the longest call seen so
			 * far still fits before the deadline.
			 */
			if (room < rs.service_cyc + SLICE_CYC_MIN) {
				continue;
			}
			t_service = now;
			t = cycles();
			rs.last_activity = ESP_SDR_GAPLESS_SERVICE;
			bool did = sink->service(MIN(room - rs.service_cyc, budget), sink->user);

			t = cycles() - t;
			rs.service_cyc = MAX(rs.service_cyc, t + t / 4U);
			if (did) {
				busy += t;
				rs.ucyc[ESP_SDR_GAPLESS_SERVICE] += t;
				rs.st.service_cycles_max = MAX(rs.st.service_cycles_max, t);
			}
		}

		uint32_t late = written - THRESHOLD;
		uint32_t t_sw = cycles();

		rs.st.late_max = MAX(rs.st.late_max, late);
		if (late > LATE_LIMIT) {
			rs.st.error = ESP_SDR_GAPLESS_ERR_LATE;
			rs.st.detail = written;
			goto out;
		}
		if (stop) {
			sys_write32(DUMP_CTRL_CIRCULAR, DUMP_CTRL_REG);
			select_blocks(saved, 0U);
		} else {
			select_blocks(saved, BIT(next));
			epoch = cycles();
		}
		uint32_t t = cycles(), first, end, count;

		while (cycles() - t < SETTLE_CYC) {
		}
		if (unit == 0U) {
			first = find_first(b, (w0 - 1024U) & RING_MASK, UNIT0_GUARD);
			end = find_end(b, wi, wi, UNIT0_END_GUARD);
		} else {
			first = find_first(b, start_probe[b], START_GUARD);
			end = find_end(b, end_probe[b], wi, END_GUARD);
		}
		if (first == NOT_FOUND || (unit != 0U && first != expected)) {
			rs.st.error = ESP_SDR_GAPLESS_ERR_START;
			rs.st.detail = (first & 0xffffU) | (expected << 16);
			goto out;
		}
		if (end == NOT_FOUND) {
			rs.st.error = ESP_SDR_GAPLESS_ERR_END;
			rs.st.detail = wi;
			goto out;
		}
		count = (end - first) & RING_MASK;
		/* Unit 0 starts anywhere in its search window: its length varies by as much. */
		if (unit != 0U ? count < MIN_PAIRS || count > MAX_PAIRS
			       : count < MIN_PAIRS - 1024U || count > MAX_PAIRS + UNIT0_GUARD) {
			rs.st.error = ESP_SDR_GAPLESS_ERR_LENGTH;
			rs.st.detail = count;
			goto out;
		}
		rs.u[b] = (struct unit){.abs = base + ((first - expected) & RING_MASK),
					.first = first, .count = count, .valid = true};
		base += (end - expected) & RING_MASK;
		expected = end;
		rs.st.units++;
		rs.st.pairs += count;
		if (cfg->budget_bps != 0U) {
			thr_servo(count, lo, hi);
		}
		if (cfg->gain_servo) {
			gain_servo();
		}
		/* What this unit lost or left over sets the cap. */
		t = cycles();
		uint32_t idle = (t - t_unit) - MIN(busy, t - t_unit);

		rs.st.idle_cycles_min = MIN(rs.st.idle_cycles_min, idle);
		if (rs.unit_lost) {
			rs.cap = MAX(rs.cap - rs.cap / 8U, CAP_MIN);
		} else if (rs.unit_capped && idle > CAP_IDLE_CYC) {
			rs.cap = MIN(rs.cap + CAP_STEP, CAP_MAX);
		}
		rs.kept_prev = rs.kept_cur;
		rs.kept_cur = 0;
		rs.cur_abs = base;
		rs.unit_capped = false;
		rs.unit_lost = false;
		rs.ucyc[ESP_SDR_GAPLESS_SWITCH] += cycles() - t_sw;
		for (int k = 0; k < ESP_SDR_GAPLESS_ACTIVITIES; k++) {
			rs.st.cycles[k] += rs.ucyc[k];
		}
		if (rs.st_used > STAGE_WORDS / 2U) {
			rs.st.busy_units++;
			for (int k = 0; k < ESP_SDR_GAPLESS_ACTIVITIES; k++) {
				rs.st.busy_cycles[k] += rs.ucyc[k];
			}
		}
		memset(rs.ucyc, 0, sizeof(rs.ucyc));
		t_unit = t;
		prepared = false;
		b = next;
		if (stop) {
			break;
		}
	}
out:
	sys_write32(0, DUMP_CTRL_REG);
	esp_sdr_dump_disarm(saved);
	irq_unlock(key);
	rs.st.baseline = baseline_q8 >> 8;
	rs.st.ratio_x16 = rs.thr_q8 / 16U;
	rs.st.gain = (uint32_t)rs.gain;
	rs.st.cap = rs.cap;
	if (rs.st.error != ESP_SDR_GAPLESS_OK) {
		ret = -EIO;
	}
	if (stats != NULL) {
		*stats = rs.st;
	}
	k_mutex_unlock(&esp_sdr_lock);
	return ret;
}
