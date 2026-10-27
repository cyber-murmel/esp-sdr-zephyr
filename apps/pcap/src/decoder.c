/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The decoder thread (doc/pcap.md, "Decoders"): every closed burst through the decoders that
 * fit it (Bluetooth LE on a channel of its grid at 4 or 2 MS/s, IEEE 802.15.4 on a channel of
 * its grid at 4 MS/s, from one bin or two neighbouring ones), wideband bursts as burst records;
 * each packet goes out as a pcapng packet with its metadata. What no decoder claimed goes to the
 * generic analysis, a thread of its own at the lowest priority with a short queue: it takes the
 * time left over and never holds the decoders up; a burst it has no room for goes out as a
 * record of its metadata. Below the channelizer and USB in priority; with two cores both
 * threads are pinned (CONFIG_APP_PCAP_DECODE_CPU, beside the reader and USB by default). Floats
 * (FPU sharing), no PIE.
 */

#include <errno.h>
#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <esp_sdr/esp_sdr_dsp.h>

#include "app.h"
#include "decode.h"
#include "dec_util.h"
#include "esdr_pfbq.h"
#include "output.h"
#include "records.h"

LOG_MODULE_REGISTER(decoder, LOG_LEVEL_INF);

#define DEC_STACK 6144
#define DEC_PRIO  8
/* Items waiting: as many as there are bursts, and the C6's wideband runs. */
#define DEC_Q     (CONFIG_APP_PCAP_BURSTS + 16)
/* The decoders' scratch, PSRAM: the larger of dec_ble_scratch() and dec_wpan_scratch() of
 * BURST_MAX_SAMPLES (decoder_init() checks). */
#define SCRATCH   (136U * 1024U)
/* The generic analysis: time-sliced with the shell, bursts waiting for it at most (an eighth of
 * the pool: the rest stays free for new ones), samples of a burst's loud part it looks at (4 ms
 * at 2 MS/s), its scratch (dec_generic_scratch(BURST_MAX_SAMPLES)). */
#define GEN_STACK   4096
#define GEN_PRIO    K_LOWEST_APPLICATION_THREAD_PRIO
#define GEN_Q       (CONFIG_APP_PCAP_BURSTS / 8)
#define GEN_SAMPLES 8192U
#define GEN_SCRATCH (176U * 1024U)
/* Access addresses learnt from CONNECT_IND PDUs at most (the advertising one is extra). */
#define AA_LEARNT 4U

#define PSRAM __attribute__((section(".ext_ram.bss")))

enum item_kind {
	ITEM_BURST,
	ITEM_WIDE,
};

struct item {
	uint8_t kind;
	struct burst *b;
	struct wide_run w;
};

K_MSGQ_DEFINE(dec_q, sizeof(struct item), DEC_Q, 8);
static K_THREAD_STACK_DEFINE(dec_stack, DEC_STACK);
static struct k_thread dec_thread;
static uint8_t scratch[SCRATCH] __aligned(16) PSRAM;

K_MSGQ_DEFINE(gen_q, sizeof(struct burst *), GEN_Q, 4);
static K_THREAD_STACK_DEFINE(gen_stack, GEN_STACK);
static struct k_thread gen_thread;
static uint8_t gen_scratch[GEN_SCRATCH] __aligned(16) PSRAM;

static struct {
	/* The burst kept for a neighbour that may follow (802.15.4 between two bins). */
	struct burst *held;
	/* Access addresses: the host's (pinned) and the learnt ones (oldest replaced). */
	struct dec_ble_aa host[DEC_BLE_AA_MAX];
	uint8_t host_n;
	struct dec_ble_aa learnt[AA_LEARNT];
	uint8_t learnt_n, learnt_next;
	struct k_spinlock aa_lock;
	/* The burst being decoded, for the emit callbacks. */
	const struct burst *cur;
	bool claimed;
	uint8_t rec[REC_BURST_MAX + REC_BLE_MAX] __aligned(4);
	char comment[REC_COMMENT_MAX];
	/* A record of a burst's metadata. */
	struct dec_generic_out meta;
} dc;

