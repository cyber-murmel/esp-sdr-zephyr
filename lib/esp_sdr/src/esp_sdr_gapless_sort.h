/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The gapless receiver's width sorting (esp_sdr_gapless_cfg.wide), its arithmetic alone: a
 * chunk of raw dump words judged by its correlations SORT_LAG_A, _B and _C samples apart,
 * summed every SORT_STRIDE samples, against its power above the noise. A flat band 17 MHz wide
 * gives 0.17, 0.19 and 0.06 at 80 MS/s, a 2 MHz one 0.98, 0.96 and 0.90; two equal narrow
 * tones keep at least one of the three above 0.5 whatever their spacing. Plain C, no Zephyr:
 * tests/unit/gapless_sort runs it on native_sim.
 */

#ifndef ESP_SDR_GAPLESS_SORT_H_
#define ESP_SDR_GAPLESS_SORT_H_

#include <stdbool.h>
#include <stdint.h>

#ifndef ESP_SDR_SORT_INLINE
#define ESP_SDR_SORT_INLINE static inline __attribute__((always_inline))
#endif

/* Samples a chunk holds (a window), points one apart, the lags. */
#define SORT_CHUNK  800U
#define SORT_STRIDE 16U
#define SORT_LAG_A  4U
#define SORT_LAG_B  6U
#define SORT_LAG_C  10U
/* A chunk is wide when all three correlations stay below half of its power (sort_decide()). */

/* The sums over a chunk's points; zeroed to start one. */
struct sort_acc {
	uint32_t points;
	/* The points' own sums: what DC the receiver's estimate left is taken out with them. */
	int32_t si, sq;
	int32_t r0;
	int32_t r1i, r1q, rai, raq, rbi, rbq, rci, rcq;
};

/* I and Q of a dump word: two 10-bit two's complement fields, I in bits 19:10, Q in 9:0. */
ESP_SDR_SORT_INLINE int32_t sort_raw_i(uint32_t w)
{
	return (int32_t)(w << 12) >> 22;
}

ESP_SDR_SORT_INLINE int32_t sort_raw_q(uint32_t w)
{
	return (int32_t)(w << 22) >> 22;
}

/*
 * The points of @p w (a chunk's words) from sample @p pos on, every SORT_STRIDE up to @p n, at
 * most @p max of them, less DC @p dci, @p dcq (raw units): a point needs SORT_LAG_C samples
 * before it, so a chunk starts at pos = SORT_LAG_C. Returns the position after the last point.
 */
ESP_SDR_SORT_INLINE uint32_t sort_points(struct sort_acc *a, const uint32_t *w, uint32_t pos,
					 uint32_t n, uint32_t max, int32_t dci, int32_t dcq)
{
	int32_t si = a->si, sq = a->sq;
	int32_t r0 = a->r0, r1i = a->r1i, r1q = a->r1q, rai = a->rai, raq = a->raq;
	int32_t rbi = a->rbi, rbq = a->rbq, rci = a->rci, rcq = a->rcq;
	uint32_t k = 0;

	for (; k < max && pos < n; k++, pos += SORT_STRIDE) {
		uint32_t w0 = w[pos], w1 = w[pos - 1U], wa = w[pos - SORT_LAG_A];
		uint32_t wb = w[pos - SORT_LAG_B], wc = w[pos - SORT_LAG_C];
		int32_t i0 = sort_raw_i(w0) - dci, q0 = sort_raw_q(w0) - dcq;
		int32_t i1 = sort_raw_i(w1) - dci, q1 = sort_raw_q(w1) - dcq;
		int32_t ia = sort_raw_i(wa) - dci, qa = sort_raw_q(wa) - dcq;
		int32_t ib = sort_raw_i(wb) - dci, qb = sort_raw_q(wb) - dcq;
		int32_t ic = sort_raw_i(wc) - dci, qc = sort_raw_q(wc) - dcq;

		/* x0 conj(xL): (i0 iL + q0 qL) + j (q0 iL - i0 qL). */
		si += i0;
		sq += q0;
		r0 += i0 * i0 + q0 * q0;
		r1i += i0 * i1 + q0 * q1;
		r1q += q0 * i1 - i0 * q1;
		rai += i0 * ia + q0 * qa;
		raq += q0 * ia - i0 * qa;
		rbi += i0 * ib + q0 * qb;
		rbq += q0 * ib - i0 * qb;
		rci += i0 * ic + q0 * qc;
		rcq += q0 * ic - i0 * qc;
	}
	a->points += k;
	a->si = si;
	a->sq = sq;
	a->r0 = r0;
	a->r1i = r1i;
	a->r1q = r1q;
	a->rai = rai;
	a->raq = raq;
	a->rbi = rbi;
	a->rbq = rbq;
	a->rci = rci;
	a->rcq = rcq;
	return pos;
}

