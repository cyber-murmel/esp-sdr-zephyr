/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Polyphase channelizer and burst detector (esdr_pfb.h).
 *
 * With n = r + p M (branch r, tap p) and N = m D the frame's newest pair, the definition splits
 * into branch sums and a DFT:
 *
 *   y_k[m] = exp(-j tau k N / M) sum_r exp(j tau k r / M) u_r,
 *   u_r = sum_p h[r + p M] x[N - r - p M].
 *
 * N / M is m / 2, so the rotation is (-1)^(k m): branch r goes to FFT input (M / 2 (m & 1) - r)
 * mod M, and a forward FFT gives the channels in natural order. The FFT is mixed radix
 * decimation in time; its digit-reversed input order is folded into the same write position
 * (widx), so the branch sums land where the first stage reads them.
 *
 * Frames of one parity are M pairs apart: branch r of frames f, f + 2, f + 4, f + 6 reads one
 * column of pairs N - r + j M, j = -(T - 1) .. 3. A block of four taps takes 7 of them and 4 taps
 * for 32 multiply-adds; with M fixed at compile time (a kernel per M) the column's offsets are
 * load immediates, and GCC keeps the 3 pairs a block shares with the next in registers.
 */

#include <errno.h>
#include <math.h>
#include <string.h>

#include <esp_sdr/esp_sdr_dsp.h>

#include "esdr_pfb.h"

#define ALIGN16(x) (((x) + 15U) & ~(size_t)15U)

/* exp(-j tau / 5) and exp(-j 2 tau / 5): cos, sin. */
#define C5_1 0.309016994374947424f
#define S5_1 0.951056516295153572f
#define C5_2 (-0.809016994374947424f)
#define S5_2 0.587785252292473129f

/* The butterflies and kernels are instantiated per M: strides become immediates. */
#if defined(__GNUC__)
#define KERNEL static inline __attribute__((always_inline))
#else
#define KERNEL static inline
#endif

/* Detector: floor follower over quiet blocks; blocks loud in a row before a channel relearns. */
#define DET_FOLLOW  64.0f
#define DET_RELEARN 2048U

/* ---- prototype ---- */

static float bessel_i0(float x)
{
	float s = 1.0f, t = 1.0f;

	for (int k = 1; k < 50; k++) {
		float q = x / (2.0f * (float)k);

		t *= q * q;
		s += t;
		if (t < 1e-9f * s) {
			break;
		}
	}
	return s;
}

/* Kaiser's beta for a stop band of @p db. */
static float kaiser_beta(float db)
{
	if (db > 50.0f) {
		return 0.1102f * (db - 8.7f);
	}
	if (db >= 21.0f) {
		return 0.5842f * powf(db - 21.0f, 0.4f) + 0.07886f * (db - 21.0f);
	}
	return 0.0f;
}

void esdr_pfb_proto(float *h, unsigned int m, unsigned int t, float stop_db, float gain)
{
	const unsigned int l = m * t;
	const float beta = kaiser_beta(stop_db), i0b = bessel_i0(beta);
	const float mid = 0.5f * (float)(l - 1U), span = 2.0f / (float)(l - 1U);
	double sum = 0.0;

	/* One half, mirrored: L is even, so the offset from the centre is never 0. */
	for (unsigned int n = 0; n < l / 2U; n++) {
		float off = (float)n - mid, a = (ESP_SDR_TAU_F / 2.0f) * off / (float)m;
		float r = off * span;

		h[n] = sinf(a) / a * bessel_i0(beta * sqrtf(fmaxf(1.0f - r * r, 0.0f))) / i0b;
		h[l - 1U - n] = h[n];
		sum += 2.0 * (double)h[n];
	}
	for (unsigned int n = 0; n < l; n++) {
		h[n] = (float)((double)h[n] * (double)gain / sum);
	}
}

/* The prototype (esdr_pfb_proto()) into p->h branch-major. @p w holds L floats of scratch. */
static void design(struct esdr_pfb *p, float stop_db, float gain, float *w)
{
	esdr_pfb_proto(w, p->m, p->t, stop_db, gain);
	for (unsigned int r = 0; r < p->m; r++) {
		for (unsigned int k = 0; k < p->t; k++) {
			p->h[r * p->t + k] = w[r + k * p->m];
		}
	}
}

float esdr_pfb_tap(const struct esdr_pfb *p, unsigned int n)
{
	return n < p->l ? p->h[(n % p->m) * p->t + n / p->m] : 0.0f;
}

/* ---- FFT ---- */

/* Radices in stage order, the largest first: stage 1 runs without twiddles. */
static unsigned int plan(unsigned int m, uint8_t radix[4])
{
	static const uint8_t tab[][5] = {
		{16, 4, 4, 0, 0}, {32, 4, 4, 2, 0}, {40, 5, 4, 2, 0},
		{64, 4, 4, 4, 0}, {80, 5, 4, 4, 0},
	};

	for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
		if (tab[i][0] == m) {
			unsigned int s = 0;

			for (; s < 4U && tab[i][s + 1U] != 0U; s++) {
				radix[s] = tab[i][s + 1U];
			}
			return s;
		}
	}
	return 0;
}

/* Twiddles of stage @p s (complex): column j = 1 .. span - 1, then q = 1 .. radix - 1. */
static size_t tw_count(const uint8_t *radix, unsigned int s)
{
	size_t span = 1;

	for (unsigned int i = 0; i < s; i++) {
		span *= radix[i];
	}
	return (span - 1U) * (radix[s] - 1U);
}