/* The generic analysis's thread. */
static struct {
	uint8_t rec[REC_BURST_MAX] __aligned(4);
	struct dec_generic_out out;
} gen;

int decoder_put(struct burst *b)
{
	struct item it = {.kind = ITEM_BURST, .b = b};

	return k_msgq_put(&dec_q, &it, K_NO_WAIT) == 0 ? 0 : -ENOSPC;
}

int decoder_put_wide(const struct wide_run *w)
{
	struct item it = {.kind = ITEM_WIDE, .w = *w};

	return k_msgq_put(&dec_q, &it, K_NO_WAIT) == 0 ? 0 : -ENOSPC;
}

uint32_t decoder_backlog(void)
{
	return k_msgq_num_used_get(&dec_q);
}

/* ---- access addresses (PCAP_REQ_ADD_AA, CONNECT_IND) ---- */

int decoder_aa(uint16_t op, uint32_t aa, uint32_t crc_init)
{
	k_spinlock_key_t key = k_spin_lock(&dc.aa_lock);
	int ret = 0;

	if (op == 2U) {
		dc.host_n = 0;
		dc.learnt_n = 0;
	} else if (op == 0U) {
		if (dc.host_n < DEC_BLE_AA_MAX - 1U - AA_LEARNT) {
			dc.host[dc.host_n++] = (struct dec_ble_aa){.aa = aa, .crc_init = crc_init};
		} else {
			ret = -ENOSPC;
		}
	} else if (op == 1U) {
		for (unsigned int k = 0; k < dc.host_n; k++) {
			if (dc.host[k].aa == aa) {
				dc.host[k] = dc.host[--dc.host_n];
				break;
			}
		}
	} else {
		ret = -EINVAL;
	}
	k_spin_unlock(&dc.aa_lock, key);
	return ret;
}

static void aa_learn(uint32_t aa, uint32_t crc_init)
{
	k_spinlock_key_t key = k_spin_lock(&dc.aa_lock);

	for (unsigned int k = 0; k < dc.learnt_n; k++) {
		if (dc.learnt[k].aa == aa) {
			dc.learnt[k].crc_init = crc_init;
			k_spin_unlock(&dc.aa_lock, key);
			return;
		}
	}
	dc.learnt[dc.learnt_next] = (struct dec_ble_aa){.aa = aa, .crc_init = crc_init};
	dc.learnt_next = (uint8_t)((dc.learnt_next + 1U) % AA_LEARNT);
	dc.learnt_n = (uint8_t)MIN(dc.learnt_n + 1U, AA_LEARNT);
	k_spin_unlock(&dc.aa_lock, key);
}

static void ble_config(struct dec_ble_cfg *c, const struct burst *b)
{
	k_spinlock_key_t key = k_spin_lock(&dc.aa_lock);

	*c = (struct dec_ble_cfg){
		.max_errors = 2,
		.phy_2m = b->rate_hz == 4000000U,
		.keep_bad = (app_cfg.dec.flags & PCAP_DEC_BAD_CRC) != 0U,
	};
	c->aa[c->aa_count++] = (struct dec_ble_aa){.aa = DEC_BLE_ADV_AA,
						   .crc_init = DEC_BLE_ADV_CRC_INIT};
	for (unsigned int k = 0; k < dc.host_n && c->aa_count < DEC_BLE_AA_MAX; k++) {
		c->aa[c->aa_count++] = dc.host[k];
	}
	if ((app_cfg.dec.flags & PCAP_DEC_BLE_DATA) != 0U) {
		for (unsigned int k = 0; k < dc.learnt_n && c->aa_count < DEC_BLE_AA_MAX; k++) {
			c->aa[c->aa_count++] = dc.learnt[k];
		}
	}
	k_spin_unlock(&dc.aa_lock, key);
}

/* ---- packets out ---- */

