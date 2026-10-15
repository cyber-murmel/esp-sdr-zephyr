/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * 1 Msym/s GFSK demodulator, see gfsk_demod.h for the design.
 *
 * Indexes are sample numbers of the 4 MS/s stream (wrapping at 2^32). The full path works in
 * groups of 4 samples starting at a multiple of 4 (one symbol period): the sample loop (hot())
 * does the FIR, the discriminator and its running sum into a ring, and the power of each group;
 * run_groups() then does the per group logic (detector, burst edge, the decision cursor).
 */

#include <string.h>

#include "gfsk_demod.h"
#include "ubt_attr.h"

#ifdef GFSK_TUNABLE
#define TUNE(f, def) (gfsk_tune.f)
struct gfsk_tune gfsk_tune = {
	.disc = 2, .win = 2, .dd_k = 5, .nseed = 4, .seed_skip = 9, .el_period = 32,
	.el_shift = 2, .el_stride = 1, .sq_shift = 2, .toff = 3, .floor_up = 12,
};
#else
#define TUNE(f, def) (def)
#endif

#define DISC      TUNE(disc, 2)
#define WIN       TUNE(win, 2)
#define DD_K      TUNE(dd_k, 5)
#define NSEED     TUNE(nseed, 4)
#define SEED_SKIP TUNE(seed_skip, 9)
#define EL_PERIOD TUNE(el_period, 32)
#define EL_SHIFT  TUNE(el_shift, 2)
#define EL_STRIDE TUNE(el_stride, 1)
#define SQ_SHIFT  TUNE(sq_shift, 2)
#define TOFF      TUNE(toff, 3)
#define FLOOR_UP  TUNE(floor_up, 12)

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
_Static_assert(GFSK_HIST >= GFSK_PRE_SAMPLES + 12U, "history covers the replay");
_Static_assert(GFSK_PRE_SAMPLES % 4U == 0U, "the replay is whole groups");