/*
 * The stages combine radix sub-transforms of span points, span apart within a group of
 * radix * span, column j of each times W^(q j) (W = exp(-j tau / (radix span))) before the
 * butterfly. src and dst may be the same.
 */
KERNEL void bfly2(const float *a, float *d, size_t s1, const float *w)
{
	float ar = a[0], ai = a[1], br = a[s1], bi = a[s1 + 1U];

	if (w != NULL) {
		float tr = br * w[0] - bi * w[1];

		bi = br * w[1] + bi * w[0];
		br = tr;
	}
	d[0] = ar + br;
	d[1] = ai + bi;
	d[s1] = ar - br;
	d[s1 + 1U] = ai - bi;
}

KERNEL void stage2(const float *src, float *dst, const unsigned int n,
		   const unsigned int span, const float *tw)
{
	const size_t s1 = 2U * span, step = 4U * span, end = 2U * n;

	for (size_t g = 0; g < end; g += step) {
		bfly2(src + g, dst + g, s1, NULL);
	}
	for (unsigned int j = 1; j < span; j++, tw += 2) {
		/* Held in locals: dst may alias the table as far as the compiler knows. */
		const float w[2] = {tw[0], tw[1]};

		for (size_t g = 2U * j; g < end; g += step) {
			bfly2(src + g, dst + g, s1, w);
		}
	}
}

KERNEL void bfly4(const float *a, float *d, size_t s1, const float *w)
{
	float a0r = a[0], a0i = a[1];
	float a1r = a[s1], a1i = a[s1 + 1U];
	float a2r = a[2U * s1], a2i = a[2U * s1 + 1U];
	float a3r = a[3U * s1], a3i = a[3U * s1 + 1U];

	if (w != NULL) {
		float tr;

		tr = a1r * w[0] - a1i * w[1];
		a1i = a1r * w[1] + a1i * w[0];
		a1r = tr;
		tr = a2r * w[2] - a2i * w[3];
		a2i = a2r * w[3] + a2i * w[2];
		a2r = tr;
		tr = a3r * w[4] - a3i * w[5];
		a3i = a3r * w[5] + a3i * w[4];
		a3r = tr;
	}
	float t0r = a0r + a2r, t0i = a0i + a2i, t1r = a0r - a2r, t1i = a0i - a2i;
	float t2r = a1r + a3r, t2i = a1i + a3i, t3r = a1r - a3r, t3i = a1i - a3i;

	/* X1 = t1 - j t3, X3 = t1 + j t3. */
	d[0] = t0r + t2r;
	d[1] = t0i + t2i;
	d[s1] = t1r + t3i;
	d[s1 + 1U] = t1i - t3r;
	d[2U * s1] = t0r - t2r;
	d[2U * s1 + 1U] = t0i - t2i;
	d[3U * s1] = t1r - t3i;
	d[3U * s1 + 1U] = t1i + t3r;
}

KERNEL void stage4(const float *src, float *dst, const unsigned int n,
		   const unsigned int span, const float *tw)
{
	const size_t s1 = 2U * span, step = 8U * span, end = 2U * n;

	for (size_t g = 0; g < end; g += step) {
		bfly4(src + g, dst + g, s1, NULL);
	}
	for (unsigned int j = 1; j < span; j++, tw += 6) {
		const float w[6] = {tw[0], tw[1], tw[2], tw[3], tw[4], tw[5]};

		for (size_t g = 2U * j; g < end; g += step) {
			bfly4(src + g, dst + g, s1, w);
		}
	}
}

KERNEL void bfly5(const float *a, float *d, size_t s1, const float *w)
{
	float a0r = a[0], a0i = a[1];
	float a1r = a[s1], a1i = a[s1 + 1U];
	float a2r = a[2U * s1], a2i = a[2U * s1 + 1U];
	float a3r = a[3U * s1], a3i = a[3U * s1 + 1U];
	float a4r = a[4U * s1], a4i = a[4U * s1 + 1U];

	if (w != NULL) {
		float tr;

		tr = a1r * w[0] - a1i * w[1];
		a1i = a1r * w[1] + a1i * w[0];
		a1r = tr;
		tr = a2r * w[2] - a2i * w[3];
		a2i = a2r * w[3] + a2i * w[2];
		a2r = tr;
		tr = a3r * w[4] - a3i * w[5];
		a3i = a3r * w[5] + a3i * w[4];
		a3r = tr;
		tr = a4r * w[6] - a4i * w[7];
		a4i = a4r * w[7] + a4i * w[6];
		a4r = tr;
	}
	float b1r = a1r + a4r, b1i = a1i + a4i, d1r = a1r - a4r, d1i = a1i - a4i;
	float b2r = a2r + a3r, b2i = a2i + a3i, d2r = a2r - a3r, d2i = a2i - a3i;
	/* X1, X4 = e1 -/+ j f1; X2, X3 = e2 -/+ j f2. */
	float e1r = a0r + C5_1 * b1r + C5_2 * b2r, e1i = a0i + C5_1 * b1i + C5_2 * b2i;
	float e2r = a0r + C5_2 * b1r + C5_1 * b2r, e2i = a0i + C5_2 * b1i + C5_1 * b2i;
	float f1r = S5_1 * d1r + S5_2 * d2r, f1i = S5_1 * d1i + S5_2 * d2i;
	float f2r = S5_2 * d1r - S5_1 * d2r, f2i = S5_2 * d1i - S5_1 * d2i;

	d[0] = a0r + b1r + b2r;
	d[1] = a0i + b1i + b2i;
	d[s1] = e1r + f1i;
	d[s1 + 1U] = e1i - f1r;
	d[4U * s1] = e1r - f1i;
	d[4U * s1 + 1U] = e1i + f1r;
	d[2U * s1] = e2r + f2i;
	d[2U * s1 + 1U] = e2i - f2r;
	d[3U * s1] = e2r - f2i;
	d[3U * s1 + 1U] = e2i + f2r;
}