/* The host time of sample @p s of burst @p b (s may be before its first). */
static uint64_t sample_ns(const struct burst *b, int64_t s)
{
	int64_t m = (int64_t)b->m0 + s;

	return timebase_ns(axis_ns(m > 0 ? (uint64_t)m : 0U, b->rate_hz));
}

static const char *comment(const struct burst *b, int32_t offset_hz, float power,
			   uint32_t duration_us, const char *phy)
{
	const struct rec_meta m = {
		.freq_hz = b->freq_hz,
		.offset_hz = offset_hz,
		.power_dbfs_x10 = dec_dbfs_x10(power),
		.floor_dbfs_x10 = dec_dbfs_x10(b->floor),
		.gain = b->gain,
		.channel = b->chan,
		.channels = b->channels,
		.rate_hz = b->rate_hz,
		.duration_us = duration_us,
		.phy = phy,
	};

	if ((app_cfg.dec.flags & PCAP_DEC_COMMENTS) == 0U ||
	    rec_comment(dc.comment, sizeof(dc.comment), &m) < 0) {
		return NULL;
	}
	return dc.comment;
}

static void ble_emit(const struct dec_ble_pkt *p, void *user)
{
	const struct burst *b = dc.cur;
	int ch = dec_ble_rf_channel(b->freq_hz);
	/* On air: the preamble (1 byte at LE 1M, 2 at LE 2M) and the bytes from the address on. */
	uint32_t us = p->phy == DEC_BLE_PHY_2M ? (2U + p->len) * 4U : (1U + p->len) * 8U;
	int n;

	ARG_UNUSED(user);
	if (p->crc_ok || !p->crc_checked) {
		dc.claimed = true;
	}
	if (p->conn_aa != 0U) {
		aa_learn(p->conn_aa, p->conn_crc_init);
	}
	n = rec_ble(dc.rec, sizeof(dc.rec), p, ch);
	if (n <= 0) {
		return;
	}
	(void)output_packet(PCAP_IF_BLE, sample_ns(b, p->start), dc.rec, (uint32_t)n,
			    comment(b, p->offset_hz, p->power, us,
				    p->phy == DEC_BLE_PHY_2M ? "LE 2M" : "LE 1M"));
}

static void wpan_emit(const struct dec_wpan_pkt *p, void *user)
{
	const struct burst *b = dc.cur;
	/* A combined pair carries the middle frequency. */
	uint64_t freq = p->combined ? b->freq_hz + b->rate_hz / 4U : b->freq_hz;
	int n;

	ARG_UNUSED(user);
	if (p->fcs_ok) {
		dc.claimed = true;
	}
	n = rec_wpan(dc.rec, sizeof(dc.rec), p, freq);
	if (n <= 0) {
		return;
	}
	(void)output_packet(PCAP_IF_WPAN, sample_ns(b, p->start), dc.rec, (uint32_t)n,
			    comment(b, p->offset_hz, p->power, (6U + p->len) * 32U, "O-QPSK"));
}

/* ---- bursts ---- */

/* Bins @p lo and @p hi neighbours whose middle is an 802.15.4 channel, overlapping in time. */
static bool wpan_pair(const struct burst *lo, const struct burst *hi)
{
	uint32_t spacing = lo->rate_hz / 2U;

	return lo->rate_hz == 4000000U && hi->rate_hz == lo->rate_hz && lo->n != 0U &&
	       hi->n != 0U && hi->freq_hz == lo->freq_hz + spacing &&
	       dec_wpan_channel(lo->freq_hz + spacing / 2U) != 0 && lo->m0 < hi->m0 + hi->n &&
	       hi->m0 < lo->m0 + lo->n;
}

