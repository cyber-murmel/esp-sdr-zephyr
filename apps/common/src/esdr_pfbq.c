/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Fixed-point polyphase channelizer and burst detector on quads (esdr_pfbq.h).
 *
 * The bank is esdr_pfb.c's (see there for the split into branch sums and a DFT), computed for
 * the four frames of a quad at once: a vector of 8 int16 holds one complex value of each frame,
 * so that every step of the FFT is the scalar algorithm on vectors.
 *
 * Branch sums: for the frame with newest pair N and the branches r = 4 g .. 4 g + 3, tap p
 * reads the pairs N - 4 g - 3 - p M .. N - 4 g - p M, four consecutive pairs of the history: one
 * vector, against a vector of the four branches' taps (each twice, for I and Q), T multiply-adds
 * into the 40-bit accumulators. Frames are D apart and D and M are multiples of 4, so with pair
 * i at a history position of i + 3 mod 4 every load is aligned. The four frames' vectors are
 * transposed (lanes become frames) and stored at their branches' FFT input positions.
 *
 * DFT: every frame uses an even frame's input order ((M - r) mod M, digit reversed); an odd
 * frame's channel k then comes out (-1)^k times its value, which a pass over the odd channels
 * undoes.
 *
 * The C kernels here are the reference: each step is the arithmetic of the PIE instruction that
 * does it in esdr_pfbq_pie.S (exact products and sums, shifts that floor, saturation to int16),
 * so both compute the same values.
 */

#include <errno.h>
#include <math.h>
#include <string.h>

#include <esp_sdr/esp_sdr_dsp.h>

#include "esdr_pfbq.h"

#define ALIGN16(x) (((x) + 15U) & ~(size_t)15U)

/* int16 in a vector: 4 frames, I and Q. */
#define VEC 8U

/*
 * Detector: floor follower over quiet blocks; a quiet block this many times below the floor
 * replaces it (10 dB: noise of a right floor does not get there, even in a block of B / 2
 * frames); blocks loud in a row before a channel relearns.
 */
#define DET_FOLLOW  64.0f
#define DET_SNAP    10.0f
#define DET_RELEARN 2048U

/* The power kernel's sums are shifted down by this much to fit 32 bits (4 quads). */
#define POWER_SHIFT 6U

/* Radix-5 butterfly constants, Q14, in the lanes the kernels select: 1, cos(tau / 5),
 * cos(2 tau / 5), sin(tau / 5), sin(2 tau / 5), their negatives; its rounding addend; -j, Q15.
 */
enum { K5_ONE, K5_C1, K5_C2, K5_S1, K5_S2, K5_NS1, K5_NS2 };
#define R5_SHIFT 14U

struct consts {
	int16_t k5[VEC], r5[VEC], j[VEC], sign[VEC];
};

static const struct consts kc __attribute__((aligned(16))) = {
	.k5 = {16384, 5063, -13255, 15582, 9630, -15582, -9630, 0},
	.r5 = {8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192},
	.j = {0, -32768, 0, -32768, 0, -32768, 0, -32768},
	.sign = {1, 1, -1, -1, 1, 1, -1, -1},
};

/* ---- kernels ---- */

/* Arguments of the branch kernel; esdr_pfbq_pie.S reads them at these offsets (ILP32). */
struct bk_args {
	const int16_t *x0;
	int16_t *work;
	const int16_t *taps;
	const uint16_t *off;
	const int16_t *rnd;
	int32_t dstep, tstep;
	uint32_t groups, t, sh;
};

struct esdr_pfbq_kern {
	/* pairs int8 pairs to int16: item * kv[0] + kv[8] (I) or kv[9] (Q). */
	void (*conv)(int16_t *dst, const int8_t *src, uint32_t pairs, const int16_t *kv);
	void (*branch)(const struct bk_args *a);
	/* First FFT stages of radix 5 and 4: n butterflies of consecutive vectors, src to dst. */
	void (*r5)(int16_t *dst, const int16_t *src, uint32_t n, const struct consts *c);
	void (*r4f)(int16_t *dst, const int16_t *src, uint32_t n, const struct consts *c);
	/* Later stages in place: span vectors apart, twiddles per column j = 1 .. span - 1. */
	void (*r4)(int16_t *buf, uint32_t m, uint32_t span, const int16_t *tw,
		   const struct consts *c);
	void (*r2)(int16_t *buf, uint32_t m, uint32_t span, const int16_t *tw);
	/* The last stage, one group (span = M / radix). */
	void (*r4l)(int16_t *buf, uint32_t span, const int16_t *tw, const struct consts *c);
	void (*r2l)(int16_t *buf, uint32_t span, const int16_t *tw);
	/* Odd channels' odd frames negated. */
	void (*flip)(int16_t *buf, uint32_t m, const struct consts *c);
	/* pw[k]: channel k's I^2 + Q^2 over the four quads, >> POWER_SHIFT, saturated. */
	void (*power4)(const int16_t *const *q, uint32_t m, uint32_t *pw);
	/* n16 vectors from src down to dst (dst <= src), ascending: the history's slide. */
	void (*move)(int16_t *dst, const int16_t *src, uint32_t n16);
};

static inline int16_t sat16(int64_t v)
{
	return (int16_t)(v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v);
}

/* v >> s rounding down, as the PIE shifts do (C leaves >> of negatives to the compiler). */
static inline int64_t asr(int64_t v, unsigned int s)
{
	return v >= 0 ? v >> s : -(int64_t)((uint64_t)(-(v + 1)) >> s) - 1;
}

static void conv_c(int16_t *dst, const int8_t *src, uint32_t pairs, const int16_t *kv)
{
	for (uint32_t i = 0; i < 2U * pairs; i++) {
		dst[i] = sat16((int32_t)sat16((int32_t)src[i] * kv[0]) + kv[8U + (i & 1U)]);
	}
}