KERNEL void stage5(const float *src, float *dst, const unsigned int n,
		   const unsigned int span, const float *tw)
{
	const size_t s1 = 2U * span, step = 10U * span, end = 2U * n;

	for (size_t g = 0; g < end; g += step) {
		bfly5(src + g, dst + g, s1, NULL);
	}
	for (unsigned int j = 1; j < span; j++, tw += 8) {
		const float w[8] = {tw[0], tw[1], tw[2], tw[3], tw[4], tw[5], tw[6], tw[7]};

		for (size_t g = 2U * j; g < end; g += step) {
			bfly5(src + g, dst + g, s1, w);
		}
	}
}

/* Forward FFTs of @p src (digit-reversed order) to @p dst (natural order). */
static void fft16(const struct esdr_pfb *p, const float *src, float *dst)
{
	stage4(src, dst, 16, 1, NULL);
	stage4(dst, dst, 16, 4, p->tw[1]);
}

static void fft32(const struct esdr_pfb *p, const float *src, float *dst)
{
	stage4(src, dst, 32, 1, NULL);
	stage4(dst, dst, 32, 4, p->tw[1]);
	stage2(dst, dst, 32, 16, p->tw[2]);
}

static void fft40(const struct esdr_pfb *p, const float *src, float *dst)
{
	stage5(src, dst, 40, 1, NULL);
	stage4(dst, dst, 40, 5, p->tw[1]);
	stage2(dst, dst, 40, 20, p->tw[2]);
}

static void fft64(const struct esdr_pfb *p, const float *src, float *dst)
{
	stage4(src, dst, 64, 1, NULL);
	stage4(dst, dst, 64, 4, p->tw[1]);
	stage4(dst, dst, 64, 16, p->tw[2]);
}

static void fft80(const struct esdr_pfb *p, const float *src, float *dst)
{
	stage5(src, dst, 80, 1, NULL);
	stage4(dst, dst, 80, 5, p->tw[1]);
	stage4(dst, dst, 80, 20, p->tw[2]);
}

/* Where natural input i sits for the stages: the last stage's digit is the most significant. */
static unsigned int digit_rev(const struct esdr_pfb *p, unsigned int i)
{
	unsigned int span[4], pos = 0;

	span[0] = 1;
	for (unsigned int s = 1; s < p->stages; s++) {
		span[s] = span[s - 1U] * p->radix[s - 1U];
	}
	for (unsigned int s = p->stages; s-- > 0U;) {
		pos += (i % p->radix[s]) * span[s];
		i /= p->radix[s];
	}
	return pos;
}

/* ---- branch sums ---- */

/* Four frames' sums of one branch, I and Q. */
struct acc4 {
	float r0, i0, r1, i1, r2, i2, r3, i3;
};

/*
 * Taps h[0 .. 3] of four frames of one parity: frame q, tap i takes z[(q - i) c], the pair M (q -
 * i) after z.
 */
KERNEL void taps4(struct acc4 *a, const float *h, const float *z, const ptrdiff_t c)
{
	const float h0 = h[0], h1 = h[1], h2 = h[2], h3 = h[3];
	const float n3r = z[-3 * c], n3i = z[-3 * c + 1];
	const float n2r = z[-2 * c], n2i = z[-2 * c + 1];
	const float n1r = z[-c], n1i = z[-c + 1];
	const float z0r = z[0], z0i = z[1];
	const float p1r = z[c], p1i = z[c + 1];
	const float p2r = z[2 * c], p2i = z[2 * c + 1];
	const float p3r = z[3 * c], p3i = z[3 * c + 1];

	a->r0 += h0 * z0r;
	a->i0 += h0 * z0i;
	a->r1 += h0 * p1r;
	a->i1 += h0 * p1i;
	a->r2 += h0 * p2r;
	a->i2 += h0 * p2i;
	a->r3 += h0 * p3r;
	a->i3 += h0 * p3i;
	a->r0 += h1 * n1r;
	a->i0 += h1 * n1i;
	a->r1 += h1 * z0r;
	a->i1 += h1 * z0i;
	a->r2 += h1 * p1r;
	a->i2 += h1 * p1i;
	a->r3 += h1 * p2r;
	a->i3 += h1 * p2i;
	a->r0 += h2 * n2r;
	a->i0 += h2 * n2i;
	a->r1 += h2 * n1r;
	a->i1 += h2 * n1i;
	a->r2 += h2 * z0r;
	a->i2 += h2 * z0i;
	a->r3 += h2 * p1r;
	a->i3 += h2 * p1i;
	a->r0 += h3 * n3r;
	a->i0 += h3 * n3i;
	a->r1 += h3 * n2r;
	a->i1 += h3 * n2i;
	a->r2 += h3 * n1r;
	a->i2 += h3 * n1i;
	a->r3 += h3 * z0r;
	a->i3 += h3 * z0i;
}

/*
 * Branch sums of four frames of one parity, the first's newest pair at @p xn, the others M pairs
 * apart each, to four consecutive slots of @p u (2 M floats each) at widx.
 */
