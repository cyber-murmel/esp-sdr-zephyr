/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Scan app's measurement (esdr_scan.h). A frame's FFT in fixed point, radix 2, every stage
 * halved, in one of two kinds:
 *
 * - int32 values with Q15 twiddles (the P4, the S31): the samples go in doubled, so the output is
 *   2 DFT / N, a frame's complex values stay within 32768 sqrt(2) and no product passes 2^31. The
 *   rounding's noise is some 1 unit squared a bin: a floor of 30 int16 rms in I and in Q (about one
 *   raw LSB, the S3's at gain 0) reads half a dB high; the radios' floors at gain 48 (300 to 1200
 *   rms) are hundreds of times the rounding.
 * - int16 on the S3's vector unit (ESDR_SCAN_FFT16: esp-dsp's dsps_fft2r_sc16_aes3_(), through
 *   esdr_scan_fft16() in esdr_scan_pie.c; the host tests build the same arithmetic in C, which
 *   the XIAO's kernel matches bit for bit). Each frame goes in at a gain of 2^e, the largest that
 *   keeps its values within FFT16_LIM (block floating point), and its power comes out divided by
 *   4^e, so the rounding scales with the frame's strongest component, not with full scale: a
 *   floor of 10 int16 rms reads within 0.05 dB. The kernel truncates; the bias that adds up to is
 *   taken off (esdr_scan_init()), what is left is noise-like, 74 dB under the strongest
 *   component, and a clean strong tone's spurs some 62 dB under it, the worst within the DC
 *   guard the panorama does not read.
 *
 * tests/host/test_scan_core.py holds both to numpy. On the XIAO S3 (2026-11-10) the library's
 * float FFT (esdr_gui.c's psd()) took 0.6 ms a 256-point frame, the int32 kind 265 us a frame in
 * all, the 16-bit one 59 us with its range and window on the vector unit too (100 in C: the
 * range 28, the window 23 to 30, the FFT 15 to 20, the power 32 to 40), plus 0.12 ms a hop for
 * the dB (0.7 ms with libm's log10f()).
 */

#include <math.h>
#include <stddef.h>

#include "esdr_scan.h"

#pragma GCC optimize("O3")

#define N    ESDR_SCAN_N
#define LOGN 8U

#if defined(CONFIG_SOC_SERIES_ESP32S3)
#define ESDR_SCAN_FFT16 1
#endif

/* Single precision only: the multiapp host exports libm's float functions (psd() uses them). */
static int16_t q15(float v, float full)
{
	float r = v * full;
	int32_t i = (int32_t)(r < 0.0f ? r - 0.5f : r + 0.5f);

	return (int16_t)(i > 32767 ? 32767 : (i < -32768 ? -32768 : i));
}

void esdr_scan_init(struct esdr_scan *s)
{
	/* Blackman-Harris, four terms, periodic: sidelobes 92 dB down, a main lobe 4 bins each side. */
	static const float a[4] = {0.35875f, 0.48829f, 0.14128f, 0.01168f};
	const float tau = 6.2831853f;
	float sum = 0.0f;

	for (uint32_t k = 0; k < N; k++) {
		float x = tau * (float)k / (float)N;
		float w = a[0] - a[1] * cosf(x) + a[2] * cosf(2.0f * x) - a[3] * cosf(3.0f * x);

		s->win[k] = q15(w, 32768.0f);
		s->win2[2U * k] = s->win[k];
		s->win2[2U * k + 1U] = s->win[k];
		sum += (float)s->win[k] / 32768.0f;
	}
	for (uint32_t k = 0; k < N; k++) {
		uint32_t r = 0;

		for (uint32_t b = 0; b < LOGN; b++) {
			r |= ((k >> b) & 1U) << (LOGN - 1U - b);
		}
		s->rev[k] = (uint8_t)r;
	}
#if defined(ESDR_SCAN_FFT16)
	/* esp-dsp's table: cos, sin of tau r / N, r the bit reversal of k (7 bits), times 32767. */
	for (uint32_t k = 0; k < N / 2U; k++) {
		const float x = tau * (float)(s->rev[k] >> 1) / (float)N;

		s->tw[2U * k] = q15(cosf(x), 32767.0f);
		s->tw[2U * k + 1U] = q15(sinf(x), 32767.0f);
	}
	/*
	 * The kernel truncates: each of its shifts takes 0.5 off on average, a butterfly's sum 1
	 * in I and in Q (two shifts), its difference nothing, and the stages carry that on into a
	 * fixed pattern (some 8 in I and Q at DC, 3 to 6 next to it: -55 dB of a full-scale tone
	 * at the gain 1); the window's shift (esdr_scan_window()) truncates too, 0.5 off every
	 * input. Worked out as its butterflies carry it (acc and peak as scratch) and taken off
	 * each frame, rounded: what is left is some 1.2 rms a bin, 74 dB under a full-scale tone
	 * (tests/host/test_scan_core.py).
	 */
	{
		float *br = s->acc, *bi = s->peak;
		const float k = 32767.0f / 32768.0f / 2.0f;

		for (uint32_t i = 0; i < N; i++) {
			br[i] = -0.5f;
			bi[i] = -0.5f;
		}
		for (uint32_t half = N / 2U, groups = 1U; half > 0U; half >>= 1, groups <<= 1) {
			uint32_t ia = 0;

			for (uint32_t j = 0; j < groups; j++) {
				const float c = (float)s->tw[2U * j] / 32767.0f;
				const float sn = (float)s->tw[2U * j + 1U] / 32767.0f;

				for (uint32_t i = 0; i < half; i++, ia++) {
					const uint32_t m = ia + half;
					const float ar = br[ia] * k, ai = bi[ia] * k;
					const float tr = (br[m] * c + bi[m] * sn) * k;
					const float ti = (bi[m] * c - br[m] * sn) * k;

					br[ia] = ar + tr - 1.0f;
					bi[ia] = ai + ti - 1.0f;
					br[m] = ar - tr;
					bi[m] = ai - ti;
				}
				ia += half;
			}
		}
		for (uint32_t i = 0; i < N; i++) {
			s->bias[2U * i] = (int8_t)(br[i] < 0.0f ? br[i] - 0.5f : br[i] + 0.5f);
			s->bias[2U * i + 1U] = (int8_t)(bi[i] < 0.0f ? bi[i] - 0.5f : bi[i] + 0.5f);
		}
	}
	/*
	 * A full-scale tone at a bin's centre: 16384 times the window's mean (DFT / N), times
	 * 32767 / 32768 a stage (the kernel halves by that factor).
	 */
	s->ref_db = 20.0f * log10f(16384.0f * sum / (float)N) +
		    20.0f * (float)LOGN * log10f(32767.0f / 32768.0f);
#else
	for (uint32_t k = 0; k < N / 2U; k++) {
		s->tw[2U * k] = q15(cosf(-tau * (float)k / (float)N), 32768.0f);
		s->tw[2U * k + 1U] = q15(sinf(-tau * (float)k / (float)N), 32768.0f);
	}
	/* A full-scale tone at a bin's centre: 2 times 16384 times the window's mean (2 DFT / N). */
	s->ref_db = 20.0f * log10f(2.0f * 16384.0f * sum / (float)N);
#endif
}

/*
 * The frame's centre (its mean for the int32 FFT, the middle of its range for the 16-bit one), the
 * samples clipped, and (16-bit) how far any lies from the centre.
 */
struct frame_stats {
	int32_t mi, mq;
	int32_t dev;
	uint32_t clip;
};

static int32_t iabs(int32_t v)
{
	return v < 0 ? -v : v;
}

#if defined(ESDR_SCAN_FFT16)
static uint32_t clip_count(const int16_t *x)
{
	uint32_t clip = 0;

	for (uint32_t k = 0; k < N; k++) {
		const int32_t ai = iabs(x[2U * k]), aq = iabs(x[2U * k + 1U]);

		clip += (ai > aq ? ai : aq) >= ESDR_SCAN_CLIP ? 1U : 0U;
	}
	return clip;
}
#else
static void frame_stats(const int16_t *x, struct frame_stats *st)
{
	int32_t si = 0, sq = 0;
	uint32_t clip = 0;

	for (uint32_t k = 0; k < N; k++) {
		const int32_t i = x[2U * k], q = x[2U * k + 1U];
		const int32_t ai = iabs(i), aq = iabs(q);

		si += i;
		sq += q;
		clip += (ai > aq ? ai : aq) >= ESDR_SCAN_CLIP ? 1U : 0U;
	}
	st->mi = (si + (int32_t)(N / 2U)) >> LOGN;
	st->mq = (sq + (int32_t)(N / 2U)) >> LOGN;
	st->clip = clip;
	st->dev = 0;
}
#endif

#if defined(ESDR_SCAN_FFT16)

/*
 * The windowed frame's I and Q at most this: complex values within FFT16_LIM sqrt(2) (32527),
 * which no stage makes larger (each halves a sum of two), so none passes int16 (the kernel's
 * adds saturate besides).
 */
#define FFT16_LIM 23000

static int16_t sat16(int32_t v)
{
	return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}

void esdr_scan_minmax_c(const int16_t *x, int16_t *mm)
{
	for (uint32_t l = 0; l < 8U; l++) {
		mm[l] = x[l];
		mm[8U + l] = x[l];
	}
	for (uint32_t k = 8U; k < 2U * N; k += 8U) {
		for (uint32_t l = 0; l < 8U; l++) {
			mm[l] = x[k + l] > mm[l] ? x[k + l] : mm[l];
			mm[8U + l] = x[k + l] < mm[8U + l] ? x[k + l] : mm[8U + l];
		}
	}
}

void esdr_scan_window_c(const int16_t *x, int16_t *d, const int16_t *w2, uint32_t c, uint32_t sar)
{
	const int32_t ci = (int16_t)(c & 0xffffU), cq = (int16_t)(c >> 16);

	for (uint32_t k = 0; k < 2U * N; k += 2U) {
		d[k] = (int16_t)((sat16(x[k] - ci) * w2[k]) >> sar);
		d[k + 1U] = (int16_t)((sat16(x[k + 1U] - cq) * w2[k + 1U]) >> sar);
	}
}

#if defined(ESDR_SCAN_FFT16_MODEL)
void esdr_scan_minmax(const int16_t *x, int16_t *mm)
{
	esdr_scan_minmax_c(x, mm);
}

void esdr_scan_window(const int16_t *x, int16_t *d, const int16_t *w2, uint32_t c, uint32_t sar)
{
	esdr_scan_window_c(x, d, w2, c, sar);
}

/*
 * esp-dsp's dsps_fft2r_sc16_aes3_() in C, step for step (the host tests): radix 2 in place,
 * natural order in, bit reversed out; per butterfly a times 32767 and b times the conjugate
 * twiddle, each shifted down 16 (halved) on its own, then added and subtracted saturating.
 */
void esdr_scan_fft16(int16_t *d, const int16_t *tw)
{
	for (uint32_t half = N / 2U, groups = 1U; half > 0U; half >>= 1, groups <<= 1) {
		uint32_t ia = 0;

		for (uint32_t j = 0; j < groups; j++) {
			const int32_t c = tw[2U * j], sn = tw[2U * j + 1U];

			for (uint32_t i = 0; i < half; i++, ia++) {
				const uint32_t m = ia + half;
				const int32_t ar = (d[2U * ia] * 32767) >> 16;
				const int32_t ai = (d[2U * ia + 1U] * 32767) >> 16;
				const int32_t br = d[2U * m], bi = d[2U * m + 1U];
				const int32_t tr = (br * c + bi * sn) >> 16;
				const int32_t ti = (bi * c - br * sn) >> 16;

				d[2U * m] = sat16(ar - tr);
				d[2U * m + 1U] = sat16(ai - ti);
				d[2U * ia] = sat16(ar + tr);
				d[2U * ia + 1U] = sat16(ai + ti);
			}
			ia += half;
		}
	}
}
#endif

/* 4^-e, e from -2 to 15. */
static float pow4_neg(int e)
{
	return e >= 0 ? 1.0f / (float)(1UL << (2 * e)) : (float)(1UL << (-2 * e));
}

/*
 * Each lane's largest and smallest (esdr_scan_minmax()): the centre in the middle of the range
 * (any constant reaches only the 7 bins around DC through this window, which the panorama takes
 * from the next hop, and the middle leaves the most gain), the clipped samples counted only where
 * the range reaches ESDR_SCAN_CLIP.
 */
static void frame_stats16(const int16_t *x, struct frame_stats *st)
{
	int16_t mm[16] __attribute__((aligned(16)));
	int32_t imax, imin, qmax, qmin;

	esdr_scan_minmax(x, mm);
	imax = mm[0];
	qmax = mm[1];
	imin = mm[8];
	qmin = mm[9];
	for (uint32_t l = 2U; l < 8U; l += 2U) {
		imax = mm[l] > imax ? mm[l] : imax;
		qmax = mm[l + 1U] > qmax ? mm[l + 1U] : qmax;
		imin = mm[8U + l] < imin ? mm[8U + l] : imin;
		qmin = mm[9U + l] < qmin ? mm[9U + l] : qmin;
	}
	st->mi = (imax + imin) >> 1;
	st->mq = (qmax + qmin) >> 1;
	st->dev = imax - st->mi;
	st->dev = st->mi - imin > st->dev ? st->mi - imin : st->dev;
	st->dev = qmax - st->mq > st->dev ? qmax - st->mq : st->dev;
	st->dev = st->mq - qmin > st->dev ? st->mq - qmin : st->dev;
	st->clip = (imax >= ESDR_SCAN_CLIP || imin <= -ESDR_SCAN_CLIP || qmax >= ESDR_SCAN_CLIP ||
		    qmin <= -ESDR_SCAN_CLIP) ? clip_count(x) : 0U;
}

/* One frame into acc and peak (DFT / N of its samples, power). */
static void frame16(struct esdr_scan *s, const int16_t *x, const struct frame_stats *st)
{
	int16_t d[2U * N] __attribute__((aligned(16)));
	const uint32_t dev = (uint32_t)st->dev;
	int e = 15;

	/*
	 * The gain: 2^e, the most that keeps the frame within FFT16_LIM: 1/2 at least, since no
	 * int16 lies more than 32768 from the middle of its frame's range (the S3's 10-bit samples
	 * times 32 never take less than 1).
	 */
	while (e > 0 && (dev << e) > FFT16_LIM) {
		e--;
	}
	while (e > -2 && e <= 0 && (dev >> -e) > FFT16_LIM) {
		e--;
	}
	if (e >= 0) {
		/* (x - c) times the window, shifted down 15 - e: within 16 bits all the way. */
		const uint32_t c = ((uint32_t)st->mi & 0xffffU) | ((uint32_t)st->mq << 16);

		esdr_scan_window(x, d, s->win2, c, 15U - (uint32_t)e);
	} else {
		const uint32_t sh = 15U + (uint32_t)-e;
		const int32_t r = (int32_t)1 << (sh - 1U);

		for (uint32_t k = 0; k < N; k++) {
			const int32_t w = s->win[k];

			d[2U * k] = (int16_t)(((x[2U * k] - st->mi) * w + r) >> sh);
			d[2U * k + 1U] = (int16_t)(((x[2U * k + 1U] - st->mq) * w + r) >> sh);
		}
	}
	esdr_scan_fft16(d, s->tw);

	const float g2 = pow4_neg(e);

	/*
	 * Out in bit reversed order (bin rev[i] at i), and so into acc and peak, the bias off. Four
	 * bins a step: the FPU's latencies overlap.
	 */
	for (uint32_t i = 0; i < N; i += 4U) {
		float p[4];

		for (uint32_t j = 0; j < 4U; j++) {
			const int32_t re = d[2U * (i + j)] - s->bias[2U * (i + j)];
			const int32_t im = d[2U * (i + j) + 1U] - s->bias[2U * (i + j) + 1U];

			p[j] = (float)((uint32_t)(re * re) + (uint32_t)(im * im)) * g2;
		}
		for (uint32_t j = 0; j < 4U; j++) {
			s->acc[i + j] += p[j];
			s->peak[i + j] = p[j] > s->peak[i + j] ? p[j] : s->peak[i + j];
		}
	}
}

/* Where frame16() put bin k. */
#define BIN_AT(s, k) ((s)->rev[k])

/* An empty bin in dB: 1e-3 of the int32 kind's unit squared, in this kind's (a quarter). */
#define POWER_FLOOR 2.5e-4f

#else /* !ESDR_SCAN_FFT16 */

/* a, b to (a + b) / 2, (a - b) / 2, rounded. */
#define HALVE(ar, ai, br, bi)                                                                     \
	do {                                                                                      \
		const int32_t t0 = (ar), t1 = (ai), t2 = (br), t3 = (bi);                        \
		(ar) = (t0 + t2 + 1) >> 1;                                                        \
		(ai) = (t1 + t3 + 1) >> 1;                                                        \
		(br) = (t0 - t2 + 1) >> 1;                                                        \
		(bi) = (t1 - t3 + 1) >> 1;                                                        \
	} while (0)

static void fft(int32_t *re, int32_t *im, const int16_t *tw)
{
	/* The first two stages take no multiplication: twiddles 1, then 1 and -j. */
	for (uint32_t i = 0; i < N; i += 4U) {
		int32_t tr, ti;

		HALVE(re[i], im[i], re[i + 1U], im[i + 1U]);
		HALVE(re[i + 2U], im[i + 2U], re[i + 3U], im[i + 3U]);
		HALVE(re[i], im[i], re[i + 2U], im[i + 2U]);
		/* (i + 3) times -j: (im, -re). */
		tr = im[i + 3U];
		ti = -re[i + 3U];
		re[i + 3U] = tr;
		im[i + 3U] = ti;
		HALVE(re[i + 1U], im[i + 1U], re[i + 3U], im[i + 3U]);
	}
	for (uint32_t half = 4U; half < N; half <<= 1) {
		const uint32_t step = N / (2U * half);

		/* Twiddle 1 at k = 0. */
		for (uint32_t i = 0; i < N; i += 2U * half) {
			HALVE(re[i], im[i], re[i + half], im[i + half]);
		}
		for (uint32_t k = 1; k < half; k++) {
			const int32_t wr = tw[2U * k * step], wi = tw[2U * k * step + 1U];

			for (uint32_t i = k; i < N; i += 2U * half) {
				const uint32_t j = i + half;
				const int32_t tr = (re[j] * wr - im[j] * wi + 16384) >> 15;
				const int32_t ti = (re[j] * wi + im[j] * wr + 16384) >> 15;
				const int32_t ar = re[i], ai = im[i];

				re[i] = (ar + tr + 1) >> 1;
				im[i] = (ai + ti + 1) >> 1;
				re[j] = (ar - tr + 1) >> 1;
				im[j] = (ai - ti + 1) >> 1;
			}
		}
	}
}

static int32_t clamp14(int32_t v)
{
	return v > 16384 ? 16384 : (v < -16384 ? -16384 : v);
}

/* One frame into acc and peak (2 DFT / N of its samples, power). */
static void frame32(struct esdr_scan *s, const int16_t *x, const struct frame_stats *st)
{
	/*
	 * The frame's values on the stack: the interpreter's in internal RAM, the Scan worker's in
	 * PSRAM (a 2 KB working set the cache holds: 5.6 ms a hop on the S31 either way).
	 */
	int32_t re[N], im[N];

	for (uint32_t k = 0; k < N; k++) {
		const uint32_t r = s->rev[k];
		const int32_t w = s->win[k];

		/* Doubled: one bit more over the rounding (2^15 * 2^15 still fits). */
		re[r] = (clamp14(x[2U * k] - st->mi) * w + 8192) >> 14;
		im[r] = (clamp14(x[2U * k + 1U] - st->mq) * w + 8192) >> 14;
	}
	fft(re, im, s->tw);
	for (uint32_t k = 0; k < N; k++) {
		const float p = (float)((uint32_t)(re[k] * re[k]) + (uint32_t)(im[k] * im[k]));

		s->acc[k] += p;
		s->peak[k] = p > s->peak[k] ? p : s->peak[k];
	}
}

#define BIN_AT(s, k) (k)
#define POWER_FLOOR 1e-3f

#endif /* ESDR_SCAN_FFT16 */

/*
 * 10 log10(y) for a power y > 0 (normal): log2 from the exponent and a polynomial of the mantissa,
 * within 0.0002 dB. libm's log10f() took some 1.4 us a call on the S3, 512 of them a hop.
 */
static float db10(float y)
{
	union {
		float f;
		uint32_t u;
	} v = {.f = y};
	const float e = (float)((int32_t)((v.u >> 23) & 0xffU) - 127);
	float t, h;

	v.u = (v.u & 0x007fffffU) | 0x3f800000U;
	t = v.f - 1.0f;
	h = 0.0599455865f;
	h = h * t - 0.227712643f;
	h = h * t + 0.442274178f;
	h = h * t - 0.717063932f;
	h = h * t + 1.44261568f;
	return 3.01029996f * (e + t * h);
}

uint32_t esdr_scan_spectrum(struct esdr_scan *s, const int16_t *iq, uint32_t n,
			    uint32_t frames_max, float *mean_db, float *peak_db, uint32_t *clipped)
{
	uint32_t frames = n / N, clip = 0;

	if (frames_max != 0U && frames > frames_max) {
		frames = frames_max;
	}
	if (clipped != NULL) {
		*clipped = 0;
	}
	if (frames == 0U) {
		return 0;
	}
	for (uint32_t k = 0; k < N; k++) {
		s->acc[k] = 0.0f;
		s->peak[k] = 0.0f;
	}
	for (uint32_t f = 0; f < frames; f++) {
		const int16_t *x = iq + 2U * N * f;
		struct frame_stats st;

#if defined(ESDR_SCAN_FFT16)
		frame_stats16(x, &st);
		frame16(s, x, &st);
#else
		frame_stats(x, &st);
		frame32(s, x, &st);
#endif
		clip += st.clip;
	}
	const float inv = 1.0f / (float)frames;

	for (uint32_t k = 0; k < N; k++) {
		/* Bin k (natural order, DC first) to out[k + N / 2]: the lowest frequency first. */
		const uint32_t o = (k + N / 2U) & (N - 1U);
		const uint32_t j = BIN_AT(s, k);

		mean_db[o] = db10(s->acc[j] * inv + POWER_FLOOR) - s->ref_db;
		if (peak_db != NULL) {
			peak_db[o] = db10(s->peak[j] + POWER_FLOOR) - s->ref_db;
		}
	}
	if (clipped != NULL) {
		*clipped = clip;
	}
	return frames;
}

int esdr_scan_fold(const float *mean_db, const float *peak_db, const float *corr_db,
		   const uint16_t *segs, uint32_t nsegs, float *recent, float *maxh, float *avg,
		   float *floor_db, uint32_t len, float alpha, float rise, float *peak_max,
		   float *floor_mean)
{
	/* 10^(x/10) and 10 log10(y) through e. */
	const float to_ln = 0.230258509f, to_db = 4.342944819f;
	const int hold = alpha > 0.0f;
	float best = ESDR_SCAN_EMPTY, sum = 0.0f;
	uint32_t count = 0;

	for (uint32_t g = 0; g < nsegs; g++) {
		const uint32_t s0 = segs[3U * g], d0 = segs[3U * g + 1U], m = segs[3U * g + 2U];

		if (s0 + m > N || d0 + m > len) {
			return -1;
		}
	}
	for (uint32_t g = 0; g < nsegs; g++) {
		const uint32_t s0 = segs[3U * g], d0 = segs[3U * g + 1U], m = segs[3U * g + 2U];

		for (uint32_t i = 0; i < m; i++) {
			const float c = corr_db != NULL ? corr_db[s0 + i] : 0.0f;
			const float v = mean_db[s0 + i] + c;
			const float p = (peak_db != NULL ? peak_db[s0 + i] : mean_db[s0 + i]) + c;
			const uint32_t d = d0 + i;

			if (recent[d] <= ESDR_SCAN_EMPTY || v > recent[d]) {
				recent[d] = v;
			}
			sum += v;
			count++;
			if (p > best) {
				best = p;
			}
			if (!hold) {
				continue;
			}
			if (maxh[d] <= ESDR_SCAN_EMPTY || p > maxh[d]) {
				maxh[d] = p;
			}
			if (avg[d] <= ESDR_SCAN_EMPTY || !(alpha < 1.0f)) {
				avg[d] = v;
			} else {
				avg[d] = to_db * logf(alpha * expf(v * to_ln) +
						      (1.0f - alpha) * expf(avg[d] * to_ln));
			}
			if (floor_db[d] <= ESDR_SCAN_EMPTY || v < floor_db[d] + rise) {
				floor_db[d] = v;
			} else {
				floor_db[d] += rise;
			}
		}
	}
	if (peak_max != NULL) {
		*peak_max = best;
	}
	if (floor_mean != NULL) {
		*floor_mean = count != 0U ? sum / (float)count : ESDR_SCAN_EMPTY;
	}
	return 0;
}

void esdr_scan_q8(const float *db, uint8_t *q, uint32_t n)
{
	for (uint32_t k = 0; k < n; k++) {
		const float v = db[k];

		if (v <= ESDR_SCAN_EMPTY || v < -127.25f) {
			q[k] = 0;
		} else {
			const float r = 255.0f + 2.0f * v;
			const int32_t i = (int32_t)(r + 0.5f);

			q[k] = (uint8_t)(i > 255 ? 255 : (i < 1 ? 1 : i));
		}
	}
}
