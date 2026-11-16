/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * 1 Msym/s GFSK demodulator, see gfsk_demod.h for the design.
 *
 * Indexes are sample numbers of the 4 MS/s stream (wrapping at 2^32). The FIR's output z and the
 * discriminator are computed ahead for a stretch of the push (the plane zp; the vector unit on the
 * S3). The full path works in groups of 4 samples starting at a multiple of 4 (one symbol period):
 * the sample loop (hot()) adds the discriminator into its running sum in a ring and takes the
 * power of each group, scaled to the reference's int16 values; run_groups() then does the per
 * group logic (detector, burst edge, the decision cursor). The closed path reads the plane's z.
 */

#include <string.h>

#include "gfsk_demod.h"
#include "gfsk_demod_off.h"
#include "ubt_attr.h"

#if defined(__ZEPHYR__) && defined(CONFIG_SOC_SERIES_ESP32S3)
#include <zephyr/kernel.h>

#include <esp_sdr/esp_sdr_xtensa.h>

#define GFSK_PIE 1
/* gfsk_pie.S */
void gfsk_pie_u(const int8_t *x, int16_t *u, uint32_t nblk);
void gfsk_pie_z(int16_t *zi, int16_t *zq, uint32_t nblk);
void gfsk_pie_e(const int16_t *zi, const int16_t *zq, int32_t *e, uint32_t nvec);
#endif

/* The reference's parameters (gfsk_ref.c; tuning runs there, GFSK_REF_TUNABLE). */
#define WIN       2
#define DD_K      5
#define NSEED     4
#define SEED_SKIP 9
#define EL_PERIOD 32
#define EL_SHIFT  2
#define SQ_SHIFT  2
#define TOFF      3
#define FLOOR_UP  12

#ifdef GFSK_PROF
struct gfsk_prof gfsk_prof;
static inline uint32_t prof_now(void)
{
	uint32_t c;

	__asm__ volatile("rsr.ccount %0" : "=a"(c));
	return c;
}
#define PROF_T(v) uint32_t v = prof_now()
#define PROF_ADD(field, t0) (gfsk_prof.field += prof_now() - (t0))
#else
#define PROF_T(v)
#define PROF_ADD(field, t0)
#endif

#define RM (GFSK_RING - 1U)
#define PM (GFSK_PRING - 1U)

/* Groups without signal before a rise counts as a new burst (two groups up confirm it). */
#define IDLE_RESET 8U
/* The cursor stays this many samples ahead of the burst start estimate. */
#define LEAD_IN 8U
/* Lock threshold over the tracked noise floor: x6, 7.8 dB. */
#define FLOOR_MUL 6U
/* Power compensation for the boxcar's roll-off at the deviation, in pwr counts. */
#define PWR_COMP_BLE 1
#define PWR_COMP_BR  0

_Static_assert((GFSK_RING & (GFSK_RING - 1U)) == 0U &&
		       GFSK_RING >= 4U * (GFSK_LAT_SYMS + GFSK_CHUNK) + 12U,
	       "ring holds the look-ahead and a chunk");
_Static_assert((GFSK_PRING & (GFSK_PRING - 1U)) == 0U &&
		       GFSK_PRING >= GFSK_LAT_SYMS + GFSK_CHUNK + 4U,
	       "power ring holds the look-ahead and a chunk");
/*
 * The reference counted its history up to 64 samples; the clamp in front_start() never binds
 * further back. The replay reaches 42 samples before the base of a push and its FIR 5 more.
 */
#define GFSK_HIST_MAX 64U
_Static_assert(GFSK_PRE_SAMPLES % 4U == 0U, "the replay is whole groups");

/* The offsets the assembly uses (gfsk_demod_off.h). */
#define GD_AT(field, off) (offsetof(struct gfsk_demod, field) == (off))
_Static_assert(GD_AT(mod, GD_MOD) && GD_AT(always, GD_ALWAYS) && GD_AT(pwr_thr, GD_PWR_THR) &&
		       GD_AT(hold, GD_HOLD) && GD_AT(s_close, GD_S_CLOSE) && GD_AT(f, GD_F) &&
		       GD_AT(lg, GD_LG) && GD_AT(replay_end, GD_REPLAY_END) &&
		       GD_AT(savg, GD_SAVG) && GD_AT(floor, GD_FLOOR) && GD_AT(s_gate, GD_S_GATE) &&
		       GD_AT(hold_cnt, GD_HOLD_CNT) && GD_AT(s_init, GD_S_INIT) &&
		       GD_AT(off, GD_OFF) && GD_AT(run, GD_RUN) && GD_AT(pw_c, GD_PW_C) &&
		       GD_AT(pend, GD_PEND) && GD_AT(sbase, GD_SBASE) && GD_AT(edge, GD_EDGE) &&
		       GD_AT(ph, GD_PH) && GD_AT(seeded, GD_SEEDED) && GD_AT(cur, GD_CUR) &&
		       GD_AT(cur_valid, GD_CUR_VALID) && GD_AT(sliced, GD_SLICED) &&
		       GD_AT(pring, GD_PRING) && GD_AT(wring, GD_WRING) &&
		       GD_AT(el_cnt, GD_EL_CNT) && GD_AT(el_e, GD_EL_E) && GD_AT(el_a, GD_EL_A) &&
		       GD_AT(m0, GD_M0) && GD_AT(m1, GD_M1) && GD_AT(cring, GD_CRING),
	       "gfsk_demod_off.h matches struct gfsk_demod");
_Static_assert(sizeof(((struct gfsk_demod *)0)->hold) == 2U &&
		       sizeof(((struct gfsk_demod *)0)->hold_cnt) == 2U &&
		       sizeof(((struct gfsk_demod *)0)->run) == 1U &&
		       sizeof(((struct gfsk_demod *)0)->s_init) == 1U &&
		       sizeof(((struct gfsk_demod *)0)->el_cnt) == 1U &&
		       sizeof(((struct gfsk_demod *)0)->ph) == 1U,
	       "the field sizes the assembly uses");
/*
 * The lowest sample a path reads z of is 2 before the replay's first, which is 42 before the base
 * of a push (the closed scan starts at most 3 before it); the plane's block holding it starts up
 * to 15 before that, and the vector kernel reads the 16 samples before the block.
 */
_Static_assert(GFSK_IN_HIST >= GFSK_PRE_SAMPLES + 2U + 2U + 15U + 16U,
	       "input history covers the plane");