KERNEL void branches4(const float *h, const float *xn, float *u, const uint16_t *widx,
		      const unsigned int m, const unsigned int t)
{
	const ptrdiff_t c = 2 * (ptrdiff_t)m;

	for (unsigned int r = 0; r < m; r++, h += t) {
		/* z[j c]: pair N - r + j M, less 4 M per block of taps. */
		const float *z = xn - 2 * (ptrdiff_t)r;
		/* -0 + x is x: the first taps multiply rather than add to zero. */
		struct acc4 a = {-0.0f, -0.0f, -0.0f, -0.0f, -0.0f, -0.0f, -0.0f, -0.0f};

		taps4(&a, h, z, c);
		for (unsigned int k = 4; k < t; k += 4U) {
			z -= 4 * c;
			taps4(&a, h + k, z, c);
		}
		float *o = u + 2U * widx[r];

		o[0] = a.r0;
		o[1] = a.i0;
		o[2U * m] = a.r1;
		o[2U * m + 1U] = a.i1;
		o[4U * m] = a.r2;
		o[4U * m + 1U] = a.i2;
		o[6U * m] = a.r3;
		o[6U * m + 1U] = a.i3;
	}
}

/* One frame's sum of one branch, even and odd taps apart. */
struct acc1 {
	float r0, i0, r1, i1;
};

/* Taps h[0 .. 3] of one frame: tap i takes z[-i c]. */
KERNEL void taps1(struct acc1 *a, const float *h, const float *z, const ptrdiff_t c)
{
	a->r0 += h[0] * z[0];
	a->i0 += h[0] * z[1];
	a->r1 += h[1] * z[-c];
	a->i1 += h[1] * z[-c + 1];
	a->r0 += h[2] * z[-2 * c];
	a->i0 += h[2] * z[-2 * c + 1];
	a->r1 += h[3] * z[-3 * c];
	a->i1 += h[3] * z[-3 * c + 1];
}

/* Branch sums of one frame, its newest pair at @p xn. */
KERNEL void branches1(const float *h, const float *xn, float *u, const uint16_t *widx,
		      const unsigned int m, const unsigned int t)
{
	const ptrdiff_t c = 2 * (ptrdiff_t)m;

	for (unsigned int r = 0; r < m; r++, h += t) {
		const float *z = xn - 2 * (ptrdiff_t)r;
		struct acc1 a = {-0.0f, -0.0f, -0.0f, -0.0f};

		taps1(&a, h, z, c);
		for (unsigned int k = 4; k < t; k += 4U) {
			z -= 4 * c;
			taps1(&a, h + k, z, c);
		}
		u[2U * widx[r]] = a.r0 + a.r1;
		u[2U * widx[r] + 1U] = a.i0 + a.i1;
	}
}

/* One M's kernels, four frames and one frame, each with T = 8 known or any T. */
#define PFB_KERNELS(m_)                                                                    \
	static void b4_##m_##_8(const struct esdr_pfb *p, const float *xn, float *u,      \
				const uint16_t *widx)                                       \
	{                                                                                  \
		branches4(p->h, xn, u, widx, m_, 8U);                                      \
	}                                                                                  \
	static void b4_##m_(const struct esdr_pfb *p, const float *xn, float *u,          \
			    const uint16_t *widx)                                           \
	{                                                                                  \
		branches4(p->h, xn, u, widx, m_, p->t);                                    \
	}                                                                                  \
	static void b1_##m_##_8(const struct esdr_pfb *p, const float *xn, float *u,      \
				const uint16_t *widx)                                       \
	{                                                                                  \
		branches1(p->h, xn, u, widx, m_, 8U);                                      \
	}                                                                                  \
	static void b1_##m_(const struct esdr_pfb *p, const float *xn, float *u,          \
			    const uint16_t *widx)                                           \
	{                                                                                  \
		branches1(p->h, xn, u, widx, m_, p->t);                                    \
	}

PFB_KERNELS(16)
PFB_KERNELS(32)
PFB_KERNELS(40)
PFB_KERNELS(64)
PFB_KERNELS(80)

typedef void (*kern_fn)(const struct esdr_pfb *p, const float *xn, float *u,
			const uint16_t *widx);

static const struct {
	uint16_t m;
	kern_fn b4_8, b4, b1_8, b1;
	void (*fft)(const struct esdr_pfb *p, const float *src, float *dst);
} kernels[] = {
	{16, b4_16_8, b4_16, b1_16_8, b1_16, fft16},
	{32, b4_32_8, b4_32, b1_32_8, b1_32, fft32},
	{40, b4_40_8, b4_40, b1_40_8, b1_40, fft40},
	{64, b4_64_8, b4_64, b1_64_8, b1_64, fft64},
	{80, b4_80_8, b4_80, b1_80_8, b1_80, fft80},
};

/* ---- set-up ---- */

struct layout {
	size_t h, hist, u, ring, tw, widx, total;
};

static bool check_cfg(const struct esdr_pfb_cfg *cfg, unsigned int *t, uint8_t radix[4],
		      unsigned int *stages)
{
	*t = cfg->taps != 0U ? cfg->taps : ESDR_PFB_T_DEFAULT;
	*stages = plan(cfg->channels, radix);
	/* A pass leaves fewer than a group of frames for the next: room for one more frame. */
	return *stages != 0U && *t % 4U == 0U && *t <= ESDR_PFB_T_MAX && cfg->stop_db >= 0.0f &&
	       cfg->gain >= 0.0f && (cfg->chunk == 0U || cfg->chunk >= 4U * cfg->channels) &&
	       cfg->chunk <= (1U << 24);
}