static void decode_pair(struct burst *a, struct burst *b)
{
	const struct dec_wpan_cfg wc = {.keep_bad = (app_cfg.dec.flags & PCAP_DEC_BAD_CRC) != 0U};
	struct burst *lo = a->freq_hz < b->freq_hz ? a : b, *hi = lo == a ? b : a;

	if ((app_cfg.dec.flags & PCAP_DEC_WPAN) == 0U || !wpan_pair(lo, hi)) {
		return;
	}
	dc.cur = lo;
	(void)dec_wpan_run_pair(lo, hi, &wc, scratch, sizeof(scratch), wpan_emit, NULL);
}

static void decode_wideband(const struct burst *b)
{
	uint32_t spacing = b->rate_hz / 2U;
	int lo = b->wide_lo <= b->channels / 2U ? b->wide_lo : (int)b->wide_lo - b->channels;
	int hi = b->wide_hi <= b->channels / 2U ? b->wide_hi : (int)b->wide_hi - b->channels;
	struct pcap_burst_hdr h = {
		.flags = PCAP_BF_WIDEBAND,
		.freq_hz = b->freq_hz,
		.bandwidth_hz = (uint32_t)(hi - lo + 1) * spacing,
		.duration_us = (uint32_t)(axis_ns(b->loud_to, b->rate_hz) / 1000U),
		.modulation = PCAP_MOD_WIDEBAND,
		.rx_gain = b->gain,
		.channel = (uint8_t)b->wide_lo,
		.channels = (uint8_t)b->channels,
		.power_dbfs_x10 = dec_dbfs_x10(b->power),
		.floor_dbfs_x10 = dec_dbfs_x10(b->floor),
		.peak_dbfs_x10 = dec_dbfs_x10(b->power),
		.sample_rate_hz = b->rate_hz,
	};
	int n;

	if ((app_cfg.dec.flags & PCAP_DEC_WIDEBAND) == 0U) {
		return;
	}
	n = rec_burst(dc.rec, sizeof(dc.rec), &h, NULL);
	if (n > 0) {
		(void)output_packet(PCAP_IF_BURST, sample_ns(b, 0), dc.rec, (uint32_t)n, NULL);
	}
}

/* Width records come at 80 MS/s only: the C6 does not sort at 40 (main.c, start_mode()). */
static void decode_wide_run(const struct wide_run *w)
{
	/* The mean frequency: the angle of the records' lag-one correlations, 80 MHz a turn. */
	float turn = atan2f(w->r1_q, w->r1_i) / ESP_SDR_TAU_F;
	struct pcap_burst_hdr h = {
		.flags = PCAP_BF_WIDEBAND,
		.freq_hz = (uint64_t)((int64_t)w->centre_hz +
				      (int64_t)lrintf(turn * (float)ESDR_GAPLESS_RATE_HZ)),
		.duration_us = (uint32_t)((w->end - w->index) / (ESDR_GAPLESS_RATE_HZ / 1000000U)),
		.modulation = PCAP_MOD_WIDEBAND,
		.rx_gain = w->gain,
		.power_dbfs_x10 = w->power_dbfs_x10,
		/* The C6 judged the samples; the P4 never saw them: no floor, no peak. */
		.floor_dbfs_x10 = INT16_MIN,
		.peak_dbfs_x10 = w->power_dbfs_x10,
		.sample_rate_hz = ESDR_GAPLESS_RATE_HZ,
	};
	int n;

	if ((app_cfg.dec.flags & PCAP_DEC_WIDEBAND) == 0U) {
		return;
	}
	n = rec_burst(dc.rec, sizeof(dc.rec), &h, NULL);
	if (n > 0) {
		(void)output_packet(PCAP_IF_BURST, timebase_ns(w->index * 25U / 2U), dc.rec,
				    (uint32_t)n, NULL);
	}
}