static void branch_c(const struct bk_args *a)
{
	const ptrdiff_t dp = a->dstep / 2, tp = a->tstep / 2;

	for (uint32_t g = 0; g < a->groups; g++) {
		for (unsigned int q = 0; q < 4U; q++) {
			const int16_t *x = a->x0 + (ptrdiff_t)q * dp - 8 * (ptrdiff_t)g;
			const int16_t *h = a->taps + (size_t)g * a->t * VEC;
			int64_t acc[VEC];

			for (unsigned int i = 0; i < VEC; i++) {
				acc[i] = a->rnd[i];
			}
			for (uint32_t t = 0; t < a->t; t++, h += VEC, x += tp) {
				for (unsigned int i = 0; i < VEC; i++) {
					acc[i] += (int32_t)h[i] * x[i];
				}
			}
			for (unsigned int j = 0; j < 4U; j++) {
				int16_t *o = a->work + a->off[4U * g + j] / 2U + 2U * q;

				o[0] = sat16(asr(acc[2U * j], a->sh));
				o[1] = sat16(asr(acc[2U * j + 1U], a->sh));
			}
		}
	}
}

static void v_add(int16_t *d, const int16_t *a, const int16_t *b)
{
	for (unsigned int i = 0; i < VEC; i++) {
		d[i] = sat16((int32_t)a[i] + b[i]);
	}
}

static void v_sub(int16_t *d, const int16_t *a, const int16_t *b)
{
	for (unsigned int i = 0; i < VEC; i++) {
		d[i] = sat16((int32_t)a[i] - b[i]);
	}
}

/* d = a b per complex lane, b in Q15 (the cmul's two halves at SAR 15). */
static void v_cmul(int16_t *d, const int16_t *a, const int16_t *b)
{
	int16_t o[VEC];

	for (unsigned int c = 0; c < VEC; c += 2U) {
		int64_t ar = a[c], ai = a[c + 1U], br = b[c], bi = b[c + 1U];

		o[c] = sat16(asr(ar * br - ai * bi, 15U));
		o[c + 1U] = sat16(asr(ar * bi + ai * br, 15U));
	}
	memcpy(d, o, sizeof(o));
}

/* Radix 4: X0 = t0 + t2, X2 = t0 - t2, X1 = t1 - j t3, X3 = t1 + j t3. */
static void bf4_c(int16_t *x0, int16_t *x1, int16_t *x2, int16_t *x3, const int16_t *tw,
		  const struct consts *c)
{
	int16_t a1[VEC], a2[VEC], a3[VEC], t0[VEC], t1[VEC], t2[VEC], t3[VEC];

	memcpy(a1, x1, sizeof(a1));
	memcpy(a2, x2, sizeof(a2));
	memcpy(a3, x3, sizeof(a3));
	if (tw != NULL) {
		v_cmul(a1, a1, tw);
		v_cmul(a2, a2, tw + VEC);
		v_cmul(a3, a3, tw + 2U * VEC);
	}
	v_add(t0, x0, a2);
	v_sub(t1, x0, a2);
	v_add(t2, a1, a3);
	v_sub(t3, a1, a3);
	v_cmul(t3, t3, c->j);
	v_add(x0, t0, t2);
	v_sub(x2, t0, t2);
	v_add(x1, t1, t3);
	v_sub(x3, t1, t3);
}

static void r4f_c(int16_t *dst, const int16_t *src, uint32_t n, const struct consts *c)
{
	for (uint32_t b = 0; b < n; b++, src += 4U * VEC, dst += 4U * VEC) {
		memmove(dst, src, 4U * VEC * sizeof(int16_t));
		bf4_c(dst, dst + VEC, dst + 2U * VEC, dst + 3U * VEC, NULL, c);
	}
}

static void r4_c(int16_t *buf, uint32_t m, uint32_t span, const int16_t *tw,
		 const struct consts *c)
{
	const size_t s = (size_t)span * VEC;

	for (uint32_t j = 0; j < span; j++) {
		const int16_t *w = j == 0U ? NULL : tw + (size_t)(j - 1U) * 3U * VEC;

		for (uint32_t g = j; g < m; g += 4U * span) {
			int16_t *x = buf + (size_t)g * VEC;

			bf4_c(x, x + s, x + 2U * s, x + 3U * s, w, c);
		}
	}
}

static void r2_c(int16_t *buf, uint32_t m, uint32_t span, const int16_t *tw)
{
	const size_t s = (size_t)span * VEC;

	for (uint32_t j = 0; j < span; j++) {
		for (uint32_t g = j; g < m; g += 2U * span) {
			int16_t *x = buf + (size_t)g * VEC, b[VEC];

			memcpy(b, x + s, sizeof(b));
			if (j != 0U) {
				v_cmul(b, b, tw + (size_t)(j - 1U) * VEC);
			}
			v_sub(x + s, x, b);
			v_add(x, x, b);
		}
	}
}

/* One output lane of a radix-5 butterfly: the five products summed exactly, rounded once. */
static inline int16_t r5_lane(const struct consts *c, int16_t a0, int16_t b1, int k1, int16_t b2,
			      int k2, int16_t j1, int k3, int16_t j2, int k4)
{
	int64_t s = c->r5[0];

	s += (int32_t)a0 * c->k5[K5_ONE];
	s += (int32_t)b1 * c->k5[k1];
	s += (int32_t)b2 * c->k5[k2];
	s += (int32_t)j1 * c->k5[k3];
	s += (int32_t)j2 * c->k5[k4];
	return sat16(asr(s, R5_SHIFT));
}

/*
 * Radix 5 (esdr_pfb.c's bfly5): with b1, d1 = a1 +/- a4, b2, d2 = a2 +/- a3 and j1, j2 = -j d1,
 * -j d2: X1, X4 = a0 + C1 b1 + C2 b2 +/- (S1 j1 + S2 j2), X2, X3 = a0 + C2 b1 + C1 b2 +/-
 * (S2 j1 - S1 j2).
 */