/*
 * The FIR's output for a stretch of the push, computed ahead of the paths that read it: z of the
 * stream samples [zb, ze) in int8 units, i[ZP_HEAD + k] = z_i of sample zb + k (zb a multiple of
 * 16). The vector unit computes it in blocks of 16 (gfsk_pie.S, its scratch in the first ZP_HEAD
 * entries), elsewhere C the same values. A path asking for samples outside it computes it anew:
 * once a push in the modes, again for the replay when the squelch opens. The full path also
 * takes the discriminator of the plane's samples, e (computed on the first request after a
 * fill). One plane for every demodulator: pushes do not run concurrently.
 */
#define ZP_HEAD 8U
#define ZP_LEN  320U
static struct {
	int16_t i[ZP_HEAD + ZP_LEN] __attribute__((aligned(16)));
	int16_t q[ZP_HEAD + ZP_LEN] __attribute__((aligned(16)));
	/* e[k]: Im(z z[n-2]*) x 256 of sample zb + k (k >= 2), the reference's per sample step. */
	int32_t e[ZP_LEN] __attribute__((aligned(16)));
	uint32_t zb, ze;
	bool e_ok;
} zp ESP_SDR_DATA_F(STATE, zp, NOINIT);

/* log2(1 + (m + 0.5) / 64) * 256 */
/* gfsk_groups.S reads it too. */
const uint8_t gfsk_log2_lut[64] ESP_SDR_DATA(TABLE, gfsk_log2_lut) = {
	3,   9,   14,  20,  25,  30,  36,  41,  46,  51,  56,  61,  66,  71,  75,  80,
	85,  89,  94,  98,  103, 107, 111, 116, 120, 124, 128, 132, 136, 140, 144, 148,
	152, 155, 159, 163, 167, 170, 174, 178, 181, 185, 188, 192, 195, 198, 202, 205,
	208, 212, 215, 218, 221, 224, 228, 231, 234, 237, 240, 243, 246, 249, 252, 255,
};

/* 10^(r / 20) * 2^15 */
static const uint32_t pow10_lut[20] = {
	32768,  36781,  41285,  46341,  52016,  58386,  65536,  73562,  82570,  92682,
	104032, 116772, 131072, 147123, 165140, 185364, 208064, 233544, 262144, 294245,
};

uint8_t gfsk_pwr_from_s(uint32_t s)
{
	unsigned int lz;
	uint32_t l;
	int32_t p;

	if (s == 0U) {
		return 0;
	}
	lz = (unsigned int)__builtin_clz(s);
	/* log2(s) in Q8: the exponent and the six bits after the leading one. */
	l = ((31U - lz) << 8) + gfsk_log2_lut[((s << lz) >> 25) & 63U];
	/* 20 log10(s) + 25.4 = 6.0206 log2(s) + 25.4 in half dB steps relative to s = 2^29. */
	p = (int32_t)((l * 1541U + 1664780U + 32768U) >> 16);
	return (uint8_t)(p > 255 ? 255 : p);
}

uint32_t gfsk_s_from_pwr(uint8_t pwr)
{
	int32_t v = (int32_t)pwr - 200;
	int32_t qd = v >= 0 ? v / 20 : -((-v + 19) / 20);
	uint64_t s = (uint64_t)pow10_lut[v - 20 * qd] << 14;

	for (; qd > 0; qd--) {
		s *= 10U;
	}
	for (; qd < 0; qd++) {
		s /= 10U;
	}
	return s > UINT32_MAX ? UINT32_MAX : (uint32_t)s;
}

static inline uint8_t pwr_of(const struct gfsk_demod *d, uint32_t s)
{
	int p = gfsk_pwr_from_s(s) + (d->mod == GFSK_BLE ? PWR_COMP_BLE : PWR_COMP_BR);

	return (uint8_t)(p > 255 ? 255 : p);
}

struct emit {
	struct gfsk_sym *out;
	size_t cap, n;
};

static inline void emit_sym(struct emit *em, uint32_t t, unsigned int bit, uint8_t pwr)
{
	if (em->n < em->cap) {
		em->out[em->n].t = t;
		em->out[em->n].bit = (uint8_t)bit;
		em->out[em->n].pwr = pwr;
		em->n++;
	}
}