/*
 * A look at the loud part, at up to GATE_POINTS points: the power above the floor over the
 * floor, and the correlation one sample apart over the power above the floor (narrow: 0 without
 * power above it). In the channel mode (2 MS/s, the C6's boxcar) Bluetooth LE keeps narrow
 * above 0.7, a band filling the channel (a Wi-Fi edge) near 0: GATE_NARROW. A bank's bin,
 * oversampled twice, correlates a band filling it to some 0.7 (simulated through the prototype:
 * 0.69 to 0.72; Wi-Fi measured 0.6 to 0.9), LE 2M at 4 MS/s and LE 1M at 2 MS/s (2 samples a
 * symbol) to 0.78 to 0.88: GATE_NARROW_BANK. LE 1M at 4 MS/s keeps 0.95 (simulated; all 437
 * packets of a 40 MS/s survey above 0.9, 58 % of the 25000 other bursts below 0.85, and they
 * held most of the BLE decoder's time): below GATE_NARROW_1M a 4 MS/s burst gets the LE 2M
 * pass only, from GATE_NARROW_2M_MAX on (LE 2M keeps 0.88 at most) the LE 1M pass only.
 */
#define GATE_POINTS        256U
#define GATE_NARROW        0.45f
#define GATE_NARROW_BANK   0.75f
#define GATE_NARROW_1M     0.85f
#define GATE_NARROW_2M_MAX 0.92f
/*
 * Less narrow than this and clearly above the noise: a band filling the channel, nothing to
 * analyse (the analysis finds such bursts unknown or wideband; Bluetooth LE keeps 0.7 and more).
 */
#define GATE_WIDE   0.5f
#define GATE_SNR    1.0f

struct gate {
	float snr, narrow;
};

static struct gate gate_look(const struct burst *b)
{
	uint32_t from = b->loud_to > b->loud_from ? b->loud_from : 0U;
	uint32_t to = b->loud_to > b->loud_from ? b->loud_to : b->n;
	uint32_t step = MAX((to - from) / GATE_POINTS, 1U);
	float r0 = 0.0f, ri = 0.0f, rq = 0.0f, sig;
	uint32_t pts = 0;
	struct gate g = {0};

	for (uint32_t k = MAX(from, 1U); k < to; k += step, pts++) {
		float i0 = b->iq[2U * k], q0 = b->iq[2U * k + 1U];
		float i1 = b->iq[2U * k - 2U], q1 = b->iq[2U * k - 1U];

		r0 += i0 * i0 + q0 * q0;
		ri += i0 * i1 + q0 * q1;
		rq += q0 * i1 - i0 * q1;
	}
	sig = r0 - b->floor * (float)pts;
	if (sig > 0.0f && pts > 0U) {
		g.snr = b->floor > 0.0f ? sig / (b->floor * (float)pts) : 1e9f;
		g.narrow = sqrtf(ri * ri + rq * rq) / sig;
	}
	return g;
}

/* gate_look() once per burst. */
static struct gate gate(struct burst *b)
{
	if ((b->flags & BURST_F_GATED) == 0U) {
		struct gate g = gate_look(b);

		b->gate_snr = g.snr;
		b->gate_narrow = g.narrow;
		b->flags |= BURST_F_GATED;
	}
	return (struct gate){.snr = b->gate_snr, .narrow = b->gate_narrow};
}

/* On air, at LE 1M: an empty PDU 80 us, the longest 2120 us; LE 2M is shorter. */
#define BLE_US_MIN 64U
#define BLE_US_MAX 2400U
/* An 802.15.4 ACK is 352 us, the longest frame 4256 us. */
#define WPAN_US_MIN 300U
#define WPAN_US_MAX 4500U