static void r5_c(int16_t *dst, const int16_t *src, uint32_t n, const struct consts *c)
{
	for (uint32_t b = 0; b < n; b++, src += 5U * VEC, dst += 5U * VEC) {
		const int16_t *a0 = src;
		int16_t b1[VEC], d1[VEC], b2[VEC], d2[VEC], x0[VEC];

		v_add(b1, src + VEC, src + 4U * VEC);
		v_sub(d1, src + VEC, src + 4U * VEC);
		v_add(b2, src + 2U * VEC, src + 3U * VEC);
		v_sub(d2, src + 2U * VEC, src + 3U * VEC);
		v_cmul(d1, d1, c->j);
		v_cmul(d2, d2, c->j);
		v_add(x0, a0, b1);
		v_add(x0, x0, b2);
		for (unsigned int i = 0; i < VEC; i++) {
			int16_t o1 = r5_lane(c, a0[i], b1[i], K5_C1, b2[i], K5_C2, d1[i], K5_S1, d2[i],
					     K5_S2);
			int16_t o4 = r5_lane(c, a0[i], b1[i], K5_C1, b2[i], K5_C2, d1[i], K5_NS1,
					     d2[i], K5_NS2);
			int16_t o2 = r5_lane(c, a0[i], b1[i], K5_C2, b2[i], K5_C1, d1[i], K5_S2, d2[i],
					     K5_NS1);
			int16_t o3 = r5_lane(c, a0[i], b1[i], K5_C2, b2[i], K5_C1, d1[i], K5_NS2,
					     d2[i], K5_S1);

			dst[VEC + i] = o1;
			dst[4U * VEC + i] = o4;
			dst[2U * VEC + i] = o2;
			dst[3U * VEC + i] = o3;
		}
		memcpy(dst, x0, sizeof(x0));
	}
}

static void flip_c(int16_t *buf, uint32_t m, const struct consts *c)
{
	for (uint32_t k = 1; k < m; k += 2U) {
		int16_t *y = buf + (size_t)k * VEC;

		for (unsigned int i = 0; i < VEC; i++) {
			y[i] = sat16((int32_t)y[i] * c->sign[i]);
		}
	}
}

static void power4_c(const int16_t *const *q, uint32_t m, uint32_t *pw)
{
	for (uint32_t k = 0; k < m; k++) {
		int64_t s = 0;

		for (unsigned int j = 0; j < 4U; j++) {
			const int16_t *y = q[j] + (size_t)k * VEC;

			for (unsigned int i = 0; i < VEC; i++) {
				s += (int32_t)y[i] * y[i];
			}
		}
		s = asr(s, POWER_SHIFT);
		pw[k] = s > INT32_MAX ? (uint32_t)INT32_MAX : (uint32_t)s;
	}
}

static void r4l_c(int16_t *buf, uint32_t span, const int16_t *tw, const struct consts *c)
{
	r4_c(buf, 4U * span, span, tw, c);
}

static void r2l_c(int16_t *buf, uint32_t span, const int16_t *tw)
{
	r2_c(buf, 2U * span, span, tw);
}

static void move_c(int16_t *dst, const int16_t *src, uint32_t n16)
{
	memmove(dst, src, (size_t)n16 * VEC * sizeof(int16_t));
}

static const struct esdr_pfbq_kern kern_c = {
	.conv = conv_c,
	.branch = branch_c,
	.r5 = r5_c,
	.r4f = r4f_c,
	.r4 = r4_c,
	.r2 = r2_c,
	.r4l = r4l_c,
	.r2l = r2l_c,
	.flip = flip_c,
	.power4 = power4_c,
	.move = move_c,
};

#if defined(ESDR_PFBQ_PIE)
/* esdr_pfbq_pie.S */
void esdr_pfbq_pie_conv(int16_t *dst, const int8_t *src, uint32_t pairs, const int16_t *kv);
void esdr_pfbq_pie_branch(const struct bk_args *a);
void esdr_pfbq_pie_r5(int16_t *dst, const int16_t *src, uint32_t n, const struct consts *c);
void esdr_pfbq_pie_r4f(int16_t *dst, const int16_t *src, uint32_t n, const struct consts *c);
void esdr_pfbq_pie_r4(int16_t *buf, uint32_t m, uint32_t span, const int16_t *tw,
		      const struct consts *c);
void esdr_pfbq_pie_r2(int16_t *buf, uint32_t m, uint32_t span, const int16_t *tw);
void esdr_pfbq_pie_r4l(int16_t *buf, uint32_t span, const int16_t *tw, const struct consts *c);
void esdr_pfbq_pie_r2l(int16_t *buf, uint32_t span, const int16_t *tw);
void esdr_pfbq_pie_flip(int16_t *buf, uint32_t m, const struct consts *c);
void esdr_pfbq_pie_power4(const int16_t *const *q, uint32_t m, uint32_t *pw);
void esdr_pfbq_pie_move(int16_t *dst, const int16_t *src, uint32_t n16);

/* PIE and the hardware loops on for this core, unaligned vector access allowed. */
void esdr_pfbq_pie_on(void);

static const struct esdr_pfbq_kern kern_pie = {
	.conv = esdr_pfbq_pie_conv,
	.branch = esdr_pfbq_pie_branch,
	.r5 = esdr_pfbq_pie_r5,
	.r4f = esdr_pfbq_pie_r4f,
	.r4 = esdr_pfbq_pie_r4,
	.r2 = esdr_pfbq_pie_r2,
	.r4l = esdr_pfbq_pie_r4l,
	.r2l = esdr_pfbq_pie_r2l,
	.flip = esdr_pfbq_pie_flip,
	.power4 = esdr_pfbq_pie_power4,
	.move = esdr_pfbq_pie_move,
};
#define KERN (&kern_pie)
#else
#define KERN (&kern_c)
#endif