/*
 * At most 64 points a chunk: the sums of squares stay below 2^25 and the squared sums of the
 * samples below 2^30, so the verdict needs no 64-bit arithmetic (on the C6 that is libgcc's,
 * out of flash, too slow for the run).
 */
_Static_assert(SORT_CHUNK / SORT_STRIDE <= 64U, "the sums fit 32 bits");

/* |re + j im| within 7 %: the larger component plus 3/8 of the smaller. */
ESP_SDR_SORT_INLINE uint32_t sort_mag(int32_t re, int32_t im)
{
	uint32_t a = re < 0 ? -(uint32_t)re : (uint32_t)re;
	uint32_t b = im < 0 ? -(uint32_t)im : (uint32_t)im;
	uint32_t mx = a > b ? a : b, mn = a > b ? b : a;

	return mx + (mn >> 2) + (mn >> 3);
}

/* @p mag over @p sig (> 0), 255 for 1 and above; 32-bit arithmetic. */
ESP_SDR_SORT_INLINE uint8_t sort_rho(uint32_t mag, uint32_t sig)
{
	if (mag >= sig) {
		return 255U;
	}
	while (sig >= (1U << 23)) {
		sig >>= 1;
		mag >>= 1;
	}
	return (uint8_t)(mag * 255U / sig);
}

/* @p v scaled by 127 over @p mx (|v| <= mx, mx > 0), in 32 bits. */
ESP_SDR_SORT_INLINE int8_t sort_unit(int32_t v, uint32_t mx)
{
	uint32_t a = v < 0 ? -(uint32_t)v : (uint32_t)v;

	while (mx >= (1U << 24)) {
		mx >>= 1;
		a >>= 1;
	}
	a = a * 127U / mx;
	return (int8_t)(v < 0 ? -(int32_t)a : (int32_t)a);
}

/* What sort_decide() found, besides the verdict. */
struct sort_result {
	/* Power per sample (raw units squared, the noise in it) of the points. */
	uint32_t power;
	/* The correlation one apart, the larger component +-127; the three rhos (255 for 1). */
	int8_t r1_i, r1_q;
	uint8_t rho[3];
	/* The chunk's power passed @p thr. */
	bool loud;
};

/*
 * The verdict on a chunk's sums: wide when its power per sample passes @p thr (the keep
 * threshold; else the chunk is no evidence and gets @p inherit) and all three correlations
 * stay below half its power above @p noise (per sample).
 */
ESP_SDR_SORT_INLINE bool sort_decide(const struct sort_acc *a, uint32_t noise, uint32_t thr,
				     bool inherit, struct sort_result *res)
{
	uint32_t pts = a->points, dc = 0U, m1i, m1q, mx, ma, mb, mc;
	int32_t sig;

	/*
	 * A DC m in every sample adds P |m|^2 to the power and to each correlation's real part
	 * (P points): taken out with the points' own mean.
	 */
	if (pts != 0U) {
		uint32_t ai = a->si < 0 ? -(uint32_t)a->si : (uint32_t)a->si;
		uint32_t aq = a->sq < 0 ? -(uint32_t)a->sq : (uint32_t)a->sq;

		dc = (ai * ai + aq * aq) / pts;
	}
	res->power = pts != 0U && (uint32_t)a->r0 > dc ? ((uint32_t)a->r0 - dc) / pts : 0U;
	res->loud = pts != 0U && res->power > thr;
	m1i = a->r1i < 0 ? -(uint32_t)a->r1i : (uint32_t)a->r1i;
	m1q = a->r1q < 0 ? -(uint32_t)a->r1q : (uint32_t)a->r1q;
	mx = m1i > m1q ? m1i : m1q;
	res->r1_i = mx != 0U ? sort_unit(a->r1i, mx) : 0;
	res->r1_q = mx != 0U ? sort_unit(a->r1q, mx) : 0;
	/* noise * pts: the baseline stays below 2^19 a sample (full scale), pts below 64. */
	sig = a->r0 - (int32_t)dc - (int32_t)((noise < (1U << 19) ? noise : (1U << 19)) * pts);
	if (!res->loud || sig <= 0) {
		res->rho[0] = res->rho[1] = res->rho[2] = 255U;
		return res->loud ? false : inherit;
	}
	ma = sort_mag(a->rai - (int32_t)dc, a->raq);
	mb = sort_mag(a->rbi - (int32_t)dc, a->rbq);
	mc = sort_mag(a->rci - (int32_t)dc, a->rcq);
	res->rho[0] = sort_rho(ma, (uint32_t)sig);
	res->rho[1] = sort_rho(mb, (uint32_t)sig);
	res->rho[2] = sort_rho(mc, (uint32_t)sig);
	/* Below half of the power above the noise, all three. */
	return 2U * ma < (uint32_t)sig && 2U * mb < (uint32_t)sig && 2U * mc < (uint32_t)sig;
}

#endif /* ESP_SDR_GAPLESS_SORT_H_ */