/* log2(1 + (m + 0.5) / 64) * 256 */
static const uint8_t log2_lut[64] = {
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
	l = ((31U - lz) << 8) + log2_lut[((s << lz) >> 25) & 63U];
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

static inline uint32_t align_up4(uint32_t x)
{
	return (x + 3U) & ~3U;
}

/* Smallest index >= x whose low two bits are ph. */
static inline uint32_t align_ph(uint32_t x, unsigned int ph)
{
	return x + ((ph - x) & 3U);
}

/* Sample k of the stream relative to the buffer start; negative k reads the history. */
static inline int32_t xs(const int16_t *hist, const int16_t *buf, int32_t k)
{
	return k >= 0 ? buf[k] : hist[(int32_t)GFSK_HIST + k];
}

static inline uint32_t power_of(int32_t zi, int32_t zq)
{
	return (((uint32_t)zi * (uint32_t)zi) + ((uint32_t)zq * (uint32_t)zq)) >> 1;
}

/* z[k] = x[k] + j x[k-1] - x[k-2] - j x[k-3], straight from the samples. */
static void fir_at(const struct gfsk_demod *d, const int16_t *i, const int16_t *q, int32_t k,
		   int32_t *zi, int32_t *zq)
{
	int32_t i0 = xs(d->hi, i, k), q0 = xs(d->hq, q, k);
	int32_t i1 = xs(d->hi, i, k - 1), q1 = xs(d->hq, q, k - 1);
	int32_t i2 = xs(d->hi, i, k - 2), q2 = xs(d->hq, q, k - 2);
	int32_t i3 = xs(d->hi, i, k - 3), q3 = xs(d->hq, q, k - 3);

	*zi = i0 - q1 - i2 + q3;
	*zq = q0 + i1 - q2 - i3;
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
static void acquire(struct gfsk_demod *d)
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
static void slice_batch(struct gfsk_demod *d, struct emit *em, uint32_t last, bool early)
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

/* Start the full path at index f0 (a multiple of 4), from the raw samples before it. */
static void front_start(struct gfsk_demod *d, const int16_t *i, const int16_t *q, uint32_t base,
			uint32_t f0)
{
	uint32_t first = base - d->hist_n;
	int32_t k, zi, zq;

	if ((int32_t)(f0 - (first + 7U)) < 0) {
		f0 = align_up4(first + 7U);
	}
	k = (int32_t)(f0 - base);
	for (int j = 1; j <= 4; j++) {
		fir_at(d, i, q, k - j, &zi, &zq);
		if (j == 1) {
			d->zi = zi;
			d->zq = zq;
			d->a1i = zi >> 2;
			d->a1q = zq >> 2;
		} else if (j == 2) {
			d->a2i = zi >> 2;
			d->a2q = zq >> 2;
		}
#ifdef GFSK_TUNABLE
		d->ah_i[(f0 - (uint32_t)j) & 3U] = zi >> 2;
		d->ah_q[(f0 - (uint32_t)j) & 3U] = zq >> 2;
#endif
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
 * The sample loop: ng groups from the buffer position pi, pq (which is at least 4 samples into
 * the stream), starting at the index d->f, a multiple of 4. Per sample the FIR z[n] = j z[n-1] +
 * x[n] - x[n-4], the delay-2 discriminator Im(z z[n-2]*) of z >> 2 (negative for a positive
 * deviation) and its running sum into the ring; per group the power. The discriminator history
 * alternates between two register pairs by the parity of the index.
 */
#ifndef GFSK_TUNABLE
UBT_HOT static void hot(struct gfsk_demod *d, const int16_t *pi, const int16_t *pq, unsigned int ng)
{
	int32_t zi = d->zi, zq = d->zq;
	int32_t r0i = d->a2i, r0q = d->a2q, r1i = d->a1i, r1q = d->a1q;
	uint32_t cs = d->cs, f = d->f;

#define STEP(PH, RI, RQ)                                         \
	do {                                                     \
		int32_t ni = (pi[PH] - pi[(PH) - 4]) - zq;       \
		int32_t nq = (pq[PH] - pq[(PH) - 4]) + zi;       \
		int32_t ai = ni >> 2, aq = nq >> 2;              \
		zi = ni;                                         \
		zq = nq;                                         \
		cs += (uint32_t)(aq * (RI) - ai * (RQ));         \
		cr[PH] = cs;                                     \
		RI = ai;                                         \
		RQ = aq;                                         \
	} while (0)

	while (ng-- != 0U) {
		uint32_t *cr = &d->cring[f & RM];

		STEP(0, r0i, r0q);
		STEP(1, r1i, r1q);
		STEP(2, r0i, r0q);
		STEP(3, r1i, r1q);
		d->pring[(f >> 2) & PM] = power_of(zi, zq);
		pi += 4;
		pq += 4;
		f += 4U;
	}
#undef STEP
	d->zi = zi;
	d->zq = zq;
	d->a2i = r0i;
	d->a2q = r0q;
	d->a1i = r1i;
	d->a1q = r1q;
	d->cs = cs;
	d->f = f;
}

/* One sample of the same, any alignment. */
static void step1(struct gfsk_demod *d, int32_t xi, int32_t xq, int32_t xi4, int32_t xq4)
{
	uint32_t f = d->f;
	int32_t ni = (xi - xi4) - d->zq, nq = (xq - xq4) + d->zi;
	int32_t ai = ni >> 2, aq = nq >> 2;
	int32_t *ri = (f & 1U) != 0U ? &d->a1i : &d->a2i, *rq = (f & 1U) != 0U ? &d->a1q : &d->a2q;

	d->zi = ni;
	d->zq = nq;
	d->cs += (uint32_t)(aq * *ri - ai * *rq);
	d->cring[f & RM] = d->cs;
	*ri = ai;
	*rq = aq;
	d->f = f + 1U;
	if (((f + 1U) & 3U) == 0U) {
		d->pring[(f >> 2) & PM] = power_of(ni, nq);
	}
}
#else
/* The tunable build keeps the discriminator history in memory to vary its delay. */
static void step1(struct gfsk_demod *d, int32_t xi, int32_t xq, int32_t xi4, int32_t xq4)
{
	uint32_t f = d->f;
	int32_t ni = (xi - xi4) - d->zq, nq = (xq - xq4) + d->zi;
	int32_t ai = ni >> 2, aq = nq >> 2, e;
	int32_t pi = d->ah_i[(f - (uint32_t)DISC) & 3U];
	int32_t pq = d->ah_q[(f - (uint32_t)DISC) & 3U];

	d->zi = ni;
	d->zq = nq;
	switch (DISC) {
	case 1:
		e = ai * pi + aq * pq;
		break;
	case 2:
		e = aq * pi - ai * pq;
		break;
	case 3:
		e = -(ai * pi + aq * pq);
		break;
	default:
		e = -(aq * pi - ai * pq);
		break;
	}
	d->ah_i[f & 3U] = ai;
	d->ah_q[f & 3U] = aq;
	d->cs += (uint32_t)e;
	d->cring[f & RM] = d->cs;
	d->f = f + 1U;
	if (((f + 1U) & 3U) == 0U) {
		d->pring[(f >> 2) & PM] = power_of(ni, nq);
	}
}
#endif

/*
 * The groups the sample loop completed: the detector and the burst edge per group, then the
 * decisions the cursor can take. Returns true when the squelch closes (d->f is then the first
 * sample not processed).
 */
static bool run_groups(struct gfsk_demod *d, struct emit *em)
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
			savg = floor = p;
			d->s_init = 1;
		} else {
			savg = (uint32_t)((int32_t)savg + (((int32_t)(p - savg)) >> SQ_SHIFT));
		}
		if ((g & 3U) == 0U) {
			pw_c = pwr_of(d, savg);
		}
		d->wring[g & PM] = pw_c;

		/* Burst edge: two groups above the gate level after a pause or a step up. */
		if (d->pwr_thr == 0U) {
			/* No squelch: the gate follows the tracked noise floor. */
			if (p < 4U * floor) {
				floor += (uint32_t)((int32_t)(p - floor) >> 6);
			} else {
				floor += (floor >> FLOOR_UP) + 1U;
			}
			sgate = floor > UINT32_MAX / FLOOR_MUL ? UINT32_MAX : floor * FLOOR_MUL;
		}
		on = p >= sgate;
		if (on) {
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
		slice_batch(d, em, lg - 1U - 4U * GFSK_LAT_SYMS, true);
	}
	return false;
}

/* Returns true when the squelch closed (d->f is then the first sample not processed). */
static bool front_run(struct gfsk_demod *d, const int16_t *i, const int16_t *q, uint32_t base,
		      size_t n, struct emit *em)
{
	int32_t k = (int32_t)(d->f - base);
	const int32_t kend = (int32_t)n;

	while (k < kend) {
#ifndef GFSK_TUNABLE
		if ((d->f & 3U) == 0U && k >= 4 && kend - k >= 4) {
			unsigned int ng = (unsigned int)((kend - k) / 4);

			ng = ng > GFSK_CHUNK ? GFSK_CHUNK : ng;
			hot(d, i + k, q + k, ng);
			k += 4 * (int32_t)ng;
		} else
#endif
		{
			step1(d, xs(d->hi, i, k), xs(d->hq, q, k), xs(d->hi, i, k - 4),
			      xs(d->hq, q, k - 4));
			k++;
			if ((d->f & 3U) != 0U && k < kend) {
				continue;
			}
		}
		if (run_groups(d, em)) {
			return true;
		}
	}
	return false;
}

/* ---------------------------------------------------------------------------------------- */
/* Closed path                                                                              */
/* ---------------------------------------------------------------------------------------- */

/* Returns true when the detector opened (d->f is then the first sample after the group). */
UBT_HOT static bool closed_scan(struct gfsk_demod *d, const int16_t *i, const int16_t *q,
				uint32_t base, size_t n)
{
	uint32_t idx = d->f + ((3U - d->f) & 3U);
	int32_t kk = (int32_t)(idx - base);
	const int32_t kend = (int32_t)n;
	const int32_t lo = 3 - (int32_t)d->hist_n;
	uint32_t savg = d->savg;
	const uint32_t thr = d->s_thr;
	bool init = d->s_init;

	for (; kk < kend; kk += 4) {
		int32_t zi, zq;
		uint32_t p;

		if (kk < lo) {
			continue;
		}
		if (kk >= 3) {
			zi = i[kk] - q[kk - 1] - i[kk - 2] + q[kk - 3];
			zq = q[kk] + i[kk - 1] - q[kk - 2] - i[kk - 3];
		} else {
			fir_at(d, i, q, kk, &zi, &zq);
		}
		p = power_of(zi, zq);
		if (!init) {
			savg = p;
			init = true;
		} else {
			savg = (uint32_t)((int32_t)savg + (((int32_t)(p - savg)) >> SQ_SHIFT));
		}
		if (savg >= thr) {
			d->savg = savg;
			d->s_init = 1;
			d->f = base + (uint32_t)kk + 1U;
			return true;
		}
	}
	d->savg = savg;
	d->s_init = init;
	d->f = base + (uint32_t)kk - 3U;
	return false;
}

static void save_history(struct gfsk_demod *d, const int16_t *i, const int16_t *q, size_t n)
{
	if (n >= GFSK_HIST) {
		memcpy(d->hi, i + n - GFSK_HIST, sizeof(d->hi));
		memcpy(d->hq, q + n - GFSK_HIST, sizeof(d->hq));
	} else if (n != 0U) {
		size_t keep = GFSK_HIST - n;

		memmove(d->hi, d->hi + n, keep * sizeof(d->hi[0]));
		memmove(d->hq, d->hq + n, keep * sizeof(d->hq[0]));
		memcpy(d->hi + keep, i, n * sizeof(d->hi[0]));
		memcpy(d->hq + keep, q, n * sizeof(d->hq[0]));
	}
	d->hist_n = (uint16_t)((size_t)d->hist_n + n > GFSK_HIST ? GFSK_HIST : d->hist_n + n);
}

size_t gfsk_demod_push(struct gfsk_demod *d, const int16_t *i, const int16_t *q, size_t n,
		       struct gfsk_sym *out, size_t out_cap)
{
	struct emit em = {.out = out, .cap = out_cap, .n = 0};
	uint32_t base = d->n;

	if (d->flushing) {
		flush_pending(d, &em);
		d->flushing = 0;
	}
	for (;;) {
		if (!d->open) {
			uint32_t trig;

			if (!closed_scan(d, i, q, base, n)) {
				break;
			}
			/* Replay the pre-roll through the full path. */
			trig = d->f;
			front_start(d, i, q, base, trig - GFSK_PRE_SAMPLES);
			d->open = 1;
			d->replay_end = trig;
			d->hold_cnt = d->hold;
		} else if (!d->front_ok) {
			front_start(d, i, q, base, align_up4(d->f));
		}
		if (!front_run(d, i, q, base, n, &em)) {
			break;
		}
		flush_pending(d, &em);
		d->open = 0;
	}
	save_history(d, i, q, n);
	d->n = base + (uint32_t)n;
	return em.n;
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
	d->n += n_lost;
	d->hist_n = 0;
	d->f = d->n;
	if (d->open) {
		d->flushing = 1;
		d->front_ok = 0;
	}
}