bool esdr_pfbq_pie(void)
{
	return KERN != &kern_c;
}

int esdr_pfbq_kernels(struct esdr_pfbq *p, bool pie)
{
	if (pie && !esdr_pfbq_pie()) {
		return -ENOTSUP;
	}
	p->k = pie ? KERN : &kern_c;
	return 0;
}

/* ---- set-up ---- */

/* Radices in stage order, the largest first (esdr_pfb.c's plan). */
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

/* Twiddle vectors of stage @p s: (span - 1) (radix - 1). */
static size_t tw_count(const uint8_t *radix, unsigned int s)
{
	size_t span = 1;

	for (unsigned int i = 0; i < s; i++) {
		span *= radix[i];
	}
	return (span - 1U) * (radix[s] - 1U);
}

/* Where natural input i sits for the stages: the last stage's digit is the most significant. */
static unsigned int digit_rev(const struct esdr_pfbq *p, unsigned int i)
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

struct layout {
	size_t taps, hist, work, ring, tw, off, rnd, total;
	uint32_t cap;
};

static bool check_cfg(const struct esdr_pfbq_cfg *cfg, unsigned int *t, uint8_t radix[4],
		      unsigned int *stages)
{
	*t = cfg->taps != 0U ? cfg->taps : ESDR_PFB_T_DEFAULT;
	*stages = plan(cfg->channels, radix);
	return *stages != 0U && *t % 4U == 0U && *t <= ESDR_PFBQ_T_MAX && cfg->stop_db >= 0.0f &&
	       cfg->gain >= 0.0f && (cfg->chunk == 0U || cfg->chunk >= 4U * cfg->channels) &&
	       cfg->chunk <= (1U << 24);
}

static bool get_layout(const struct esdr_pfbq_cfg *cfg, struct layout *lo)
{
	uint8_t radix[4];
	unsigned int t, stages;
	size_t m, l, d, chunk, quads, tw = 0;

	if (cfg == NULL || !check_cfg(cfg, &t, radix, &stages)) {
		return false;
	}
	m = cfg->channels;
	l = m * t;
	d = m / 2U;
	chunk = cfg->chunk != 0U ? cfg->chunk : 2U * l;
	quads = cfg->quads != 0U ? cfg->quads : 1U;
	for (unsigned int s = 0; s < stages; s++) {
		tw += tw_count(radix, s);
	}
	/* A run's first quad reaches 4 D before its start; 3 pairs of alignment. */
	lo->cap = (uint32_t)(l - 1U + 4U * d + 3U + chunk);
	lo->taps = 0;
	lo->hist = lo->taps + ALIGN16(m * t * 2U * sizeof(int16_t));
	/* A flush writes up to 4 D zeros past the fill; the prototype is designed in here. */
	lo->work = lo->hist + ALIGN16(2U * (lo->cap + 4U * d) * sizeof(int16_t) + l * sizeof(float));
	lo->ring = lo->work + m * VEC * sizeof(int16_t);
	lo->tw = lo->ring + quads * m * VEC * sizeof(int16_t);
	lo->off = lo->tw + tw * VEC * sizeof(int16_t);
	lo->rnd = lo->off + ALIGN16(m * sizeof(uint16_t));
	lo->total = lo->rnd + VEC * sizeof(int16_t);
	return true;
}

size_t esdr_pfbq_mem_size(const struct esdr_pfbq_cfg *cfg)
{
	struct layout lo;

	return get_layout(cfg, &lo) ? lo.total + 16U : 0U;
}

static int16_t q15(double v)
{
	double s = floor(v * 32768.0 + 0.5);

	return (int16_t)(s > 32767.0 ? 32767 : s < -32768.0 ? -32768 : (int)s);
}

int esdr_pfbq_init(struct esdr_pfbq *p, const struct esdr_pfbq_cfg *cfg, void *mem, size_t len)
{
	struct layout lo;
	unsigned int t, stages, qt;
	uint8_t *base;
	float *h, top = 0.0f;
	int16_t *tw;

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
	p->quads = cfg->quads != 0U ? cfg->quads : 1U;
	p->cap = lo.cap;
	p->taps = (int16_t *)(base + lo.taps);
	p->hist = (int16_t *)(base + lo.hist);
	p->work = (int16_t *)(base + lo.work);
	p->ring = (int16_t *)(base + lo.ring);
	p->tw = (int16_t *)(base + lo.tw);
	p->off = (uint16_t *)(base + lo.off);

	/* The prototype, in the history's room (scratch until the first push). */
	h = (float *)(void *)(p->hist + 2U * (p->cap + 4U * p->d));
	esdr_pfb_proto(h, p->m, t, cfg->stop_db != 0.0f ? cfg->stop_db : 60.0f,
		       cfg->gain != 0.0f ? cfg->gain : 1.0f);
	for (unsigned int n = 0; n < p->l; n++) {
		top = fabsf(h[n]) > top ? fabsf(h[n]) : top;
	}
	/* The largest scale up to 14 that keeps the taps in int16 (the rounding addend too). */
	for (qt = 14U; qt > 0U && (double)top * (double)(1U << qt) > 32767.0; qt--) {
	}
	if ((double)top * (double)(1U << qt) > 32767.0) {
		return -EINVAL;
	}
	p->sh = (uint8_t)(qt + 1U);
	for (unsigned int g = 0; g < p->m / 4U; g++) {
		for (unsigned int k = 0; k < t; k++) {
			int16_t *v = p->taps + (size_t)(g * t + k) * VEC;

			for (unsigned int j = 0; j < 4U; j++) {
				double x = (double)h[4U * g + 3U - j + k * p->m] * (double)(1U << qt);
				int16_t q = (int16_t)floor(x + 0.5);

				v[2U * j] = q;
				v[2U * j + 1U] = q;
			}
		}
	}
	p->rnd = (int16_t *)(base + lo.rnd);
	for (unsigned int i = 0; i < VEC; i++) {
		p->rnd[i] = (int16_t)(1U << (p->sh - 1U));
	}

	/* Twiddles of the stages after the first, each replicated over the 4 frames. */
	tw = p->tw;
	for (unsigned int s = 0, span = 1; s < stages; span *= p->radix[s], s++) {
		unsigned int rdx = p->radix[s], ls = span * rdx;

		p->tws[s] = tw;
		for (unsigned int j = 1; j < span; j++) {
			for (unsigned int q = 1; q < rdx; q++) {
				double a = -ESP_SDR_TAU * (double)((q * j) % ls) / (double)ls;
				int16_t wr = q15(cos(a)), wi = q15(sin(a));

				for (unsigned int i = 0; i < VEC; i += 2U) {
					tw[i] = wr;
					tw[i + 1U] = wi;
				}
				tw += VEC;
			}
		}
	}
	for (unsigned int g = 0; g < p->m / 4U; g++) {
		for (unsigned int j = 0; j < 4U; j++) {
			unsigned int r = 4U * g + 3U - j;

			p->off[4U * g + j] =
				(uint16_t)(digit_rev(p, (p->m - r) % p->m) * VEC * sizeof(int16_t));
		}
	}
#if defined(ESDR_PFBQ_PIE)
	esdr_pfbq_pie_on();
#endif
	p->k = KERN;
	return 0;
}