static bool get_layout(const struct esdr_pfb_cfg *cfg, struct layout *lo)
{
	uint8_t radix[4];
	unsigned int t, stages;
	size_t m, l, chunk, frames, tw = 0;

	if (cfg == NULL || !check_cfg(cfg, &t, radix, &stages)) {
		return false;
	}
	m = cfg->channels;
	l = m * t;
	chunk = cfg->chunk != 0U ? cfg->chunk : 2U * l;
	frames = cfg->frames != 0U ? cfg->frames : 1U;
	for (unsigned int s = 0; s < stages; s++) {
		tw += 2U * tw_count(radix, s);
	}
	lo->h = 0;
	lo->hist = lo->h + ALIGN16(l * sizeof(float));
	lo->u = lo->hist + ALIGN16(2U * (l - 1U + chunk) * sizeof(float));
	lo->ring = lo->u + ALIGN16(2U * m * ESDR_PFB_GROUP * sizeof(float));
	lo->tw = lo->ring + ALIGN16(2U * m * frames * sizeof(float));
	lo->widx = lo->tw + ALIGN16(tw * sizeof(float));
	lo->total = lo->widx + ALIGN16(2U * m * sizeof(uint16_t));
	return true;
}

size_t esdr_pfb_mem_size(const struct esdr_pfb_cfg *cfg)
{
	struct layout lo;

	return get_layout(cfg, &lo) ? lo.total + 16U : 0U;
}

int esdr_pfb_init(struct esdr_pfb *p, const struct esdr_pfb_cfg *cfg, void *mem, size_t len)
{
	struct layout lo;
	unsigned int t, stages;
	uint8_t *base;

	if (p == NULL || mem == NULL || ((uintptr_t)mem & 3U) != 0U || !get_layout(cfg, &lo)) {
		return -EINVAL;
	}
	base = (uint8_t *)ALIGN16((uintptr_t)mem);
	if ((size_t)(base - (uint8_t *)mem) + lo.total > len) {
		return -ENOMEM;
	}
	memset(p, 0, sizeof(*p));
	(void)check_cfg(cfg, &t, p->radix, &stages);
	p->stages = (uint8_t)stages;
	p->m = cfg->channels;
	p->d = (uint16_t)(p->m / 2U);
	p->t = (uint16_t)t;
	p->l = (uint16_t)(p->m * t);
	p->frames = cfg->frames != 0U ? cfg->frames : 1U;
	p->cap = (uint32_t)(p->l - 1U + (cfg->chunk != 0U ? cfg->chunk : 2U * p->l));
	p->h = (float *)(base + lo.h);
	p->hist = (float *)(base + lo.hist);
	p->u = (float *)(base + lo.u);
	p->ring = (float *)(base + lo.ring);
	p->widx = (uint16_t *)(base + lo.widx);

	/* The history is scratch until the first push. */
	design(p, cfg->stop_db != 0.0f ? cfg->stop_db : 60.0f,
	       cfg->gain != 0.0f ? cfg->gain : 1.0f, p->hist);

	float *tw = (float *)(base + lo.tw);
	unsigned int span = 1;

	for (unsigned int s = 0; s < stages; s++) {
		unsigned int rdx = p->radix[s], ls = span * rdx;

		p->tw[s] = tw;
		for (unsigned int j = 1; j < span; j++) {
			for (unsigned int q = 1; q < rdx; q++) {
				double a = -ESP_SDR_TAU * (double)((q * j) % ls) / (double)ls;

				*tw++ = (float)cos(a);
				*tw++ = (float)sin(a);
			}
		}
		span = ls;
	}
	for (size_t i = 0; i < sizeof(kernels) / sizeof(kernels[0]); i++) {
		if (kernels[i].m == p->m) {
			p->b4 = t == 8U ? kernels[i].b4_8 : kernels[i].b4;
			p->b1 = t == 8U ? kernels[i].b1_8 : kernels[i].b1;
			p->fft = kernels[i].fft;
		}
	}
	for (unsigned int par = 0; par < 2U; par++) {
		for (unsigned int r = 0; r < p->m; r++) {
			unsigned int i = (p->m - r + par * p->d) % p->m;

			p->widx[par * p->m + r] = (uint16_t)digit_rev(p, i);
		}
	}
	return 0;
}

void esdr_pfb_reset(struct esdr_pfb *p)
{
	p->live = false;
}

/* ---- running ---- */

/* FFT of the branch sums at @p u into the next ring slot, and the callback. */
static void emit(struct esdr_pfb *p, const float *u, esdr_pfb_frame_cb cb, void *user)
{
	float *out = p->ring + 2U * p->m * p->slot;
	uint32_t flags = (p->nn >= p->full ? ESDR_PFB_COMPLETE : 0U) | (p->gap ? ESDR_PFB_GAP : 0U);

	p->slot = (uint16_t)(p->slot + 1U == p->frames ? 0U : p->slot + 1U);
	p->gap = false;
	p->fft(p, u, out);
	if (cb != NULL) {
		cb(p->nm, out, flags, user);
	}
	p->nm++;
	p->nn += p->d;
}

/*
 * The frames whose newest pair is in the history: whole groups, and with @p all the rest one by
 * one (else it waits for more input).
 */