static void decode(struct burst *b)
{
	const uint32_t flags = app_cfg.dec.flags;
	uint32_t loud = b->loud_to > b->loud_from ? b->loud_to - b->loud_from : b->n;
	uint32_t loud_us = (uint32_t)(axis_ns(loud, b->rate_hz) / 1000U);
	/* A burst cut at either end may hold part of a packet longer than its loud part (one
	 * shorter than the shortest packet holds none). */
	bool cut = (b->flags & (BURST_F_TRUNCATED | BURST_F_CUT | BURST_F_DROPPED)) != 0U;

	dc.cur = b;
	dc.claimed = false;
	if ((flags & PCAP_DEC_BLE) != 0U && (b->rate_hz == 4000000U || b->rate_hz == 2000000U) &&
	    dec_ble_rf_channel(b->freq_hz) >= 0 &&
	    loud_us >= BLE_US_MIN && (cut || loud_us <= BLE_US_MAX) &&
	    gate(b).narrow >= (b->channels == 1U ? GATE_NARROW : GATE_NARROW_BANK)) {
		struct dec_ble_cfg bc;

		ble_config(&bc, b);
		bc.skip_1m = b->rate_hz == 4000000U && gate(b).narrow < GATE_NARROW_1M;
		bc.phy_2m = bc.phy_2m && gate(b).narrow < GATE_NARROW_2M_MAX;
		(void)dec_ble_run(b, &bc, scratch, sizeof(scratch), ble_emit, NULL);
	}
	if ((flags & PCAP_DEC_WPAN) != 0U && (b->rate_hz == 4000000U || b->rate_hz == 2000000U) &&
	    dec_wpan_channel(b->freq_hz) != 0 &&
	    loud_us >= WPAN_US_MIN && (cut || loud_us <= WPAN_US_MAX)) {
		const struct dec_wpan_cfg wc = {.keep_bad = (flags & PCAP_DEC_BAD_CRC) != 0U};

		(void)dec_wpan_run(b, &wc, scratch, sizeof(scratch), wpan_emit, NULL);
	}
	if (dc.claimed) {
		b->flags |= BURST_F_CLAIMED;
	}
}

/* A record of @p b's metadata alone: modulation @p mod, PCAP_BF_* @p flags added. */
static void record_meta(const struct burst *b, uint8_t mod, uint16_t flags)
{
	struct burst v = *b;
	int n;

	v.n = 0U;
	v.iq = NULL;
	if (dec_generic_run(&v, NULL, NULL, 0U, &dc.meta) != 0) {
		return;
	}
	if (mod != PCAP_MOD_UNKNOWN) {
		dc.meta.hdr.modulation = mod;
	}
	dc.meta.hdr.flags |= flags;
	n = rec_burst(dc.rec, sizeof(dc.rec), &dc.meta.hdr, NULL);
	if (n > 0) {
		(void)output_packet(PCAP_IF_BURST, sample_ns(b, dc.meta.start), dc.rec, (uint32_t)n,
				    NULL);
	}
}

/*
 * The decoder thread is done with @p b: back to the pool, or to the generic analysis when no
 * decoder claimed it (a band filling the channel goes out as a wideband record, a burst the
 * analysis has no room for as a record of its metadata).
 */
static void done(struct burst *b)
{
	uint32_t min_us = app_cfg.dec.min_burst_us != 0U ? app_cfg.dec.min_burst_us : 20U;
	uint32_t loud = b->loud_to > b->loud_from ? b->loud_to - b->loud_from : b->n;
	struct gate g;

	if ((b->flags & BURST_F_CLAIMED) != 0U || (app_cfg.dec.flags & PCAP_DEC_GENERIC) == 0U ||
	    b->n == 0U || axis_ns(loud, b->rate_hz) < 1000U * min_us) {
		burst_put(b);
		return;
	}
	g = gate(b);
	if (g.snr >= GATE_SNR && g.narrow < GATE_WIDE) {
		record_meta(b, PCAP_MOD_WIDEBAND, 0U);
		app_stats.generic_wide++;
		burst_put(b);
		return;
	}
	if (k_msgq_put(&gen_q, &b, K_NO_WAIT) != 0) {
		record_meta(b, PCAP_MOD_UNKNOWN, PCAP_BF_SKIPPED);
		app_stats.generic_skipped++;
		burst_put(b);
	}
}