int16_t esdr_pfbq_tap(const struct esdr_pfbq *p, unsigned int n)
{
	unsigned int r = n % p->m, k = n / p->m;

	if (n >= p->l) {
		return 0;
	}
	return p->taps[(size_t)((r / 4U) * p->t + k) * VEC + 2U * (3U - r % 4U)];
}

unsigned int esdr_pfbq_qt(const struct esdr_pfbq *p)
{
	return p->sh - 1U;
}

void esdr_pfbq_reset(struct esdr_pfbq *p)
{
	p->live = false;
}

/* ---- running ---- */

#if defined(ESDR_PFBQ_PROFILE)
/* Cycles in the branch sums, the FFTs, the conversion, the detector's block powers; the app
 * supplies the clock.
 */
uint64_t esdr_pfbq_prof[4];
uint64_t esdr_pfb_now(void);
#define PROF_START() uint64_t prof_t = esdr_pfb_now()
#define PROF_END(k)  (esdr_pfbq_prof[k] += esdr_pfb_now() - prof_t)
#else
#define PROF_START() do { } while (0)
#define PROF_END(k)  do { } while (0)
#endif

/* The FFT of the branch sums in work into @p out, natural order, odd frames' signs undone. */
static void fft(const struct esdr_pfbq *p, int16_t *out)
{
	const struct esdr_pfbq_kern *k = p->k;
	unsigned int span = p->radix[0];

	if (p->radix[0] == 5U) {
		k->r5(out, p->work, p->m / 5U, &kc);
	} else {
		k->r4f(out, p->work, p->m / 4U, &kc);
	}
	for (unsigned int s = 1; s < p->stages; span *= p->radix[s], s++) {
		const bool last = s + 1U == p->stages;

		if (p->radix[s] == 4U) {
			if (last) {
				k->r4l(out, span, p->tws[s], &kc);
			} else {
				k->r4(out, p->m, span, p->tws[s], &kc);
			}
		} else if (last) {
			k->r2l(out, span, p->tws[s]);
		} else {
			k->r2(out, p->m, span, p->tws[s]);
		}
	}
	k->flip(out, p->m, &kc);
}

/* The quad nq: its frames' windows are in the history (or the zeros a flush put after it). */
static void quad(struct esdr_pfbq *p, esdr_pfbq_quad_cb cb, void *user)
{
	const uint8_t valid = (uint8_t)(~p->inval & 0xFU);
	const uint8_t complete = (uint8_t)(valid & ~p->incompl & 0xFU);
	const uint64_t m0 = p->nq;

	p->nq += ESDR_PFBQ_FRAMES;
	p->inval >>= ESDR_PFBQ_FRAMES;
	p->incompl >>= ESDR_PFBQ_FRAMES;
	if (valid == 0U) {
		/* All in a gap: nothing to send. */
		return;
	}

	/* Position of pair m0 D: fill less the pairs from it to next (fewer than 4 D past it). */
	const uint32_t pos = (uint32_t)((int64_t)p->fill - (int64_t)(p->next - m0 * p->d));
	int16_t *out = p->ring + (size_t)p->slot * p->m * VEC;
	const struct bk_args a = {
		.x0 = p->hist + 2U * (pos - 3U),
		.work = p->work,
		.taps = p->taps,
		.off = p->off,
		.rnd = p->rnd,
		.dstep = 4 * (int32_t)p->d,
		.tstep = -4 * (int32_t)p->m,
		.groups = p->m / 4U,
		.t = p->t,
		.sh = p->sh,
	};

	{
		PROF_START();
		p->k->branch(&a);
		PROF_END(0);
	}
	{
		PROF_START();
		fft(p, out);
		PROF_END(1);
	}
	p->slot = (uint16_t)(p->slot + 1U == p->quads ? 0U : p->slot + 1U);
	if (cb != NULL) {
		cb(m0, out, valid, complete, user);
	}
}

/* The quads whose last frame's newest pair is in the history. */
static int produce(struct esdr_pfbq *p, esdr_pfbq_quad_cb cb, void *user)
{
	int n = 0;

	while ((p->nq + 3U) * p->d < p->next) {
		quad(p, cb, user);
		n++;
	}
	return n;
}

/*
 * Keeps the window of the next quad: pairs from nq D - L + 1, positions moved by 4 pairs (whole
 * vectors, the last one past the fill too: the room after it is free).
 */