static inline uint32_t min_u32(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

static inline uint32_t align_up4(uint32_t x)
{
	return (x + 3U) & ~3U;
}

/* Smallest index >= x whose low two bits are ph. */
static inline uint32_t align_ph(uint32_t x, unsigned int ph)
{
	return x + ((ph - x) & 3U);
}

void gfsk_demod_init(struct gfsk_demod *d, enum gfsk_modulation mod)
{
	memset(d, 0, sizeof(*d));
	d->mod = (uint8_t)mod;
	d->always = 1;
	d->hold = 1;
	gfsk_demod_reset(d);
}

void gfsk_demod_reset(struct gfsk_demod *d)
{
	/* The configuration sits before n in the struct. */
	size_t keep = offsetof(struct gfsk_demod, n);

	memset((uint8_t *)d + keep, 0, sizeof(*d) - keep);
	d->ph = 3;
	d->open = d->always ? 1U : 0U;
	d->hold_cnt = d->hold;
	/*
	 * The squelch's gate is configuration (without a squelch it follows the floor from the
	 * first group on). Zeroed with the stream, every group passed it after a retune: each
	 * burst's edge fell on the first group of the replay, its phase and levels came from noise.
	 */
	d->s_gate = d->s_thr >> 1;
}

void gfsk_demod_set_squelch(struct gfsk_demod *d, uint8_t pwr_threshold, uint16_t hold_syms,
			    bool always_emit)
{
	d->pwr_thr = pwr_threshold;
	d->hold = hold_syms < 1U ? 1U : hold_syms;
	d->always = (always_emit || pwr_threshold == 0U) ? 1U : 0U;
	if (pwr_threshold == 0U) {
		d->s_thr = d->s_close = d->s_gate = 0;
	} else {
		d->s_thr = gfsk_s_from_pwr(pwr_threshold);
		d->s_close = d->s_thr >> 1;
		d->s_gate = d->s_thr >> 1;
	}
	if (d->always && !d->open) {
		d->open = 1;
		d->front_ok = 0;
	}
	d->hold_cnt = d->hold;
}

uint8_t gfsk_demod_level(const struct gfsk_demod *d)
{
	return pwr_of(d, d->savg);
}

uint8_t gfsk_demod_noise_floor(const struct gfsk_demod *d)
{
	return pwr_of(d, d->floor);
}

int32_t gfsk_demod_cfo_hz(const struct gfsk_demod *d)
{
	/* The mean deviation of a modulated carrier is about 0.9 of the peak. */
	int32_t dev_hz = d->mod == GFSK_BLE ? 225000 : 144000;
	int32_t half = (d->m0 >> 1) - (d->m1 >> 1), mid = (d->m0 >> 1) + (d->m1 >> 1);

	if (!d->seeded || half <= 0) {
		return 0;
	}
	return (int32_t)(-((int64_t)dev_hz * mid) / half);
}

uint32_t gfsk_demod_horizon(const struct gfsk_demod *d)
{
	if (d->open && d->cur_valid) {
		return d->cur - (uint32_t)TOFF - 2U;
	}
	return d->f - GFSK_PRE_SAMPLES - 8U;
}

/* ---------------------------------------------------------------------------------------- */
/* Sampling phase, slicing                                                                  */
/* ---------------------------------------------------------------------------------------- */

/* Window sum of the discriminator ending at index c. */
static inline int32_t wsum(const struct gfsk_demod *d, uint32_t c)
{
	return (int32_t)(d->cring[c & RM] - d->cring[(c - (uint32_t)WIN) & RM]);
}

/*
 * A burst starts at about d->edge. Every packet begins with alternating symbols (the
 * preamble, and for BR its first sync bit), so the window sums of consecutive symbols differ by
 * twice the eye opening at the right phase and by less at the others: the largest difference
 * sets the phase. The mean of the sums at that phase is the middle between the two levels
 * whatever the carrier offset, and their mean distance from it the distance to each level.
 */
/* Once per burst: in flash (noinline keeps it out of the IRAM slicer). */
ESP_SDR_CODE(EVENT, acquire)
static __attribute__((noinline)) void acquire(struct gfsk_demod *d)
{
	uint32_t g0 = d->edge >> 2, g, from, best_a = 0, c0, pf, half;
	unsigned int best = 0;
	int32_t mean = 0, dev = 0;
	int n = (d->mod == GFSK_BLE ? NSEED : NSEED - 1), w[8];

	/* The burst started at the first group with half the power it settles at. */
	pf = (d->pring[(g0 + 3U) & PM] >> 2) + (d->pring[(g0 + 4U) & PM] >> 2) +
	     (d->pring[(g0 + 5U) & PM] >> 2) + (d->pring[(g0 + 6U) & PM] >> 2);
	half = pf >> 1;
	for (g = g0 - 1U; g != g0 + 3U && d->pring[g & PM] < half; g++) {
	}
	from = (g << 2) + (uint32_t)SEED_SKIP;
	if ((int32_t)(from - d->start - 4U) < 0) {
		from = d->start + 4U;
	}
	n = n > 8 ? 8 : n;
	for (unsigned int p = 0; p < 4U; p++) {
		uint32_t a = 0, c = align_ph(from, p);
		int32_t prev = wsum(d, c);

		for (int j = 1; j < n; j++) {
			int32_t x = wsum(d, c + 4U * (uint32_t)j);
			int32_t dx = x - prev;

			a += (uint32_t)(dx < 0 ? -dx : dx) >> 4;
			prev = x;
		}
		if (a > best_a) {
			best_a = a;
			best = p;
		}
	}
	d->ph = (uint8_t)best;
	c0 = align_ph(from, best);
	for (int j = 0; j < n; j++) {
		w[j] = wsum(d, c0 + 4U * (uint32_t)j);
		mean += w[j] / n;
	}
	for (int j = 0; j < n; j++) {
		int32_t x = w[j] - mean;

		dev += (x < 0 ? -x : x) / n;
	}
	d->m0 = mean + dev;
	d->m1 = mean - dev;
	d->el_e = 0;
	d->el_a = 0;
	d->el_cnt = (uint8_t)EL_PERIOD;
	d->seeded = 1;
}

/*
 * The symbols at the cursor while its window ends at or before @p last: per symbol the window
 * sum against the middle of the two levels, the level the decision belongs to follows it, and
 * (while @p early: the samples after the window are there) the eye opening one sample later
 * against one sample earlier moves the sampling phase when it is clearly better on one side.
 */
UBT_HOT(SAMPLE, slice_c)
static void slice_c(struct gfsk_demod *d, struct emit *em, uint32_t last, bool early)
{
	uint32_t cur = d->cur;
	unsigned int ph = d->ph;
	int32_t m0 = d->m0, m1 = d->m1, thr = (m0 >> 1) + (m1 >> 1);
	unsigned int cnt = d->el_cnt;
	int32_t el_e = d->el_e;
	uint32_t el_a = d->el_a, en = em->n;
	const uint32_t *cr = d->cring;
	const uint8_t *wr = d->wring;
	struct gfsk_sym *out = em->out;

	while ((int32_t)(last - cur) >= 0) {
		int32_t w;
		unsigned int bit, sh;
		uint8_t pw;

		if (!d->seeded && (int32_t)(cur + LEAD_IN - d->edge) >= 0) {
			d->m0 = m0;
			d->m1 = m1;
			acquire(d);
			ph = d->ph;
			m0 = d->m0;
			m1 = d->m1;
			cnt = d->el_cnt;
			el_e = 0;
			el_a = 0;
			thr = (m0 >> 1) + (m1 >> 1);
		}
		/* Align to the sampling phase: a step of -1 to +2 samples. */
		sh = (ph - cur) & 3U;
		cur = sh == 3U ? cur - 1U : cur + sh;
		w = (int32_t)(cr[cur & RM] - cr[(cur - (uint32_t)WIN) & RM]);
		/* e = -|z|^2 sin(theta): a positive deviation is negative. */
		bit = w < thr ? 1U : 0U;
		if (bit != 0U) {
			m1 += (w - m1) >> DD_K;
		} else {
			m0 += (w - m0) >> DD_K;
		}
		if (early && EL_PERIOD != 0) {
			/* The eye opening one sample later against one sample earlier. */
			int32_t wl = (int32_t)(cr[(cur + 1U) & RM] - cr[(cur + 1U - (uint32_t)WIN) & RM]);
			int32_t we = (int32_t)(cr[(cur - 1U) & RM] - cr[(cur - 1U - (uint32_t)WIN) & RM]);
			int32_t dl = (wl - we) >> 4;
			int32_t ow = (w - thr) >> 4;

			el_e += bit != 0U ? -dl : dl;
			el_a += (uint32_t)(ow < 0 ? -ow : ow);
			if (--cnt == 0U) {
				int32_t lim = (int32_t)(el_a >> EL_SHIFT);

				cnt = (unsigned int)EL_PERIOD;
				if (el_e > lim) {
					ph = (ph + 1U) & 3U;
				} else if (el_e < -lim) {
					ph = (ph - 1U) & 3U;
				}
				el_e = 0;
				el_a = 0;
			}
		}
		thr = (m0 >> 1) + (m1 >> 1);
		pw = wr[(((cur + 1U) >> 2) - 1U) & PM];
		if (en < em->cap) {
			out[en].t = cur - (uint32_t)TOFF;
			out[en].bit = (uint8_t)bit;
			out[en].pwr = pw;
			en++;
		}
		cur += 4U;
	}
	d->cur = cur;
	d->ph = (uint8_t)ph;
	d->m0 = m0;
	d->m1 = m1;
	d->el_cnt = (uint8_t)cnt;
	d->el_e = el_e;
	d->el_a = el_a;
	d->sliced |= (uint8_t)(en != em->n);
	em->n = en;
}

#ifdef GFSK_PIE
_Static_assert(sizeof(struct gfsk_sym) == 8 && offsetof(struct gfsk_sym, bit) == 4 &&
		       offsetof(struct gfsk_sym, pwr) == 5,
	       "the symbol gfsk_slice.S writes");
_Static_assert(WIN == 2 && DD_K == 5 && EL_PERIOD == 32 && EL_SHIFT == 2 && TOFF == 3 &&
		       GFSK_RING == 128U && GFSK_PRING == 32U,
	       "the parameters gfsk_slice.S has built in");
/*
 * gfsk_slice.S: the symbols up to last from op on (dropped at oend), with acq_on stopping before
 * the one whose cursor reaches acq; returns the next op.
 */
struct gfsk_sym *gfsk_slice_fast(struct gfsk_demod *d, uint32_t last, struct gfsk_sym *op,
				 struct gfsk_sym *oend, uint32_t acq_on, uint32_t acq);
#endif

/*
 * The symbols up to @p last (see slice_c()): with the early/late comparison gfsk_slice.S on the
 * S3, which stops where the burst is to be acquired; the acquisition, and the flushes, in C.
 */
UBT_HOT(SAMPLE, slice_batch)
static void slice_batch(struct gfsk_demod *d, struct emit *em, uint32_t last, bool early)
{
#ifdef GFSK_PIE
	if (early) {
		for (;;) {
			struct gfsk_sym *op = gfsk_slice_fast(d, last, em->out + em->n,
							      em->out + em->cap, !d->seeded,
							      d->edge - LEAD_IN);
			size_t en = (size_t)(op - em->out);

			d->sliced |= (uint8_t)(en != em->n);
			em->n = en;
			if (d->seeded || (int32_t)(last - d->cur) < 0 ||
			    (int32_t)(d->cur + LEAD_IN - d->edge) < 0) {
				return;
			}
			/* As slice_c() at this symbol: the acquisition sets the phase and the levels. */
			acquire(d);
		}
	}
#endif
	slice_c(d, em, last, early);
}

/* Emit every symbol whose window the processed samples cover. */
static void flush_pending(struct gfsk_demod *d, struct emit *em)
{
	if (d->cur_valid) {
		slice_batch(d, em, d->f - 3U, false);
	}
	d->cur_valid = 0;
}

/* ---------------------------------------------------------------------------------------- */
/* Full path                                                                                */
/* ---------------------------------------------------------------------------------------- */

/* The input of one push: stream samples [base - GFSK_IN_HIST, end) readable. */
struct in {
	const int8_t *i, *q; /* stream sample base */
	uint32_t base, end;
	/* The vector unit may compute the plane (aligned as gfsk_demod_push() states). */
	bool vec;
};

/* z of [zb, ze) in C: the direct form of the FIR on the int8 samples (flash: the fallback). */
ESP_SDR_CODE(COLD, zp_fill_c)
static __attribute__((noinline)) void zp_fill_c(const struct in *in, uint32_t zb, uint32_t ze)
{
	const int8_t *xi = in->i + (int32_t)(zb - in->base), *xq = in->q + (int32_t)(zb - in->base);
	int16_t *zi = &zp.i[ZP_HEAD], *zq = &zp.q[ZP_HEAD];

	/* + 2: each sample read at the middle of the floor's step, x + 1/2. */
	for (int32_t k = 0; k < (int32_t)(ze - zb); k++) {
		zi[k] = (int16_t)(xi[k] + xi[k - 1] + xi[k - 2] + xi[k - 3] + 2);
		zq[k] = (int16_t)(xq[k] + xq[k - 1] + xq[k - 2] + xq[k - 3] + 2);
	}
}

/* The plane from the block holding @p lo on, as far as it reaches. */
UBT_HOT(SAMPLE, zp_fill)
static void zp_fill(const struct in *in, uint32_t lo)
{
	uint32_t zb = lo & ~15U;
	uint32_t room = in->end - zb;

#ifdef GFSK_PIE
	if (in->vec) {
		/* Whole blocks: up to 15 samples past the end are readable (gfsk_demod.h). */
		uint32_t nblk = (room > ZP_LEN ? ZP_LEN : room + 15U) / 16U;
		const int8_t *xi = in->i + (int32_t)(zb - 16U - in->base);
		const int8_t *xq = in->q + (int32_t)(zb - 16U - in->base);
		unsigned int key = arch_irq_lock();

		/* The vector registers are not part of the thread context. */
		esp_sdr_pie_enable();
		gfsk_pie_u(xi, zp.i, nblk);
		gfsk_pie_u(xq, zp.q, nblk);
		gfsk_pie_z(zp.i, zp.q, nblk);
		arch_irq_unlock(key);
		zp.zb = zb;
		zp.ze = zb + 16U * nblk;
		zp.e_ok = false;
		return;
	}
#endif
	zp.zb = zb;
	zp.ze = zb + (room > ZP_LEN ? ZP_LEN : room);
	zp.e_ok = false;
	zp_fill_c(in, zp.zb, zp.ze);
}

/* The discriminator of the plane's samples in C (flash: the fallback). */
ESP_SDR_CODE(COLD, zp_fill_e_c)
static __attribute__((noinline)) void zp_fill_e_c(void)
{
	const int16_t *zi = &zp.i[ZP_HEAD], *zq = &zp.q[ZP_HEAD];

	for (uint32_t k = 2; k < zp.ze - zp.zb; k++) {
		zp.e[k] = (int32_t)((uint32_t)(zi[k] * zq[k - 2] - zq[k] * zi[k - 2]) << 8);
	}
}

/* The discriminator of the plane's samples from zb + 2 on. */
UBT_HOT(SAMPLE, zp_fill_e)
static void zp_fill_e(const struct in *in)
{
#ifdef GFSK_PIE
	if (in->vec) {
		unsigned int key = arch_irq_lock();

		esp_sdr_pie_enable();
		gfsk_pie_e(&zp.i[ZP_HEAD - 8U], &zp.q[ZP_HEAD - 8U], zp.e, (zp.ze - zp.zb) / 8U);
		arch_irq_unlock(key);
		zp.e_ok = true;
		return;
	}
#else
	(void)in;
#endif
	zp_fill_e_c();
	zp.e_ok = true;
}

/* The plane covers the samples [lo, hi) (hi - lo at most ZP_LEN - 15). */
static inline void zp_need(const struct in *in, uint32_t lo, uint32_t hi)
{
	if (lo - zp.zb >= zp.ze - zp.zb || hi - zp.zb > zp.ze - zp.zb) {
		zp_fill(in, lo);
	}
}

/* ... and the discriminator of [lo, hi), which needs z from lo - 2 on (once a batch). */
UBT_HOT(SAMPLE, zp_need_e)
static void zp_need_e(const struct in *in, uint32_t lo, uint32_t hi)
{
	zp_need(in, lo - 2U, hi);
	if (!zp.e_ok) {
		zp_fill_e(in);
	}
}

/*
 * Start the full path at index f0 (a multiple of 4): the running sum and the acquisition state.
 * The discriminator of f0 reaches back to z of f0 - 2, whose FIR reaches 3 further: f0 stays 7
 * past the first sample of the input since the last reset or gap (hist_n of them precede base),
 * as the reference's clamp has it.
 */
static void front_start(struct gfsk_demod *d, const struct in *in, uint32_t f0)
{
	uint32_t first = in->base - d->hist_n;

	if ((int32_t)(f0 - (first + 7U)) < 0) {
		f0 = align_up4(first + 7U);
	}
	d->cs = 0;
	d->cring[(f0 - 1U) & RM] = 0;
	d->f = d->lg = d->start = f0;
	d->off = (uint8_t)IDLE_RESET;
	d->run = 0;
	d->seeded = 0;
	d->cur_valid = 0;
	d->need_cursor = 1;
	d->sliced = 0;
	d->s_init = 0;
	d->front_ok = 1;
}

/*
 * The sample loop: ng groups from the index d->f (a multiple of 4) on, e, zi and zq the planes'
 * entries of that sample. Per sample the running sum of the discriminator into the ring, per
 * group the power. In the reference's units: the discriminator x 256, the power x 2048 (it works
 * on the int16 stream (x << 6) + 32, with the discriminator of z >> 2 and the power halved).
 */
UBT_HOT(SAMPLE, hot)
static void hot(struct gfsk_demod *d, const int32_t *e, const int16_t *zi,
			const int16_t *zq, unsigned int ng)
{
	uint32_t cs = d->cs, f = d->f;
	uint32_t *const cring = d->cring, *const pring = d->pring;

	/* Rolled: GCC would unroll it 8 times, 400 bytes more IRAM for a branch a group. */
#pragma GCC unroll 1
	while (ng-- != 0U) {
		uint32_t *cr = &cring[f & RM];

		cs += (uint32_t)e[0];
		cr[0] = cs;
		cs += (uint32_t)e[1];
		cr[1] = cs;
		cs += (uint32_t)e[2];
		cr[2] = cs;
		cs += (uint32_t)e[3];
		cr[3] = cs;
		pring[(f >> 2) & PM] = (uint32_t)(zi[3] * zi[3] + zq[3] * zq[3]) << 11;
		e += 4;
		zi += 4;
		zq += 4;
		f += 4U;
	}
	d->cs = cs;
	d->f = f;
}

/* One sample of the same, any alignment. */
static void step1(struct gfsk_demod *d, const int32_t *e, const int16_t *zi, const int16_t *zq)
{
	uint32_t f = d->f;

	d->cs += (uint32_t)e[0];
	d->cring[f & RM] = d->cs;
	d->f = f + 1U;
	if (((f + 1U) & 3U) == 0U) {
		d->pring[(f >> 2) & PM] = (uint32_t)(zi[0] * zi[0] + zq[0] * zq[0]) << 11;
	}
}

#ifdef GFSK_PIE
/* gfsk_groups.S: the loop below in assembly (closing: 1). */
int gfsk_groups_fast(struct gfsk_demod *d);
_Static_assert(SQ_SHIFT == 2 && FLOOR_UP == 12 && FLOOR_MUL == 6U && IDLE_RESET == 8U &&
		       LEAD_IN == 8U && PWR_COMP_BLE == 1 && PWR_COMP_BR == 0 && GFSK_BLE == 0 &&
		       GFSK_PRING == 32U,
	       "the parameters gfsk_groups.S has built in");
#endif

/*
 * The per group logic of the groups the sample loop completed (the detector, the burst edge, the
 * squelch hold); returns true when the squelch closes, lg then the end of the closing group. On
 * the S3 gfsk_groups.S runs it and this is the check's oracle (flash).
 */
ESP_SDR_CODE(COLD, groups_c)
static __attribute__((noinline)) bool groups_c(struct gfsk_demod *d)
{
	uint32_t lg = d->lg, f = d->f, savg = d->savg, floor = d->floor, sgate = d->s_gate;
	uint32_t pend = d->pend, sbase = d->sbase;
	unsigned int run = d->run, off = d->off;
	bool closing = false;
	uint8_t pw_c = d->pw_c;

	while ((int32_t)(f - lg) >= 4) {
		uint32_t g = lg >> 2, p = d->pring[g & PM], savg_prev = savg;
		bool on;

		lg += 4U;
		if (!d->s_init) {
			/*
			 * The floor is seeded once after a reset, not at every start of the full path:
			 * after a gap the first group (a Wi-Fi burst, say) set the floor 30 dB high.
			 */
			savg = p;
			floor = floor == 0U ? p : floor;
			d->s_init = 1;
		} else {
			savg = (uint32_t)((int32_t)savg + (((int32_t)(p - savg)) >> SQ_SHIFT));
		}
		if ((g & 3U) == 0U) {
			pw_c = pwr_of(d, savg);
		}
		d->wring[g & PM] = pw_c;

		/*
		 * Burst edge: two groups above the gate level after a pause or a step up. A step up
		 * inside a run (a group 6 dB over the smoothed power before it) starts the run over:
		 * a burst that begins while noise or a weaker burst holds the gate open is acquired
		 * anew, not sliced with the phase and levels of what came before it.
		 */
		if (d->pwr_thr == 0U) {
			/* No squelch: the gate follows the tracked noise floor. */
			/*
			 * In uint32 without products: a group near full scale (the receiver's start
			 * after a tune) made 4 floor wrap, and the floor never came down again.
			 */
			if ((p >> 2) < floor) {
				if (p >= floor) {
					floor += (p - floor) >> 6;
				} else {
					floor -= (floor - p) >> 6;
				}
			} else {
				floor += (floor >> FLOOR_UP) + 1U;
			}
			sgate = floor > UINT32_MAX / FLOOR_MUL ? UINT32_MAX : floor * FLOOR_MUL;
		}
		on = p >= sgate;
		if (on) {
			if (run >= 2U && p > 4U * savg_prev) {
				run = 0;
			}
			run = run < 255U ? run + 1U : run;
			if (run == 1U) {
				pend = lg - 4U;
				sbase = savg_prev;
			} else if (run == 2U) {
				if (off >= IDLE_RESET || p > 4U * sbase) {
					d->edge = pend;
					d->seeded = 0;
					if (d->cur_valid && !d->sliced) {
						uint32_t c = align_ph(pend - LEAD_IN, d->ph);

						if ((int32_t)(c - d->cur) > 0) {
							d->cur = c;
						}
					}
				}
				off = 0;
			}
		} else {
			run = 0;
			off = off < 255U ? off + 1U : off;
		}

		/* Squelch hold (not while replaying the pre-roll). */
		if (!d->always && (int32_t)(lg - d->replay_end) >= 0) {
			if (savg >= d->s_close) {
				d->hold_cnt = d->hold;
			} else if (d->hold_cnt <= 1U) {
				closing = true;
				break;
			} else {
				d->hold_cnt--;
			}
		}
	}
	d->lg = lg;
	d->savg = savg;
	d->floor = floor;
	d->s_gate = sgate;
	d->pend = pend;
	d->sbase = sbase;
	d->run = (uint8_t)run;
	d->off = (uint8_t)off;
	d->pw_c = pw_c;
	return closing;
}

/*
 * The groups the sample loop completed: the per group logic, then the decisions the cursor can
 * take. Returns true when the squelch closes (d->f is then the first sample not processed).
 */
UBT_HOT(SAMPLE, run_groups)
static bool run_groups(struct gfsk_demod *d, struct emit *em)
{
#ifdef GFSK_PIE
	bool closing = gfsk_groups_fast(d) != 0;
#else
	bool closing = groups_c(d);
#endif
	uint32_t lg = d->lg;

	/* Decision cursor. */
	if (d->need_cursor && (int32_t)(lg - 1U - d->start) >= 7) {
		d->cur = align_ph(d->start + 4U, d->ph);
		d->cur_valid = 1;
		d->need_cursor = 0;
	}
	if (closing) {
		d->f = lg;
		return true;
	}
	if (d->cur_valid) {
		PROF_T(t2);
		slice_batch(d, em, lg - 1U - 4U * GFSK_LAT_SYMS, true);
		PROF_ADD(slice, t2);
	}
	return false;
}

/* Returns true when the squelch closed (d->f is then the first sample not processed). */
UBT_HOT(SAMPLE, front_run)
static bool front_run(struct gfsk_demod *d, const struct in *in, struct emit *em)
{
	int32_t k = (int32_t)(d->f - in->base);
	const int32_t kend = (int32_t)(in->end - in->base);

	while (k < kend) {
		if ((d->f & 3U) == 0U && k >= 4 && kend - k >= 4) {
			unsigned int ng = (unsigned int)((kend - k) / 4);

			ng = ng > GFSK_CHUNK ? GFSK_CHUNK : ng;
			PROF_T(t0);
			zp_need_e(in, d->f, d->f + 4U * ng);
			hot(d, &zp.e[d->f - zp.zb], &zp.i[ZP_HEAD + (d->f - zp.zb)],
			    &zp.q[ZP_HEAD + (d->f - zp.zb)], ng);
			PROF_ADD(hot, t0);
			k += 4 * (int32_t)ng;
		} else {
			zp_need_e(in, d->f, d->f + 1U);
			step1(d, &zp.e[d->f - zp.zb], &zp.i[ZP_HEAD + (d->f - zp.zb)],
			      &zp.q[ZP_HEAD + (d->f - zp.zb)]);
			k++;
			if ((d->f & 3U) != 0U && k < kend) {
				continue;
			}
		}
		PROF_T(t1);
		bool closed = run_groups(d, em);

		PROF_ADD(groups, t1);
		if (closed) {
			return true;
		}
	}
	return false;
}

/* ---------------------------------------------------------------------------------------- */
/* Closed path                                                                              */
/* ---------------------------------------------------------------------------------------- */

/*
 * Returns true when the detector opened (d->f is then the first sample after the group): the
 * smoothed power of each group end from d->f on, groups whose FIR reaches before the first
 * sample of the input since the last reset or gap skipped.
 */
UBT_HOT(SAMPLE, closed_scan)
static bool closed_scan(struct gfsk_demod *d, const struct in *in)
{
	uint32_t idx = d->f + ((3U - d->f) & 3U);
	int32_t kk = (int32_t)(idx - in->base);
	const int32_t kend = (int32_t)(in->end - in->base);
	const int32_t lo = 3 - (int32_t)d->hist_n;
	uint32_t savg = d->savg;
	const uint32_t thr = d->s_thr;
	bool init = d->s_init;

	if (kk < lo) {
		/* The groups whose FIR reaches before the first sample since the reset or gap. */
		kk += ((lo < kend ? lo : kend) - kk + 3) & ~3;
	}
	while (kk < kend) {
		uint32_t at = in->base + (uint32_t)kk;
		const int16_t *zi, *zq;
		int32_t kw;

		zp_need(in, at, at + 1U);
		zi = &zp.i[ZP_HEAD + (at - zp.zb)];
		zq = &zp.q[ZP_HEAD + (at - zp.zb)];
		kw = (int32_t)(zp.ze - in->base);
		kw = kw < kend ? kw : kend;
		for (; kk < kw; kk += 4, zi += 4, zq += 4) {
			uint32_t p = (uint32_t)(zi[0] * zi[0] + zq[0] * zq[0]) << 11;

			if (!init) {
				savg = p;
				init = true;
			} else {
				savg = (uint32_t)((int32_t)savg + (((int32_t)(p - savg)) >> SQ_SHIFT));
			}
			if (savg >= thr) {
				d->savg = savg;
				d->s_init = 1;
				d->f = in->base + (uint32_t)kk + 1U;
				return true;
			}
		}
	}
	d->savg = savg;
	d->s_init = init;
	d->f = in->base + (uint32_t)kk - 3U;
	return false;
}

UBT_HOT(CHUNK, gfsk_demod_push)
size_t gfsk_demod_push(struct gfsk_demod *d, const int8_t *i, const int8_t *q, size_t n,
		       struct gfsk_sym *out, size_t out_cap)
{
	struct emit em = {.out = out, .cap = out_cap, .n = 0};
	const struct in in = {
		.i = i,
		.q = q,
		.base = d->n,
		.end = d->n + (uint32_t)n,
		.vec = ((((uintptr_t)i - d->n) | ((uintptr_t)q - d->n)) & 15U) == 0U,
	};

	/* The plane belongs to the previous push's input. */
	zp.zb = 0;
	zp.ze = 0;

	if (d->flushing) {
		/* The symbols whose windows end before the gap; f has already jumped over it. */
		uint32_t f = d->f;

		d->f = d->flush_f;
		flush_pending(d, &em);
		d->f = f;
		d->flushing = 0;
	}
	for (;;) {
		if (!d->open) {
			uint32_t trig;

			if (!closed_scan(d, &in)) {
				break;
			}
			/* Replay the pre-roll through the full path. */
			trig = d->f;
			front_start(d, &in, trig - GFSK_PRE_SAMPLES);
			d->open = 1;
			d->replay_end = trig;
			d->hold_cnt = d->hold;
		} else if (!d->front_ok) {
			front_start(d, &in, align_up4(d->f));
		}
		PROF_T(t3);
#ifdef GFSK_PROF
		uint32_t f_before = d->f;
#endif
		bool closed = front_run(d, &in, &em);

		PROF_ADD(front, t3);
#ifdef GFSK_PROF
		gfsk_prof.open += d->f - f_before;
		gfsk_prof.opens += closed ? 1U : 0U;
#endif
		if (!closed) {
			break;
		}
		flush_pending(d, &em);
		d->open = 0;
	}
	/* Samples of the input before the next push's base (the reference's history count). */
	d->hist_n = (uint16_t)(d->hist_n + n > GFSK_HIST_MAX ? GFSK_HIST_MAX : d->hist_n + n);
	d->n = in.end;
	return em.n;
}

uint32_t gfsk_demod_fir_check(uint32_t seed)
{
	uint32_t bad = 0;
#ifdef GFSK_PIE
	/* PSRAM: internal RAM is short, and the check is not timed. */
	static ESP_SDR_DATA(BULK, xi) int8_t xi[16U + ZP_LEN + 16U] __attribute__((aligned(16)));
	static ESP_SDR_DATA(BULK, xq) int8_t xq[16U + ZP_LEN + 16U] __attribute__((aligned(16)));
	static ESP_SDR_DATA(BULK, ci) int16_t ci[ZP_LEN], cq[ZP_LEN];
	static ESP_SDR_DATA(BULK, ce) int32_t ce[ZP_LEN];
	uint32_t r = seed | 1U;

	for (unsigned int round = 0; round < 8U; round++) {
		/* The base a multiple of 16, as the vector unit needs the addresses. */
		struct in in = {.i = &xi[16], .q = &xq[16], .base = 4096U, .end = 4096U + ZP_LEN};

		for (size_t k = 0; k < sizeof(xi); k++) {
			r = r * 1664525U + 1013904223U;
			switch (round) {
			case 4: /* the extremes, anyhow */
				xi[k] = (r >> 24) & 1U ? 127 : -128;
				xq[k] = (r >> 25) & 1U ? 127 : -128;
				break;
			case 5: /* runs of the extremes, which reach |z| = 512 */
				xi[k] = (k & 8U) != 0U ? -128 : 127;
				xq[k] = ((k + 4U) & 8U) != 0U ? -128 : 127;
				break;
			default:
				xi[k] = (int8_t)(r >> 24);
				xq[k] = (int8_t)(r >> 16);
				break;
			}
		}
		in.vec = true;
		zp_fill(&in, in.base);
		zp_fill_e(&in);
		memcpy(ci, &zp.i[ZP_HEAD], sizeof(ci));
		memcpy(cq, &zp.q[ZP_HEAD], sizeof(cq));
		memcpy(ce, zp.e, sizeof(ce));
		in.vec = false;
		zp_fill(&in, in.base);
		zp_fill_e(&in);
		for (size_t k = 0; k < ZP_LEN; k++) {
			bad += (ci[k] != zp.i[ZP_HEAD + k] || cq[k] != zp.q[ZP_HEAD + k] ||
				(k >= 2U && ce[k] != zp.e[k])) ? 1U : 0U;
		}
	}
	zp.zb = 0;
	zp.ze = 0;
#else
	(void)seed;
#endif
	return bad;
}

uint32_t gfsk_demod_groups_check(uint32_t seed)
{
	uint32_t bad = 0;
#ifdef GFSK_PIE
	/* PSRAM: internal RAM is short, and the check is not timed. */
	static ESP_SDR_DATA(BULK, a) struct gfsk_demod a, b;
	/* Through a pointer: constant arguments make GCC clone it into IRAM. */
	static bool (*volatile fc)(struct gfsk_demod *) = groups_c;
	uint32_t r = seed | 1U;

	for (unsigned int round = 0; round < 4000U; round++) {
		uint32_t scale;
		bool ca, cb;

#define RND() (r = r * 1664525U + 1013904223U, r ^ (r >> 15))
		memset(&a, 0, sizeof(a));
		/* Group powers around the gate, near zero and up to the top of the range. */
		scale = 1U << (RND() % 32U);
		for (size_t k = 0; k < GFSK_PRING; k++) {
			a.pring[k] = (RND() & 7U) == 0U ? RND() : RND() % (scale | 1U);
		}
		for (size_t k = 0; k < GFSK_PRING; k++) {
			a.wring[k] = (uint8_t)RND();
		}
		a.mod = (uint8_t)(RND() & 1U);
		a.always = (uint8_t)((RND() & 3U) == 0U);
		a.pwr_thr = (uint8_t)((RND() & 1U) != 0U ? 0U : RND());
		a.hold = (uint16_t)(RND() % 40U);
		a.hold_cnt = (uint16_t)(RND() % 40U);
		a.s_close = RND() % (scale | 1U);
		a.lg = RND() & ~3U;
		a.f = a.lg + RND() % 160U;
		a.replay_end = a.lg + RND() % 64U - 32U;
		a.savg = RND() % (scale | 1U);
		a.floor = (RND() & 3U) == 0U ? 0U : RND() % (scale | 1U);
		a.s_gate = (RND() & 7U) == 0U ? UINT32_MAX : RND() % (scale | 1U);
		a.s_init = (uint8_t)((RND() & 7U) != 0U);
		a.run = (uint8_t)((RND() & 7U) == 0U ? 250U + RND() % 6U : RND() % 4U);
		a.off = (uint8_t)((RND() & 7U) == 0U ? 250U + RND() % 6U : RND() % 12U);
		a.pw_c = (uint8_t)RND();
		a.pend = RND();
		a.sbase = RND() % (scale | 1U);
		a.edge = RND();
		a.ph = (uint8_t)(RND() & 3U);
		a.seeded = (uint8_t)(RND() & 1U);
		a.cur = a.lg + RND() % 64U - 48U;
		a.cur_valid = (uint8_t)(RND() & 1U);
		a.sliced = (uint8_t)(RND() & 1U);
#undef RND
		memcpy(&b, &a, sizeof(a));
		ca = fc(&a);
		cb = gfsk_groups_fast(&b) != 0;
		if (ca != cb || memcmp(&a, &b, sizeof(a)) != 0) {
			bad++;
		}
	}
#else
	(void)seed;
#endif
	return bad;
}

uint32_t gfsk_demod_slice_check(uint32_t seed)
{
	uint32_t bad = 0;
#ifdef GFSK_PIE
	/* PSRAM: internal RAM is short, and the check is not timed. */
	static ESP_SDR_DATA(BULK, a) struct gfsk_demod a, b;
	static ESP_SDR_DATA(BULK, oa) struct gfsk_sym oa[24], ob[24];
	static void (*volatile fc)(struct gfsk_demod *, struct emit *, uint32_t, bool) = slice_c;
	static void (*volatile fb)(struct gfsk_demod *, struct emit *, uint32_t, bool) = slice_batch;
	uint32_t r = seed | 1U;

	for (unsigned int round = 0; round < 4000U; round++) {
		struct emit ea = {.out = oa}, eb = {.out = ob};
		uint32_t last;

#define RND() (r = r * 1664525U + 1013904223U, r ^ (r >> 15))
		memset(&a, 0, sizeof(a));
		for (size_t k = 0; k < GFSK_RING; k++) {
			/* Window sums anywhere, and some that wrap the 32 bits of the running sum. */
			a.cring[k] = (round & 1U) != 0U ? RND() : RND() % 4096U - 2048U;
		}
		for (size_t k = 0; k < GFSK_PRING; k++) {
			a.wring[k] = (uint8_t)RND();
			/* The group powers the acquisition reads: a burst's rise somewhere. */
			uint32_t span = (RND() & 1U) != 0U ? 4096U : 65536U;

			a.pring[k] = RND() % span;
		}
		a.seeded = (uint8_t)((round & 4U) == 0U);
		a.cur = RND();
		a.edge = a.cur + RND() % 96U - 24U;
		a.start = a.edge - RND() % 64U;
		a.mod = (uint8_t)(RND() & 1U);
		a.ph = (uint8_t)(RND() & 3U);
		a.m0 = (int32_t)(RND() % 4096U) - 2048;
		a.m1 = (int32_t)(RND() % 4096U) - 2048;
		if ((round & 2U) != 0U) {
			a.m0 = (int32_t)RND();
			a.m1 = (int32_t)RND();
		}
		a.el_cnt = (uint8_t)(RND() % 40U);
		a.el_e = (int32_t)(RND() % 2048U) - 1024;
		a.el_a = RND() % 4096U;
		a.sliced = (uint8_t)(RND() & 1U);
		ea.cap = RND() % 25U;
		eb.cap = ea.cap;
		ea.n = eb.n = ea.cap != 0U ? RND() % ea.cap : 0U;
		last = a.cur + RND() % 80U - 8U;
		memcpy(&b, &a, sizeof(a));
		memset(oa, 0, sizeof(oa));
		memset(ob, 0, sizeof(ob));
		/* Through pointers: constant arguments make GCC clone the slicers into IRAM again. */
		fc(&a, &ea, last, true);
		fb(&b, &eb, last, true);
#undef RND
		if (memcmp(&a, &b, sizeof(a)) != 0 || ea.n != eb.n ||
		    memcmp(oa, ob, sizeof(oa)) != 0) {
			bad++;
		}
	}
#else
	(void)seed;
#endif
	return bad;
}

size_t gfsk_demod_flush(struct gfsk_demod *d, struct gfsk_sym *out, size_t out_cap)
{
	struct emit em = {.out = out, .cap = out_cap, .n = 0};

	if (d->open && d->front_ok) {
		flush_pending(d, &em);
		d->front_ok = 0;
		d->f = d->n;
	}
	return em.n;
}

void gfsk_demod_gap(struct gfsk_demod *d, uint32_t n_lost)
{
	if (d->open && !d->flushing) {
		/* Slicing the look-ahead past f would read ring entries of samples never pushed. */
		d->flushing = 1;
		d->flush_f = d->f;
	}
	if (d->open) {
		d->front_ok = 0;
	}
	d->n += n_lost;
	d->hist_n = 0;
	d->f = d->n;
}