static int produce(struct esdr_pfb *p, esdr_pfb_frame_cb cb, void *user, bool all)
{
	const unsigned int m = p->m, d = p->d;
	uint32_t nf, pos, f = 0;

	if (p->nn >= p->next) {
		return 0;
	}
	nf = (uint32_t)(p->next - 1U - p->nn) / d + 1U;
	pos = p->fill - (uint32_t)(p->next - p->nn);
	for (; nf - f >= ESDR_PFB_GROUP; f += ESDR_PFB_GROUP) {
		const uint16_t *w0 = p->widx + (p->nm & 1U) * m, *w1 = p->widx + (~p->nm & 1U) * m;
		const float *x = p->hist + 2U * (pos + f * d);

		/* Even frames of the group to slots 0 .. 3, odd ones to 4 .. 7. */
		p->b4(p, x, p->u, w0);
		p->b4(p, x + 2U * d, p->u + 8U * m, w1);
		for (unsigned int g = 0; g < ESDR_PFB_GROUP; g++) {
			emit(p, p->u + 2U * m * ((g & 1U) * 4U + g / 2U), cb, user);
		}
	}
	if (!all) {
		return (int)f;
	}
	for (; f < nf; f++) {
		p->b1(p, p->hist + 2U * (pos + f * d), p->u, p->widx + (p->nm & 1U) * m);
		emit(p, p->u, cb, user);
	}
	return (int)nf;
}

/* Keeps the window of the oldest frame still to come: at least the last L - 1 pairs. */
static void slide(struct esdr_pfb *p)
{
	const uint64_t oldest = p->nn < p->next ? p->nn : p->next;
	const uint32_t keep = (uint32_t)(p->next - oldest) + p->l - 1U;

	memmove(p->hist, p->hist + 2U * (p->fill - keep), 2U * keep * sizeof(float));
	p->fill = keep;
}

/* A new run from @p index: zeros before it. */
static void start(struct esdr_pfb *p, uint64_t index)
{
	memset(p->hist, 0, 2U * (p->l - 1U) * sizeof(float));
	p->fill = p->l - 1U;
	p->next = index;
	p->full = index + p->l - 1U;
	p->nm = (index + p->d - 1U) / p->d;
	p->nn = p->nm * p->d;
	p->live = true;
	p->gap = true;
}

/* A gap shorter than L up to @p index: zeros, and no frames inside it. */
static void fill_gap(struct esdr_pfb *p, uint64_t index)
{
	uint32_t g = (uint32_t)(index - p->next);

	/* First, so that a slide does not keep the windows of frames that will not come. */
	while (p->nn < index) {
		p->nm++;
		p->nn += p->d;
		p->gap = true;
	}
	p->full = index + p->l - 1U;
	while (g > 0U) {
		uint32_t n;

		if (p->fill == p->cap) {
			slide(p);
		}
		n = p->cap - p->fill < g ? p->cap - p->fill : g;
		memset(p->hist + 2U * p->fill, 0, 2U * n * sizeof(float));
		p->fill += n;
		p->next += n;
		g -= n;
	}
}

/* Items @p q8 become item sc + oi (I) or item sc + oq (Q), else floats @p qf are copied. */
static int push(struct esdr_pfb *p, uint64_t index, const int8_t *q8, const float *qf,
		size_t pairs, float sc, float oi, float oq, esdr_pfb_frame_cb cb, void *user)
{
	int frames = 0;

	if (pairs == 0U) {
		return 0;
	}
	if (!p->live) {
		start(p, index);
	} else if (index != p->next) {
		if (index < p->next) {
			return -ERANGE;
		}
		if (index - p->next >= p->l) {
			start(p, index);
		} else {
			fill_gap(p, index);
		}
	}
	while (pairs > 0U) {
		size_t n;
		float *x;

		if (p->fill == p->cap) {
			slide(p);
		}
		n = p->cap - p->fill < pairs ? p->cap - p->fill : pairs;
		x = p->hist + 2U * p->fill;
		if (q8 != NULL) {
			size_t i = 0;

			/* A multiply-add per item, the offset its addend; i is even: I, Q, I, Q. */
			for (; i + 4U <= 2U * n; i += 4U) {
				x[i] = (float)q8[i] * sc + oi;
				x[i + 1U] = (float)q8[i + 1U] * sc + oq;
				x[i + 2U] = (float)q8[i + 2U] * sc + oi;
				x[i + 3U] = (float)q8[i + 3U] * sc + oq;
			}
			if (i < 2U * n) {
				x[i] = (float)q8[i] * sc + oi;
				x[i + 1U] = (float)q8[i + 1U] * sc + oq;
			}
			q8 += 2U * n;
		} else {
			memcpy(x, qf, 2U * n * sizeof(float));
			qf += 2U * n;
		}
		p->fill += (uint32_t)n;
		p->next += n;
		pairs -= n;
		frames += produce(p, cb, user, pairs == 0U);
	}
	return frames;
}

int esdr_pfb_push(struct esdr_pfb *p, uint64_t index, const int8_t *iq, size_t pairs,
		  unsigned int shift, int32_t dc_i, int32_t dc_q, esdr_pfb_frame_cb cb,
		  void *user)
{
	float half;

	if (shift > 15U) {
		return -EINVAL;
	}
	/* The C6's shift floors: an item stands for the middle of its step, (2^s - 1) / 2 above. */
	half = 0.5f * (float)((1U << shift) - 1U);
	return push(p, index, iq, NULL, pairs, (float)(1U << shift), half - (float)dc_i,
		    half - (float)dc_q, cb, user);
}

int esdr_pfb_push_f32(struct esdr_pfb *p, uint64_t index, const float *iq, size_t pairs,
		      esdr_pfb_frame_cb cb, void *user)
{
	return push(p, index, NULL, iq, pairs, 1.0f, 0.0f, 0.0f, cb, user);
}