static void slide(struct esdr_pfbq *p)
{
	const uint32_t keep = (uint32_t)(p->next + p->l - 1U - p->nq * p->d);
	const uint32_t src = (p->fill - keep) & ~3U;

	p->k->move(p->hist, p->hist + 2U * src, (p->fill - src + 3U) / 4U);
	p->fill -= src;
}

/*
 * A new run from @p index: its first quad holds the first frame at or after it, the frames
 * before not valid; zeros from that quad's window on, placed so that pair i sits at a position
 * of i + 3 mod 4.
 */
static void start(struct esdr_pfbq *p, uint64_t index)
{
	const uint64_t mf = (index + p->d - 1U) / p->d;
	const uint64_t mfull = (index + p->l - 1U + p->d - 1U) / p->d;
	uint32_t z, e;

	p->nq = mf - mf % ESDR_PFBQ_FRAMES;
	p->inval = (1ULL << (mf - p->nq)) - 1U;
	p->incompl = (1ULL << (mfull - p->nq)) - 1U;
	z = (uint32_t)(index - p->nq * p->d) + p->l - 1U;
	e = ((uint32_t)(index & 3U) + 7U - (z & 3U)) & 3U;
	memset(p->hist, 0, 2U * (e + z) * sizeof(int16_t));
	p->fill = e + z;
	p->next = index;
	p->live = true;
}

/* A gap shorter than L up to @p index: zeros; its frames not valid, those after not complete. */
static int fill_gap(struct esdr_pfbq *p, uint64_t index, esdr_pfbq_quad_cb cb, void *user)
{
	uint32_t g = (uint32_t)(index - p->next);
	int n = 0;

	for (uint64_t m = (p->next + p->d - 1U) / p->d; m * p->d < index + p->l - 1U; m++) {
		uint64_t b = m - p->nq;

		if (b < 64U) {
			p->incompl |= 1ULL << b;
			if (m * p->d < index) {
				p->inval |= 1ULL << b;
			}
		}
	}
	while (g > 0U) {
		uint32_t k;

		if (p->fill == p->cap) {
			slide(p);
		}
		k = p->cap - p->fill < g ? p->cap - p->fill : g;
		memset(p->hist + 2U * p->fill, 0, 2U * k * sizeof(int16_t));
		p->fill += k;
		p->next += k;
		g -= k;
		n += produce(p, cb, user);
	}
	return n;
}

int esdr_pfbq_flush(struct esdr_pfbq *p, esdr_pfbq_quad_cb cb, void *user)
{
	uint32_t ext;

	if (!p->live || p->nq * p->d >= p->next) {
		return 0;
	}
	/* Zeros past the fill, up to the quad's last pair; the input will overwrite them. */
	ext = (uint32_t)((p->nq + 3U) * p->d + 1U - p->next);
	for (unsigned int q = 0; q < ESDR_PFBQ_FRAMES; q++) {
		if ((p->nq + q) * p->d >= p->next) {
			p->inval |= 1ULL << q;
		}
	}
	memset(p->hist + 2U * p->fill, 0, 2U * ext * sizeof(int16_t));
	quad(p, cb, user);
	return 1;
}

/* Twice the raw units: item * 2^(shift + 1) + 2^shift - 1 - 2 dc. */
static void kv_set(int16_t *kv, unsigned int shift, int32_t dc_i, int32_t dc_q)
{
	const int32_t half = (int32_t)(1U << shift) - 1;

	for (unsigned int i = 0; i < VEC; i += 2U) {
		kv[i] = (int16_t)(2U << shift);
		kv[i + 1U] = (int16_t)(2U << shift);
		kv[VEC + i] = sat16(half - 2 * (int64_t)dc_i);
		kv[VEC + i + 1U] = sat16(half - 2 * (int64_t)dc_q);
	}
}

/* @p n pairs to the history at the fill: the kernel for whole aligned vectors, C for the ends. */
static void convert(struct esdr_pfbq *p, const int8_t *src, uint32_t n, const int16_t *kv)
{
	int16_t *dst = p->hist + 2U * p->fill;
	uint32_t head = (uint32_t)((16U - ((uintptr_t)dst & 15U)) & 15U) / 4U, body;

	head = head < n ? head : n;
	conv_c(dst, src, head, kv);
	dst += 2U * head;
	src += 2U * head;
	n -= head;
	body = n & ~7U;
	if (body != 0U) {
		p->k->conv(dst, src, body, kv);
	}
	conv_c(dst + 2U * body, src + 2U * body, n - body, kv);
}

int esdr_pfbq_push(struct esdr_pfbq *p, uint64_t index, const int8_t *iq, size_t pairs,
		   unsigned int shift, int32_t dc_i, int32_t dc_q, esdr_pfbq_quad_cb cb, void *user)
{
	int16_t kv[2U * VEC] __attribute__((aligned(16)));
	int quads = 0;

	if (shift > ESDR_PFBQ_SHIFT_MAX) {
		return -EINVAL;
	}
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
			quads += esdr_pfbq_flush(p, cb, user);
			start(p, index);
		} else {
			quads += fill_gap(p, index, cb, user);
		}
	}
	kv_set(kv, shift, dc_i, dc_q);
	while (pairs > 0U) {
		uint32_t n;

		if (p->fill == p->cap) {
			slide(p);
		}
		n = (uint32_t)(p->cap - p->fill < pairs ? p->cap - p->fill : pairs);
		{
			PROF_START();
			convert(p, iq, n, kv);
			PROF_END(2);
		}
		p->fill += n;
		p->next += n;
		iq += 2U * n;
		pairs -= n;
		quads += produce(p, cb, user);
	}
	return quads;
}

/* ---- burst detector ---- */

struct det_layout {
	size_t slot, sent, use, zero, scratch, floor, learn, pw, sum, loud, quiet, learnt, open, total;
};