static void handle(struct item *it)
{
	struct burst *b = it->b;
	uint64_t t0 = esdr_pfbq_cycles();

	if (it->kind == ITEM_WIDE) {
		decode_wide_run(&it->w);
	} else if ((b->flags & BURST_F_WIDEBAND) != 0U) {
		decode_wideband(b);
		burst_put(b);
	} else {
		decode(b);
		/* The burst before may be this one's neighbour: 802.15.4 between their bins. */
		if (dc.held != NULL) {
			decode_pair(dc.held, b);
			done(dc.held);
			dc.held = NULL;
		}
		if (b->rate_hz == 4000000U && b->channels > 1U) {
			dc.held = b;
		} else {
			done(b);
		}
	}
	app_stats.decoded++;
	app_stats.dec_cycles += esdr_pfbq_cycles() - t0;
}

static void dec_fn(void *a, void *b, void *c)
{
	struct item it;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		if (k_msgq_get(&dec_q, &it, K_MSEC(OUTPUT_FLUSH_MS / 2)) == 0) {
			handle(&it);
		} else if (dc.held != NULL) {
			/* Nothing came: the held burst has no neighbour. */
			done(dc.held);
			dc.held = NULL;
		}
		output_poll();
	}
}

/* ---- the generic analysis ---- */

static void analyse(const struct burst *b)
{
	const struct dec_generic_cfg gc = {
		.max_bits = app_cfg.dec.max_bits != 0U ? app_cfg.dec.max_bits : PCAP_BURST_BITS_MAX,
		.max_samples = GEN_SAMPLES,
	};
	int n;

	if (dec_generic_run(b, &gc, gen_scratch, sizeof(gen_scratch), &gen.out) != 0) {
		return;
	}
	n = rec_burst(gen.rec, sizeof(gen.rec), &gen.out.hdr, gen.out.bits);
	if (n > 0) {
		(void)output_packet(PCAP_IF_BURST, sample_ns(b, gen.out.start), gen.rec, (uint32_t)n,
				    NULL);
	}
}

static void gen_fn(void *a, void *b, void *c)
{
	struct burst *bu;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		uint64_t t0;

		(void)k_msgq_get(&gen_q, &bu, K_FOREVER);
		t0 = esdr_pfbq_cycles();
		analyse(bu);
		burst_put(bu);
		app_stats.generic++;
		app_stats.generic_cycles += esdr_pfbq_cycles() - t0;
	}
}

void decoder_init(void)
{
	size_t need = MAX(dec_ble_scratch(BURST_MAX_SAMPLES), dec_wpan_scratch(BURST_MAX_SAMPLES));

	if (need > sizeof(scratch) || dec_generic_scratch(BURST_MAX_SAMPLES) > sizeof(gen_scratch)) {
		LOG_ERR("decoders need %u and %u bytes of scratch, %u and %u at hand",
			(unsigned int)need, (unsigned int)dec_generic_scratch(BURST_MAX_SAMPLES),
			(unsigned int)sizeof(scratch), (unsigned int)sizeof(gen_scratch));
	}
	k_thread_create(&dec_thread, dec_stack, K_THREAD_STACK_SIZEOF(dec_stack), dec_fn, NULL,
			NULL, NULL, DEC_PRIO, 0, K_FOREVER);
	k_thread_name_set(&dec_thread, "decoder");
	k_thread_create(&gen_thread, gen_stack, K_THREAD_STACK_SIZEOF(gen_stack), gen_fn, NULL,
			NULL, NULL, GEN_PRIO, 0, K_FOREVER);
	k_thread_name_set(&gen_thread, "generic");
#if defined(CONFIG_SMP)
	/* Pinned, so the cycle counts (mcycle, per core) stay on one core. */
	if (k_thread_cpu_pin(&dec_thread, CONFIG_APP_PCAP_DECODE_CPU) != 0 ||
	    k_thread_cpu_pin(&gen_thread, CONFIG_APP_PCAP_DECODE_CPU) != 0) {
		LOG_ERR("decoder threads not pinned");
	}
#endif
	k_thread_start(&dec_thread);
	k_thread_start(&gen_thread);
}