/* ---- burst detector ---- */

struct det_layout {
	size_t slot, floor, learn, pw, learnt, open, quiet, loud, sent, use, total;
};

static bool det_layout(const struct esdr_det_cfg *cfg, struct det_layout *lo)
{
	size_t m, b;

	if (cfg == NULL || cfg->channels == 0U || cfg->channels > ESDR_PFB_M_MAX ||
	    cfg->channels % 4U != 0U || cfg->block == 1U || cfg->block > 1024U ||
	    cfg->open_ratio < 0.0f) {
		return false;
	}
	m = cfg->channels;
	b = cfg->block != 0U ? cfg->block : 16U;
	lo->slot = 0;
	lo->sent = lo->slot + ALIGN16(2U * b * sizeof(struct esdr_det_slot));
	lo->use = lo->sent + ALIGN16(m * sizeof(uint64_t));
	lo->floor = lo->use + ALIGN16(b * sizeof(const float *));
	lo->learn = lo->floor + ALIGN16(m * sizeof(float));
	lo->pw = lo->learn + ALIGN16(m * ESDR_DET_LEARN * sizeof(float));
	lo->loud = lo->pw + ALIGN16(m * sizeof(float));
	lo->quiet = lo->loud + ALIGN16(m * sizeof(uint32_t));
	lo->learnt = lo->quiet + ALIGN16(m * sizeof(uint16_t));
	lo->open = lo->learnt + ALIGN16(m);
	lo->total = lo->open + ALIGN16(m);
	return true;
}

size_t esdr_det_mem_size(const struct esdr_det_cfg *cfg)
{
	struct det_layout lo;

	return det_layout(cfg, &lo) ? lo.total + 16U : 0U;
}

int esdr_det_init(struct esdr_det *d, const struct esdr_det_cfg *cfg, void *mem, size_t len)
{
	struct det_layout lo;
	uint8_t *base;

	if (d == NULL || mem == NULL || ((uintptr_t)mem & 3U) != 0U || !det_layout(cfg, &lo)) {
		return -EINVAL;
	}
	base = (uint8_t *)ALIGN16((uintptr_t)mem);
	if ((size_t)(base - (uint8_t *)mem) + lo.total > len) {
		return -ENOMEM;
	}
	memset(d, 0, sizeof(*d));
	d->m = cfg->channels;
	d->b = cfg->block != 0U ? cfg->block : 16U;
	d->hang = cfg->hang != 0U ? cfg->hang : 4U;
	d->nslot = (uint16_t)(2U * d->b);
	d->ratio = cfg->open_ratio != 0.0f ? cfg->open_ratio : 4.0f;
	d->slot = (struct esdr_det_slot *)(base + lo.slot);
	d->sent = (uint64_t *)(base + lo.sent);
	d->use = (const float **)(base + lo.use);
	d->floor = (float *)(base + lo.floor);
	d->learn = (float *)(base + lo.learn);
	d->pw = (float *)(base + lo.pw);
	d->loud = (uint32_t *)(base + lo.loud);
	d->quiet = (uint16_t *)(base + lo.quiet);
	d->learnt = base + lo.learnt;
	d->open = base + lo.open;
	esdr_det_reset(d);
	return 0;
}

void esdr_det_reset(struct esdr_det *d)
{
	for (unsigned int k = 0; k < d->m; k++) {
		d->floor[k] = 0.0f;
		d->learnt[k] = 0;
		d->open[k] = 0;
		d->quiet[k] = 0;
		d->loud[k] = 0;
		d->sent[k] = 0;
	}
	d->have_cur = false;
	d->have_prev = false;
	d->have_last = false;
	d->wr = 0;
}

static inline uint16_t det_wrap(const struct esdr_det *d, uint32_t s)
{
	return (uint16_t)(s >= d->nslot ? s - d->nslot : s);
}

/* The lower quartile of a channel's learning blocks (sorts them). */
static float lower_quartile(float *v)
{
	for (unsigned int i = 1; i < ESDR_DET_LEARN; i++) {
		float x = v[i];
		unsigned int j = i;

		for (; j > 0U && v[j - 1U] > x; j--) {
			v[j] = v[j - 1U];
		}
		v[j] = x;
	}
	return v[ESDR_DET_LEARN / 4U];
}

/* Mean power per channel over the @p n frames in d->use. */
static void block_power(struct esdr_det *d, unsigned int n)
{
	const float inv = 1.0f / (float)n;

	for (unsigned int k = 0; k < d->m; k += 4U) {
		float p0 = 0.0f, p1 = 0.0f, p2 = 0.0f, p3 = 0.0f;

		for (unsigned int i = 0; i < n; i++) {
			const float *y = d->use[i] + 2U * k;

			p0 += y[0] * y[0];
			p0 += y[1] * y[1];
			p1 += y[2] * y[2];
			p1 += y[3] * y[3];
			p2 += y[4] * y[4];
			p2 += y[5] * y[5];
			p3 += y[6] * y[6];
			p3 += y[7] * y[7];
		}
		d->pw[k] = p0 * inv;
		d->pw[k + 1U] = p1 * inv;
		d->pw[k + 2U] = p2 * inv;
		d->pw[k + 3U] = p3 * inv;
	}
}

/*
 * Channel @p k's frames of the current block, with @p pre the block before too, less what it
 * sent already.
 */