static bool det_layout(const struct esdr_detq_cfg *cfg, struct det_layout *lo)
{
	size_t m, b, ns;

	if (cfg == NULL || cfg->channels == 0U || cfg->channels > ESDR_PFB_M_MAX ||
	    cfg->channels % 4U != 0U || cfg->block % 4U != 0U || cfg->block > 1024U ||
	    cfg->open_ratio < 0.0f) {
		return false;
	}
	m = cfg->channels;
	b = cfg->block != 0U ? cfg->block : 16U;
	ns = 2U * b / ESDR_PFBQ_FRAMES;
	lo->slot = 0;
	lo->sent = lo->slot + ALIGN16(ns * sizeof(struct esdr_detq_slot));
	lo->use = lo->sent + ALIGN16(m * sizeof(uint64_t));
	lo->zero = lo->use + ALIGN16(ns * sizeof(const int16_t *));
	/* Masked copies: a block holds B / 4 quads. */
	lo->scratch = lo->zero + m * VEC * sizeof(int16_t);
	lo->floor = lo->scratch + (b / ESDR_PFBQ_FRAMES) * m * VEC * sizeof(int16_t);
	lo->learn = lo->floor + ALIGN16(m * sizeof(float));
	lo->pw = lo->learn + ALIGN16(m * ESDR_DET_LEARN * sizeof(float));
	lo->sum = lo->pw + ALIGN16(m * sizeof(float));
	lo->loud = lo->sum + ALIGN16(m * sizeof(uint32_t));
	lo->quiet = lo->loud + ALIGN16(m * sizeof(uint32_t));
	lo->learnt = lo->quiet + ALIGN16(m * sizeof(uint16_t));
	lo->open = lo->learnt + ALIGN16(m);
	lo->total = lo->open + ALIGN16(m);
	return true;
}

size_t esdr_detq_mem_size(const struct esdr_detq_cfg *cfg)
{
	struct det_layout lo;

	return det_layout(cfg, &lo) ? lo.total + 16U : 0U;
}

int esdr_detq_init(struct esdr_detq *d, const struct esdr_detq_cfg *cfg, void *mem, size_t len)
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
	d->nslot = (uint16_t)(2U * d->b / ESDR_PFBQ_FRAMES);
	d->ratio = cfg->open_ratio != 0.0f ? cfg->open_ratio : 4.0f;
	d->slot = (struct esdr_detq_slot *)(base + lo.slot);
	d->sent = (uint64_t *)(base + lo.sent);
	d->use = (const int16_t **)(base + lo.use);
	d->zero = (int16_t *)(base + lo.zero);
	memset(d->zero, 0, d->m * VEC * sizeof(int16_t));
	d->scratch = (int16_t *)(base + lo.scratch);
	d->floor = (float *)(base + lo.floor);
	d->learn = (float *)(base + lo.learn);
	d->pw = (float *)(base + lo.pw);
	d->sum = (uint32_t *)(base + lo.sum);
	d->loud = (uint32_t *)(base + lo.loud);
	d->quiet = (uint16_t *)(base + lo.quiet);
	d->learnt = base + lo.learnt;
	d->open = base + lo.open;
	esdr_detq_reset(d);
#if defined(ESDR_PFBQ_PIE)
	esdr_pfbq_pie_on();
#endif
	return 0;
}

void esdr_detq_reset(struct esdr_detq *d)
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

static inline uint16_t det_wrap(const struct esdr_detq *d, uint32_t s)
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

/*
 * Mean power per channel over the complete frames of the current block (@p n of them), quads
 * four at a time by the kernel (the zero quad fills a short group); a quad with frames that are
 * not complete goes in as a copy with those zeroed.
 */
static void block_power(struct esdr_detq *d, unsigned int n)
{
	const float scale = (float)(1U << POWER_SHIFT) / (float)n;
	unsigned int nuse = 0, nscratch = 0;

	for (unsigned int i = 0; i < d->cur_n; i++) {
		const struct esdr_detq_slot *s = &d->slot[det_wrap(d, d->cur_first + i)];
		uint32_t keep[ESDR_PFBQ_FRAMES];
		const uint32_t *src;
		uint32_t *dst;

		if (s->complete == 0xFU) {
			d->use[nuse++] = s->iq;
			continue;
		}
		if (s->complete == 0U) {
			continue;
		}
		/* A frame's I and Q are one word of each channel's vector. */
		for (unsigned int q = 0; q < ESDR_PFBQ_FRAMES; q++) {
			keep[q] = (s->complete & (1U << q)) != 0U ? UINT32_MAX : 0U;
		}
		src = (const uint32_t *)(const void *)s->iq;
		dst = (uint32_t *)(void *)(d->scratch + (size_t)nscratch++ * d->m * VEC);
		for (unsigned int k = 0; k < d->m; k++, src += 4, dst += 4) {
			dst[0] = src[0] & keep[0];
			dst[1] = src[1] & keep[1];
			dst[2] = src[2] & keep[2];
			dst[3] = src[3] & keep[3];
		}
		d->use[nuse++] = (const int16_t *)(const void *)(dst - 4U * d->m);
	}
	/* A block of B frames is B / 4 quads: one group of four for the usual B of 16. */
	for (unsigned int i = 0; i < nuse; i += 4U) {
		const int16_t *q[4];

		for (unsigned int j = 0; j < 4U; j++) {
			q[j] = i + j < nuse ? d->use[i + j] : d->zero;
		}
		KERN->power4(q, d->m, d->sum);
		for (unsigned int k = 0; k < d->m; k++) {
			float v = (float)d->sum[k] * scale;

			d->pw[k] = i == 0U ? v : d->pw[k] + v;
		}
	}
}

/*
 * Channel @p k's quads of the current block, with @p pre the block before too, less what it
 * sent already.
 */