static void det_send(struct esdr_det *d, unsigned int k, uint16_t flags, bool pre, float power,
		     esdr_det_cb cb, void *user)
{
	struct esdr_det_ev ev = {
		.chan = (uint16_t)k,
		.flags = flags,
		.first = pre ? d->prev_first : d->cur_first,
		.n = pre ? (uint32_t)d->prev_n + d->cur_n : d->cur_n,
		.power = power,
		.floor = d->floor[k],
	};

	/* A channel that closed and opened again: those frames went out. */
	while (ev.n > 0U && d->slot[ev.first].m < d->sent[k]) {
		ev.first = det_wrap(d, ev.first + 1U);
		ev.n--;
	}
	if (cb != NULL) {
		cb(d, &ev, user);
	}
	d->sent[k] = d->cur_start + d->b;
	if ((flags & ESDR_DET_CLOSE) != 0U) {
		d->open[k] = 0;
	}
}

/* The current block's decisions and events; it becomes the previous one. */
static void finish(struct esdr_det *d, esdr_det_cb cb, void *user)
{
	const bool adj = d->have_prev && d->prev_start + d->b == d->cur_start;
	unsigned int nuse = 0;
	bool valid;

	for (unsigned int i = 0; i < d->cur_n; i++) {
		const struct esdr_det_slot *s = &d->slot[det_wrap(d, d->cur_first + i)];

		if ((s->flags & ESDR_PFB_COMPLETE) != 0U) {
			d->use[nuse++] = s->iq;
		}
	}
	/* Blocks of mostly transients (a run's first frames) decide nothing. */
	valid = 2U * nuse >= d->b;
	if (valid) {
		block_power(d, nuse);
	}
	for (unsigned int k = 0; k < d->m; k++) {
		const float pw = valid ? d->pw[k] : 0.0f;
		const bool loud = pw > d->floor[k] * d->ratio;
		uint16_t flags = 0;

		if (d->open[k] == 0U) {
			if (!valid) {
				continue;
			}
			if (d->learnt[k] < ESDR_DET_LEARN) {
				d->learn[k * ESDR_DET_LEARN + d->learnt[k]++] = pw;
				if (d->learnt[k] == ESDR_DET_LEARN) {
					d->floor[k] = lower_quartile(&d->learn[k * ESDR_DET_LEARN]);
				}
				continue;
			}
			if (!loud) {
				d->floor[k] += (pw - d->floor[k]) * (1.0f / DET_FOLLOW);
				continue;
			}
			d->open[k] = 1;
			d->quiet[k] = 0;
			d->loud[k] = 1;
			flags = ESDR_DET_OPEN;
		} else if (!valid) {
			/* Nothing to decide: the frames go out. */
		} else if (loud) {
			d->quiet[k] = 0;
			if (++d->loud[k] >= DET_RELEARN) {
				/* Loud for too long: what it hears is the floor now. */
				d->learnt[k] = 0;
				flags = ESDR_DET_CLOSE;
			}
		} else if (++d->quiet[k] >= d->hang) {
			flags = ESDR_DET_CLOSE;
		}
		det_send(d, k, flags, adj && flags == ESDR_DET_OPEN, pw, cb, user);
	}
	d->prev_start = d->cur_start;
	d->prev_first = d->cur_first;
	d->prev_n = d->cur_n;
	d->have_prev = true;
	d->have_cur = false;
}

void esdr_det_frame(struct esdr_det *d, uint64_t m, const float *iq, uint32_t flags,
		    esdr_det_cb cb, void *user)
{
	if (d->have_last && m <= d->last_m) {
		/* The stream went back (a new stream): no block or frame of before applies. */
		if (d->have_cur) {
			finish(d, cb, user);
		}
		d->have_prev = false;
		for (unsigned int k = 0; k < d->m; k++) {
			d->sent[k] = 0;
		}
	} else if (d->have_cur && m >= d->cur_start + d->b) {
		finish(d, cb, user);
	}
	if (!d->have_cur) {
		uint64_t pend = d->prev_start + d->b;

		d->cur_start = d->have_prev && m >= pend && m < pend + d->b ? pend : m - m % d->b;
		d->cur_first = d->wr;
		d->cur_n = 0;
		d->have_cur = true;
	}
	d->slot[d->wr] = (struct esdr_det_slot){.iq = iq, .m = m, .flags = flags};
	d->wr = det_wrap(d, d->wr + 1U);
	d->cur_n++;
	d->last_m = m;
	d->have_last = true;
}

void esdr_det_flush(struct esdr_det *d, esdr_det_cb cb, void *user)
{
	if (d->have_cur) {
		finish(d, cb, user);
	}
}

unsigned int esdr_det_take(const struct esdr_det *d, const struct esdr_det_ev *ev,
			   uint32_t *pos, float *iq, unsigned int max, uint64_t *m0,
			   uint32_t *flags)
{
	const size_t c = 2U * ev->chan;
	unsigned int n = 0;
	uint16_t s;
	uint64_t want;
	uint32_t done;

	if (*pos >= ev->n || max == 0U) {
		return 0;
	}
	s = det_wrap(d, ev->first + *pos);
	*m0 = d->slot[s].m;
	*flags = d->slot[s].flags;
	done = d->slot[s].flags & ESDR_PFB_COMPLETE;
	want = *m0;
	while (*pos < ev->n && n < max) {
		const struct esdr_det_slot *sl = &d->slot[s];

		if (sl->m != want || (sl->flags & ESDR_PFB_COMPLETE) != done) {
			break;
		}
		iq[2U * n] = sl->iq[c];
		iq[2U * n + 1U] = sl->iq[c + 1U];
		n++;
		want++;
		(*pos)++;
		s = det_wrap(d, s + 1U);
	}
	return n;
}