static void det_send(struct esdr_detq *d, unsigned int k, uint16_t flags, bool pre, float power,
		     esdr_detq_cb cb, void *user)
{
	struct esdr_detq_ev ev = {
		.chan = (uint16_t)k,
		.flags = flags,
		.first = pre ? d->prev_first : d->cur_first,
		.n = pre ? (uint32_t)d->prev_n + d->cur_n : d->cur_n,
		.power = power,
		.floor = d->floor[k],
	};

	/* A channel that closed and opened again: those quads went out. */
	while (ev.n > 0U && d->slot[ev.first].m0 < d->sent[k]) {
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
static void finish(struct esdr_detq *d, esdr_detq_cb cb, void *user)
{
	const bool adj = d->have_prev && d->prev_start + d->b == d->cur_start;
	unsigned int ncomp = 0;
	bool valid;

	for (unsigned int i = 0; i < d->cur_n; i++) {
		uint8_t c = d->slot[det_wrap(d, d->cur_first + i)].complete;

		ncomp += (c & 1U) + ((c >> 1) & 1U) + ((c >> 2) & 1U) + ((c >> 3) & 1U);
	}
	/* Blocks of mostly transients (a run's first frames) decide nothing. */
	valid = 2U * ncomp >= d->b;
	if (valid) {
		PROF_START();
		block_power(d, ncomp);
		PROF_END(3);
		d->decided++;
		d->decided_start = d->cur_start;
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
				/*
				 * Far below: the floor came from bursts (a pruned stream may hold
				 * nothing quieter at first), this block is the channel's own noise.
				 */
				if (pw * DET_SNAP < d->floor[k]) {
					d->floor[k] = pw;
				} else {
					d->floor[k] += (pw - d->floor[k]) * (1.0f / DET_FOLLOW);
				}
				continue;
			}
			d->open[k] = 1;
			d->quiet[k] = 0;
			d->loud[k] = 1;
			flags = ESDR_DET_OPEN;
		} else if (!valid) {
			/* Nothing to decide: the quads go out. */
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

void esdr_detq_quad(struct esdr_detq *d, uint64_t m0, const int16_t *iq, uint8_t valid,
		    uint8_t complete, esdr_detq_cb cb, void *user)
{
	if (d->have_last && m0 <= d->last_m) {
		/* The stream went back (a new stream): no block or quad of before applies. */
		if (d->have_cur) {
			finish(d, cb, user);
		}
		d->have_prev = false;
		for (unsigned int k = 0; k < d->m; k++) {
			d->sent[k] = 0;
		}
	} else if (d->have_cur && m0 >= d->cur_start + d->b) {
		finish(d, cb, user);
	}
	if (!d->have_cur) {
		uint64_t pend = d->prev_start + d->b;

		d->cur_start = d->have_prev && m0 >= pend && m0 < pend + d->b ? pend : m0 - m0 % d->b;
		d->cur_first = d->wr;
		d->cur_n = 0;
		d->have_cur = true;
	}
	d->slot[d->wr] = (struct esdr_detq_slot){
		.iq = iq, .m0 = m0, .valid = valid, .complete = complete,
	};
	d->wr = det_wrap(d, d->wr + 1U);
	d->cur_n++;
	d->last_m = m0;
	d->have_last = true;
}

void esdr_detq_flush(struct esdr_detq *d, esdr_detq_cb cb, void *user)
{
	if (d->have_cur) {
		finish(d, cb, user);
	}
}

uint32_t esdr_detq_decided(const struct esdr_detq *d, uint64_t *start)
{
	if (start != NULL) {
		*start = d->decided_start;
	}
	return d->decided;
}

const float *esdr_detq_power(const struct esdr_detq *d)
{
	return d->pw;
}

const float *esdr_detq_floor(const struct esdr_detq *d)
{
	return d->floor;
}

const uint8_t *esdr_detq_open(const struct esdr_detq *d)
{
	return d->open;
}

unsigned int esdr_detq_take(const struct esdr_detq *d, const struct esdr_detq_ev *ev,
			    uint32_t *pos, int16_t *iq, unsigned int max, uint64_t *m0,
			    uint32_t *flags)
{
	const size_t c = (size_t)ev->chan * VEC;
	const uint32_t end = ev->n * ESDR_PFBQ_FRAMES;
	unsigned int n = 0;
	uint64_t want = 0;
	bool done = false;

	while (*pos < end && n < max) {
		const struct esdr_detq_slot *s = &d->slot[det_wrap(d, ev->first + *pos / 4U)];
		const unsigned int q = *pos % 4U;
		uint64_t m = s->m0 + q;
		bool comp;

		/* A whole quad of one completeness, all valid: its 4 samples at once. */
		if (q == 0U && s->valid == 0xFU && (s->complete == 0U || s->complete == 0xFU) &&
		    max - n >= 4U) {
			comp = s->complete != 0U;
			if (n == 0U) {
				*m0 = m;
				*flags = comp ? ESDR_PFB_COMPLETE : 0U;
				done = comp;
			} else if (m != want || comp != done) {
				break;
			}
			/* Words: the build's memcpy() is slow (the P4's ROM). */
			const uint32_t *w = (const uint32_t *)(const void *)&s->iq[c];
			uint32_t *o = (uint32_t *)(void *)&iq[2U * n];

			o[0] = w[0];
			o[1] = w[1];
			o[2] = w[2];
			o[3] = w[3];
			n += 4U;
			want = m + 4U;
			*pos += 4U;
			continue;
		}
		comp = (s->complete & (1U << q)) != 0U;
		if ((s->valid & (1U << q)) == 0U) {
			if (n > 0U) {
				break;
			}
			(*pos)++;
			continue;
		}
		if (n == 0U) {
			*m0 = m;
			*flags = comp ? ESDR_PFB_COMPLETE : 0U;
			done = comp;
		} else if (m != want || comp != done) {
			break;
		}
		iq[2U * n] = s->iq[c + 2U * q];
		iq[2U * n + 1U] = s->iq[c + 2U * q + 1U];
		n++;
		want = m + 1U;
		(*pos)++;
	}
	return n;
}
