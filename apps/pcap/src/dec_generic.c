/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The generic analysis of a burst no decoder claimed (decode.h, doc/pcap.md "Decoders"): its
 * centre, bandwidth, modulation class, symbol rate and bits, best effort. Plain C, no Zephyr:
 * tests/unit/pcap_generic runs it on native_sim.
 *
 * Measure and refine. The loud part, trimmed to where the envelope stands clear of the floor, is
 * measured from its averaged power spectrum (256 points, Hann, up to SPEC_SEGS segments over it):
 * the noise per bin is the corrected 25th percentile of the bins, the signal what lies above it,
 * its centre and width the band of its middle 90 %. While refine_decim() allows (the signal fills
 * less than a quarter of the rate), the stream is mixed to that centre and decimated (refine.c),
 * then measured again (or, when no further step can follow and the bins coarsen little, the
 * measurement carried over: the spectra are a short burst's main cost): the analysis runs at a
 * rate fitting the signal, without the noise beside it. The transforms are this file's (radix 4
 * where the size allows, the tables built by symmetry), cheaper than esp_sdr_fft()'s on the P4.
 *
 * Classify, from the envelope and the phase steps (dec_step(), about the measured centre):
 * - OOK: the smoothed power splits into two levels (Otsu), the lower one near the noise, with a
 *   gap inside the burst;
 * - FSK: the frequency's histogram has two to four peaks about the centre, or no carrier line
 *   comes and the frequency spreads; the transitions (its sign changes) give the symbol grid, and
 *   on it the histogram of the symbols' mean frequencies shows two or four tones with the eye
 *   open between them; MSK when two tones lie a quarter of the symbol rate either side (measured
 *   over the symbols' middles) and the frequency steps sharply at the boundaries (BLE's GFSK and
 *   GMSK, smooth, stay 2FSK);
 * - CW: one frequency, a steady envelope, the spectrum's power within two bins of its peak;
 * - BPSK and QPSK: one frequency, a line in the spectrum of the squared (fourth power) samples
 *   at twice (four times) the centre, the phase steps on a grid;
 * - WIDEBAND: a strong signal filling the band, its power spread as Gaussian noise's;
 * - UNKNOWN otherwise.
 * The symbol rate comes from the times of the transitions (frequency sign changes, envelope edges,
 * phase steps): the highest cluster of their intervals (the smallest, for clean OOK edges), the
 * period error from their phase drift across the burst, a least-squares grid through them, checked
 * against twice and half its period. The bits are sliced on that grid: FSK and MSK the mean
 * frequency over the middle SLICE_SPAN of a symbol against the centre (4FSK Gray coded: sign,
 * then inner), OOK the mean power against the levels' midpoint, BPSK the differential phase
 * (DBPSK), QPSK the differential quadrant (DQPSK, Gray coded). Estimates that disagree (too few
 * transitions or symbols, transitions off the grid, an eye that is not open) give UNKNOWN and no
 * bits (OOK keeps its class without a rate).
 */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <esp_sdr/esp_sdr_dsp.h>

#include "dec_util.h"
#include "decode.h"
#include "refine.h"

/* Spectrum: points, the shortest transform, segments averaged at most. */
#define SPEC_N      256U
#define SPEC_MIN    32U
#define SPEC_SEGS   4U
/* A refined stream keeps at least this many samples. */
#define REFINE_KEEP 256U
/* In-band signal to noise ratio (power) below which nothing is refined or classified. */
#define SNR_MIN     2.0f
/* Window of the envelope that trims the loud part, samples. */
#define TRIM_WIN    16U
/* Transitions kept for the symbol rate; symbols kept for the tones and the bits (a record's
 * bits are fewer). */
#define TR_MAX      2048U
#define SYM_MAX     4096U
/* Lags (and the envelope's smoothing) at most, samples. */
#define LAG_MAX     64U
/* Histogram of the frequency: bins, binary angle units a bin is 2^FH_SHIFT. */
#define FH_N        64U
#define FH_SHIFT    10U
/* Histogram of the smoothed power: bins. */
#define EH_N        64U
/* Interval histogram: bins over four times the median interval. */
#define LH_N        128U
/* Transitions a grid needs at least. */
#define GRID_MIN    8U
/* Transitions a block of the grid's phase holds: coarse, fine. */
#define GRID_BLK1   4U
#define GRID_BLK2   16U
/* Peaks of the frequency histogram: at most, the share of the highest one a peak needs, the
 * valley (share of the lower peak) below which two peaks stay apart. */
#define PK_MAX      6U
#define PK_REL      0.15f
#define PK_VALLEY   0.75f
/* A peak lighter than this share of the average peak's weight is dropped. */
#define PK_LIGHT    0.4f
/* A symbol's slicing window: this share of the symbol about its centre. */
#define SLICE_SPAN  0.7f

struct gws {
	float re[SPEC_N], im[SPEC_N], acc[SPEC_N], tmp[SPEC_N];
	float twc[SPEC_N / 2U], tws[SPEC_N / 2U];
	float win[SPEC_N];
	/* Bit and base-4 digit reversal of the indices of SPEC_N points. */
	uint8_t rev[SPEC_N], rev4[SPEC_N];
	float wsum2;
	uint32_t win_n;
	uint32_t ntr;
	float tr[TR_MAX];
	/* The grid's blocks: mean time, phase, coherence. */
	float bt[TR_MAX / GRID_BLK1], bp[TR_MAX / GRID_BLK1], bc[TR_MAX / GRID_BLK1];
	/* The symbols' mean frequencies on the grid (sym_all()). */
	float sym[SYM_MAX];
};

struct meas {
	/* Hz against the stream's centre: the 90 % band's middle and width, the strongest bin. */
	float centre, bw, peak;
	/* Power a sample: the signal above the noise, the noise over the stream's band. */
	float sig, noise;
	/* Share of the signal's power within two bins of the strongest one. */
	float conc;
	/* Hz a bin. */
	float bin;
};

struct env {
	/* Mean power a sample, the squared coefficient of variation of the power, the largest
	 * power smoothed over the envelope's window (steps() fills these). */
	float mean, cv2, pmax;
	/* The smoothed power's two levels (Otsu), their separation (between-class variance over
	 * the total), the lower level's share of the samples. */
	float lo, hi, sep, wlo;
};

struct peak {
	/* Position (bins, 0 .. FH_N), weight. */
	float pos, wt;
};

struct grid {
	/* Samples a symbol, the first boundary (boundaries at tau + k t), share of transitions on
	 * the grid. */
	float t, tau, q;
};

struct ctx {
	struct gws *ws;
	const int16_t *iq;
	int16_t *step;
	/* The symbols' mean frequencies on the grid (sym_all()), room for sym_max. */
	float *sym;
	uint32_t sym_max;
	uint32_t n;
	float rate;
	struct meas m;
	/* Results: class, grid, deviation and centre (Hz against the stream's centre). */
	uint8_t mod;
	struct grid g;
	bool have_grid;
	float dev_hz, centre_hz;
	/* Bits: the record, the most it takes. */
	struct dec_generic_out *out;
	uint32_t cap;
};

static void *align8(void *p)
{
	return (void *)(((uintptr_t)p + 7U) & ~(uintptr_t)7U);
}

static size_t al8(size_t v)
{
	return (v + 7U) & ~(size_t)7U;
}

/* A sample's power (up to 2^31: unsigned). */
static inline uint32_t pw(const int16_t *iq, uint32_t k)
{
	int32_t i = iq[2U * k], q = iq[2U * k + 1U];

	return (uint32_t)(i * i) + (uint32_t)(q * q);
}

/*
 * A sample's power over 2^PW_SHIFT: the running sums of up to LAG_MAX of them stay within 32 bits
 * (whose conversion to float is an instruction; a 64-bit one is a library call on the P4).
 */
#define PW_SHIFT 6U
static inline uint32_t pws(const int16_t *iq, uint32_t k)
{
	return pw(iq, k) >> PW_SHIFT;
}

/* A sum of binary angles back to -32768 .. 32767. */
static inline int32_t wrap16(int32_t v)
{
	return ((v + 32768) & 0xFFFF) - 32768;
}

/* Floor, ceiling and nearest integer of a float well within int32, without a library call. */
static inline int32_t ifloor(float x)
{
	int32_t i = (int32_t)x;

	return (float)i > x ? i - 1 : i;
}

static inline int32_t iceil(float x)
{
	int32_t i = (int32_t)x;

	return (float)i < x ? i + 1 : i;
}

static inline int32_t iround(float x)
{
	return ifloor(x + 0.5f);
}

/*
 * cos and sin of tau u (u in turns) within 2e-4: polynomials about 0 after folding into a
 * quarter turn either side. Enough for the grid's circular means, cheaper than cosf(), sinf().
 */
static void sincos_turn(float u, float *c, float *s)
{
	float x, x2, k = 1.0f;

	u -= (float)iround(u);
	if (u > 0.25f) {
		u = 0.5f - u;
		k = -1.0f;
	} else if (u < -0.25f) {
		u = -0.5f - u;
		k = -1.0f;
	}
	x = ESP_SDR_TAU_F * u;
	x2 = x * x;
	*s = x * (1.0f -
		  x2 * (1.0f / 6.0f) * (1.0f - x2 * (1.0f / 20.0f) * (1.0f - x2 * (1.0f / 42.0f))));
	*c = k * (1.0f - x2 * 0.5f *
				 (1.0f - x2 * (1.0f / 12.0f) *
						 (1.0f - x2 * (1.0f / 30.0f) *
								 (1.0f - x2 * (1.0f / 56.0f)))));
}

static inline float sh(const float *acc, uint32_t l, uint32_t j)
{
	return acc[(j + l / 2U) & (l - 1U)];
}

/* The k-th smallest of v[0 .. n) (reorders v). */
static float kth(float *v, uint32_t n, uint32_t k)
{
	int32_t lo = 0, hi = (int32_t)n - 1, kk = (int32_t)k;

	while (lo < hi) {
		float p = v[(lo + hi) / 2];
		int32_t i = lo, j = hi;

		while (i <= j) {
			while (v[i] < p) {
				i++;
			}
			while (v[j] > p) {
				j--;
			}
			if (i <= j) {
				float t = v[i];

				v[i] = v[j];
				v[j] = t;
				i++;
				j--;
			}
		}
		if (kk <= j) {
			hi = j;
		} else if (kk >= i) {
			lo = i;
		} else {
			break;
		}
	}
	return v[kk];
}

/* ---- spectrum ---- */

/* cos(tau k / SPEC_N) for k = 0 .. SPEC_N / 4: a quarter wave (65 cosf() calls cost a short
 * burst's analysis some 10 % on the P4). */
static const float qwave[SPEC_N / 4U + 1U] = {
	1.0f, 0.999698819f, 0.998795456f, 0.997290457f, 0.995184727f, 0.992479535f, 0.98917651f,
	0.985277642f, 0.98078528f, 0.97570213f, 0.970031253f, 0.963776066f, 0.956940336f,
	0.949528181f, 0.941544065f, 0.932992799f, 0.923879533f, 0.914209756f, 0.903989293f,
	0.893224301f, 0.881921264f, 0.870086991f, 0.85772861f, 0.844853565f, 0.831469612f,
	0.817584813f, 0.803207531f, 0.788346428f, 0.773010453f, 0.757208847f, 0.740951125f,
	0.724247083f, 0.707106781f, 0.689540545f, 0.671558955f, 0.653172843f, 0.634393284f,
	0.615231591f, 0.595699304f, 0.575808191f, 0.555570233f, 0.53499762f, 0.514102744f,
	0.492898192f, 0.471396737f, 0.44961133f, 0.427555093f, 0.405241314f, 0.382683432f,
	0.359895037f, 0.336889853f, 0.31368174f, 0.290284677f, 0.266712757f, 0.24298018f,
	0.21910124f, 0.195090322f, 0.170961889f, 0.146730474f, 0.122410675f, 0.0980171403f,
	0.0735645636f, 0.0490676743f, 0.0245412285f, 0.0f
};
_Static_assert(SPEC_N == 256U, "qwave is the quarter wave of 256 points");

/* The transform's twiddles (esp_sdr_fft_twiddles()'s for SPEC_N) from the quarter wave, by
 * symmetry. */
static void twiddles(struct gws *ws)
{
	const float *q = qwave;

	for (uint32_t k = 0; k < SPEC_N / 2U; k++) {
		ws->twc[k] = k <= SPEC_N / 4U ? q[k] : -q[SPEC_N / 2U - k];
		ws->tws[k] = -(k <= SPEC_N / 4U ? q[SPEC_N / 4U - k] : q[k - SPEC_N / 4U]);
	}
	/* The reversals by recurrence: k's from k / 2's (k / 4's) and its last bit (digit). */
	ws->rev[0] = 0U;
	ws->rev4[0] = 0U;
	for (uint32_t k = 1; k < SPEC_N; k++) {
		ws->rev[k] = (uint8_t)((ws->rev[k >> 1] >> 1) | ((k & 1U) * (SPEC_N / 2U)));
		ws->rev4[k] = (uint8_t)((ws->rev4[k >> 2] >> 2) | ((k & 3U) * (SPEC_N / 4U)));
	}
	ws->win_n = 0U;
}

/* The twiddle W^m = exp(-j tau m / SPEC_N) for m below 3 SPEC_N / 4. */
static inline void tw(const struct gws *ws, uint32_t m, float *wr, float *wi)
{
	if (m < SPEC_N / 2U) {
		*wr = ws->twc[m];
		*wi = ws->tws[m];
	} else {
		*wr = -ws->twc[m - SPEC_N / 2U];
		*wi = -ws->tws[m - SPEC_N / 2U];
	}
}

/*
 * fft() for l a power of four (16, 64, SPEC_N), radix 4, the inputs in base-4 digit-reversed
 * order: three complex multiplications for four points where radix 2 takes four, half the
 * passes; the first stage without multiplications.
 */
static void fft4(struct gws *ws, uint32_t l)
{
	float *re = ws->re, *im = ws->im;

	for (uint32_t len = 4U; len <= l; len <<= 2) {
		uint32_t q = len >> 2, step = SPEC_N / len;

		for (uint32_t k = 0; k < q; k++) {
			float w1r = 1.0f, w1i = 0.0f, w2r = 1.0f, w2i = 0.0f, w3r = 1.0f,
			      w3i = 0.0f;

			if (k > 0U) {
				tw(ws, k * step, &w1r, &w1i);
				tw(ws, 2U * k * step, &w2r, &w2i);
				tw(ws, 3U * k * step, &w3r, &w3i);
			}
			for (uint32_t i = k; i < l; i += len) {
				float a0r = re[i], a0i = im[i];
				float a1r = re[i + q] * w1r - im[i + q] * w1i;
				float a1i = re[i + q] * w1i + im[i + q] * w1r;
				float a2r = re[i + 2U * q] * w2r - im[i + 2U * q] * w2i;
				float a2i = re[i + 2U * q] * w2i + im[i + 2U * q] * w2r;
				float a3r = re[i + 3U * q] * w3r - im[i + 3U * q] * w3i;
				float a3i = re[i + 3U * q] * w3i + im[i + 3U * q] * w3r;
				float b0r = a0r + a2r, b0i = a0i + a2i, b1r = a0r - a2r,
				      b1i = a0i - a2i;
				float b2r = a1r + a3r, b2i = a1i + a3i, b3r = a1r - a3r,
				      b3i = a1i - a3i;

				re[i] = b0r + b2r;
				im[i] = b0i + b2i;
				re[i + 2U * q] = b0r - b2r;
				im[i + 2U * q] = b0i - b2i;
				/* b1 -+ j b3 */
				re[i + q] = b1r + b3i;
				im[i + q] = b1i - b3r;
				re[i + 3U * q] = b1r - b3i;
				im[i + 3U * q] = b1i + b3r;
			}
		}
	}
}

/*
 * The forward transform of ws->re, ws->im in place (esp_sdr_fft()'s, l points, a power of two
 * from 4 to SPEC_N), the inputs in bit-reversed order (spectrum() loads them so), the output in
 * natural order: radix 2, the first two stages without multiplications (twiddles 1 and -j), the
 * twiddles loaded once a stage and index.
 */
static void fft(struct gws *ws, uint32_t l)
{
	float *re = ws->re, *im = ws->im;

	for (uint32_t i = 0; i < l; i += 4U) {
		float ar = re[i] + re[i + 1U], ai = im[i] + im[i + 1U];
		float br = re[i] - re[i + 1U], bi = im[i] - im[i + 1U];
		float cr = re[i + 2U] + re[i + 3U], ci = im[i + 2U] + im[i + 3U];
		float dr = re[i + 2U] - re[i + 3U], di = im[i + 2U] - im[i + 3U];

		re[i] = ar + cr;
		im[i] = ai + ci;
		re[i + 2U] = ar - cr;
		im[i + 2U] = ai - ci;
		re[i + 1U] = br + di;
		im[i + 1U] = bi - dr;
		re[i + 3U] = br - di;
		im[i + 3U] = bi + dr;
	}
	for (uint32_t len = 8U; len <= l; len <<= 1) {
		uint32_t half = len >> 1, step = SPEC_N / len;

		for (uint32_t k = 0; k < half; k++) {
			float wr = ws->twc[k * step], wi = ws->tws[k * step];

			for (uint32_t a = k; a < l; a += len) {
				uint32_t b = a + half;
				float xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;

				re[b] = re[a] - xr;
				im[b] = im[a] - xi;
				re[a] += xr;
				im[a] += xi;
			}
		}
	}
}

/*
 * The mean power spectrum of iq[0 .. n) in ws->acc, natural order, its bins summing to the mean
 * power a sample: up to @p maxsegs Hann windowed segments spread over the stream, as many as
 * cover it. @p kind 0: the samples; 1: their squares; 2: their fourth powers over their
 * power (the lines of BPSK and QPSK). Returns the points (0: too short), the segments in *segs.
 */
static uint32_t spectrum(struct gws *ws, const int16_t *iq, uint32_t n, int kind, uint32_t maxsegs,
			 uint32_t *segs)
{
	uint32_t l = SPEC_N, ns, shift = 0U, j;
	float norm;
	bool r4;

	while (l > n && l > SPEC_MIN) {
		l >>= 1;
	}
	if (l > n) {
		return 0U;
	}
	if (ws->win_n != l) {
		ws->wsum2 = 0.0f;
		/* Hann, 0.5 - 0.5 cos(tau k / l), the cosine from the twiddles. */
		for (uint32_t k = 0; k < l; k++) {
			uint32_t m = k * (SPEC_N / l);

			ws->win[k] = 0.5f - 0.5f * (m < SPEC_N / 2U ? ws->twc[m]
								    : -ws->twc[m - SPEC_N / 2U]);
			ws->wsum2 += ws->win[k] * ws->win[k];
		}
		ws->win_n = l;
	}
	/* Segments enough to cover the whole stream (spread over it beyond SPEC_SEGS). */
	ns = (n + l - 1U) / l;
	if (ns > maxsegs) {
		ns = maxsegs;
	}
	while ((SPEC_N >> shift) > l) {
		shift++;
	}
	/* Radix 4 for a power of four (SPEC_N is one: shift even). */
	r4 = (shift & 1U) == 0U;
	memset(ws->acc, 0, sizeof(ws->acc[0]) * l);
	for (uint32_t s = 0; s < ns; s++) {
		uint32_t at =
			ns > 1U ? (uint32_t)((uint64_t)(n - l) * s / (ns - 1U)) : (n - l) / 2U;
		const int16_t *x = iq + 2U * at;

		for (uint32_t k = 0; k < l; k++) {
			float xr = x[2U * k], xi = x[2U * k + 1U];

			if (kind > 0) {
				float p = xr * xr + xi * xi, t = xr * xr - xi * xi;

				xi = 2.0f * xr * xi;
				xr = t;
				if (kind > 1) {
					t = p > 0.0f ? 1.0f / p : 0.0f;
					p = (xr * xr - xi * xi) * t;
					xi = 2.0f * xr * xi * t;
					xr = p;
				}
			}
			/* Into the order fft4() or fft() takes. */
			j = r4 ? ws->rev4[k] >> shift : ws->rev[k] >> shift;
			ws->re[j] = ws->win[k] * xr;
			ws->im[j] = ws->win[k] * xi;
		}
		if (r4) {
			fft4(ws, l);
		} else {
			fft(ws, l);
		}
		for (uint32_t k = 0; k < l; k++) {
			ws->acc[k] += ws->re[k] * ws->re[k] + ws->im[k] * ws->im[k];
		}
	}
	norm = 1.0f / ((float)ns * (float)l * ws->wsum2);
	for (uint32_t k = 0; k < l; k++) {
		ws->acc[k] *= norm;
	}
	*segs = ns;
	return l;
}

/* The 25th percentile of the mean of @p segs unit exponentials (Wilson and Hilferty). */
static float q25(uint32_t segs)
{
	float c = 1.0f / (9.0f * (float)segs);
	float v = 1.0f - c - 0.6745f * sqrtf(c);

	return v * v * v;
}

/* Parabolic interpolation of a peak at the middle of three values: the offset, -0.5 .. 0.5. */
static float parabola(float a, float b, float c)
{
	float d = a - 2.0f * b + c;

	return d < 0.0f ? 0.5f * (a - c) / d : 0.0f;
}

/*
 * Centre, bandwidth, signal and noise of a stream at @p rate from its spectrum. The noise per
 * bin: the corrected 25th percentile of the central three quarters of the band (a refined
 * stream's half-band stages lower it towards the band's edges), or the share of @p floor (power
 * a sample, 0 unknown) when it comes out above twice that (a signal filling the band). The
 * signal: what stands above it, of a refined stream in the central three quarters only; its
 * centre and width the band of its middle 90 %. False when the stream is too short.
 */
static bool measure(struct gws *ws, const int16_t *iq, uint32_t n, float rate, float floor,
		    bool refined, struct meas *m)
{
	uint32_t segs, l = spectrum(ws, iq, n, 0, SPEC_SEGS, &segs), c0, c1, j0, j1, cnt = 0U,
		       jp = 0U;
	float nb, tot = 0.0f, cum = 0.0f, p5 = -1.0f, p95 = -1.0f, best = -1.0f, cs = 0.0f;

	memset(m, 0, sizeof(*m));
	if (l == 0U) {
		return false;
	}
	m->bin = rate / (float)l;
	c0 = l / 8U;
	c1 = l - l / 8U;
	for (uint32_t j = c0; j < c1; j++) {
		ws->tmp[cnt++] = sh(ws->acc, l, j);
	}
	nb = kth(ws->tmp, cnt, cnt / 4U) / q25(segs);
	if (floor > 0.0f && nb > 2.0f * floor / (float)l) {
		nb = floor / (float)l;
	}
	m->noise = nb * (float)l;
	j0 = refined ? c0 : 0U;
	j1 = refined ? c1 : l;
	for (uint32_t j = j0; j < j1; j++) {
		tot += sh(ws->acc, l, j) - nb;
	}
	if (!(tot > 0.0f)) {
		return true;
	}
	m->sig = tot;
	for (uint32_t j = j0; j < j1; j++) {
		float s = sh(ws->acc, l, j) - nb;

		if (p5 < 0.0f && cum + s >= 0.05f * tot) {
			p5 = (float)j - 0.5f + (0.05f * tot - cum) / s;
		}
		if (p95 < 0.0f && cum + s >= 0.95f * tot) {
			p95 = (float)j - 0.5f + (0.95f * tot - cum) / s;
		}
		cum += s;
		if (s > best) {
			best = s;
			jp = j;
		}
	}
	if (p5 < 0.0f || p95 < p5) {
		return true;
	}
	m->centre = (0.5f * (p5 + p95) - (float)(l / 2U)) * m->bin;
	m->bw = (p95 - p5) * m->bin;
	for (uint32_t j = jp >= j0 + 2U ? jp - 2U : j0; j <= jp + 2U && j < j1; j++) {
		cs += sh(ws->acc, l, j) - nb;
	}
	m->conc = cs / tot;
	m->peak = (float)jp - (float)(l / 2U);
	if (jp > 0U && jp + 1U < l) {
		m->peak += parabola(sh(ws->acc, l, jp - 1U), sh(ws->acc, l, jp),
				    sh(ws->acc, l, jp + 1U));
	}
	m->peak *= m->bin;
	return true;
}

/* The strongest line of the last spectrum (frequency, Hz) and its share of the power. */
static float line(const struct gws *ws, uint32_t l, float rate, float *freq)
{
	uint32_t jp = 0U;
	float tot = 0.0f, best = -1.0f, d;

	for (uint32_t j = 0; j < l; j++) {
		float v = sh(ws->acc, l, j);

		tot += v;
		if (v > best) {
			best = v;
			jp = j;
		}
	}
	if (!(tot > 0.0f) || jp == 0U || jp + 1U >= l) {
		*freq = 0.0f;
		return 0.0f;
	}
	d = parabola(sh(ws->acc, l, jp - 1U), best, sh(ws->acc, l, jp + 1U));
	*freq = ((float)jp + d - (float)(l / 2U)) * rate / (float)l;
	return (sh(ws->acc, l, jp - 1U) + best + sh(ws->acc, l, jp + 1U)) / tot;
}

/* ---- the loud part ---- */

/*
 * The loud part [*pa, *pe) shrunk to where the envelope (a TRIM_WIN window) stands a quarter of
 * the way from the floor to the part's mean power (the edge a quarter window in), when the part
 * is clearly above the floor.
 */
static void trim(const int16_t *iq, float floor, uint32_t *pa, uint32_t *pe)
{
	uint32_t a = *pa, e = *pe, s0 = a, e0 = e, run;
	bool f0 = false, f1 = false;
	uint64_t sum = 0U;
	float mean = 0.0f, thr;

	if (!(floor > 0.0f) || e - a < 4U * TRIM_WIN) {
		return;
	}
	for (uint32_t k = a; k < e; k++) {
		sum += pw(iq, k);
	}
	mean = (float)sum / (float)(e - a);
	if (mean < 2.0f * floor) {
		return;
	}
	/* On the window's sum of the powers over 2^PW_SHIFT. */
	thr = (floor + 0.25f * (mean - floor)) * (float)TRIM_WIN / (float)(1U << PW_SHIFT);
	run = 0U;
	for (uint32_t k = a; k < e; k++) {
		run += pws(iq, k);
		if (k >= a + TRIM_WIN) {
			run -= pws(iq, k - TRIM_WIN);
		}
		if (k + 1U >= a + TRIM_WIN && (float)run >= thr) {
			/* A quarter of the window holds the signal: a sharp edge lies there. */
			s0 = k + 1U - TRIM_WIN / 4U;
			f0 = true;
			break;
		}
	}
	run = 0U;
	for (uint32_t k = e; k-- > a;) {
		run += pws(iq, k);
		if (k + TRIM_WIN < e) {
			run -= pws(iq, k + TRIM_WIN);
		}
		if (k + TRIM_WIN <= e && (float)run >= thr) {
			e0 = k + TRIM_WIN / 4U;
			f1 = true;
			break;
		}
	}
	if (f0 && f1 && e0 >= s0 + 2U * TRIM_WIN) {
		*pa = s0;
		*pe = e0;
	}
}

/* ---- features of the final stream ---- */

/*
 * The phase steps less @p centre (binary angles a sample: the frequency about the centre) and,
 * in the same pass, the power's statistics for envelope(): its mean, its squared coefficient of
 * variation, its largest mean over @p ms consecutive samples.
 */
static void steps(const int16_t *iq, uint32_t n, int32_t centre, uint32_t ms, int16_t *st,
		  struct env *ev)
{
	float s1 = 0.0f, s2 = 0.0f;
	uint32_t run = 0U, rmax = 0U;

	memset(ev, 0, sizeof(*ev));
	for (uint32_t k = 0; k < n; k++) {
		uint32_t pk = pw(iq, k);
		float p = (float)pk;

		s1 += p;
		s2 += p * p;
		run += pk >> PW_SHIFT;
		if (k >= ms) {
			run -= pws(iq, k - ms);
		}
		if (k + 1U >= ms && run > rmax) {
			rmax = run;
		}
		st[k] = k == 0U ? 0
				: (int16_t)wrap16(dec_step(iq[2U * k - 2U], iq[2U * k - 1U],
							   iq[2U * k], iq[2U * k + 1U]) -
						  centre);
	}
	if (n > 0U) {
		ev->mean = s1 / (float)n;
		ev->cv2 = ev->mean > 0.0f ? s2 / (float)n / (ev->mean * ev->mean) - 1.0f : 0.0f;
	}
	ev->pmax = (float)rmax * (float)(1U << PW_SHIFT);
}

/* The median of histogram bins [b0, b1), in bins (interpolated within the bin). */
static float hmedian(const float *h, uint32_t b0, uint32_t b1)
{
	float tot = 0.0f, cum = 0.0f;

	for (uint32_t i = b0; i < b1; i++) {
		tot += h[i];
	}
	for (uint32_t i = b0; i < b1; i++) {
		if (h[i] > 0.0f && cum + h[i] >= 0.5f * tot) {
			return (float)i + (0.5f * tot - cum) / h[i];
		}
		cum += h[i];
	}
	return (float)b0;
}

/*
 * The two-level split (Otsu) of the power smoothed over @p ms samples: the histogram over
 * [0, ev->pmax) that steps() measured.
 */
static void envelope(const int16_t *iq, uint32_t n, uint32_t ms, struct env *ev)
{
	float h[EH_N], pmax = ev->pmax, scale, mt = 0.0f, vt = 0.0f, best = 0.0f;
	float c0 = 0.0f, m0 = 0.0f, cnt = 0.0f;
	uint32_t split = 1U, run = 0U;

	ev->lo = 0.0f;
	ev->hi = 0.0f;
	ev->sep = 0.0f;
	ev->wlo = 0.0f;
	if (n == 0U || !(pmax > 0.0f)) {
		return;
	}
	scale = (float)EH_N / (pmax * 1.0001f);
	memset(h, 0, sizeof(h));
	run = 0U;
	scale *= (float)(1U << PW_SHIFT);
	for (uint32_t k = 0; k < n; k++) {
		run += pws(iq, k);
		if (k >= ms) {
			run -= pws(iq, k - ms);
		}
		if (k + 1U >= ms) {
			h[(uint32_t)((float)run * scale)] += 1.0f;
			cnt += 1.0f;
		}
	}
	scale /= (float)(1U << PW_SHIFT);
	for (uint32_t i = 0; i < EH_N; i++) {
		mt += h[i] * ((float)i + 0.5f);
	}
	mt /= cnt;
	for (uint32_t i = 0; i < EH_N; i++) {
		float d = (float)i + 0.5f - mt;

		vt += h[i] * d * d;
	}
	vt /= cnt;
	for (uint32_t t = 1; t < EH_N; t++) {
		float w0, w1, mu0, mu1, sb;

		c0 += h[t - 1U];
		m0 += h[t - 1U] * ((float)t - 0.5f);
		w0 = c0 / cnt;
		w1 = 1.0f - w0;
		if (c0 <= 0.0f || w1 <= 0.0f) {
			continue;
		}
		mu0 = m0 / c0;
		mu1 = (mt * cnt - m0) / (cnt - c0);
		sb = w0 * w1 * (mu1 - mu0) * (mu1 - mu0);
		if (sb > best) {
			best = sb;
			split = t;
			ev->wlo = w0;
		}
	}
	ev->sep = vt > 0.0f ? best / vt : 0.0f;
	/* The levels: each class's median (the samples smoothed across an edge are few). */
	ev->lo = hmedian(h, 0U, split) / (scale * (float)ms);
	ev->hi = hmedian(h, split, EH_N) / (scale * (float)ms);
}

/* Histogram of the phase steps over a lag of @p w samples, weighted by the samples' power. */
static void fhist(const int16_t *iq, const int16_t *st, uint32_t n, uint32_t w, float *h)
{
	int32_t run = 0;

	memset(h, 0, sizeof(h[0]) * FH_N);
	for (uint32_t k = 1; k < n; k++) {
		run += st[k];
		if (k > w) {
			run -= st[k - w];
		}
		if (k >= w) {
			uint32_t bin = (uint32_t)(wrap16(run) + 32768) >> FH_SHIFT;

			h[bin] += 0.5f * ((float)pw(iq, k) + (float)pw(iq, k - w));
		}
	}
}

/*
 * Peaks of a histogram of FH_N bins, in order: local maxima of the histogram smoothed by
 * @p passes of a (1 2 1) kernel, merged without a valley between them, the light ones dropped.
 * Returns their count (up to PK_MAX).
 */
static unsigned int fpeaks(const float *h, unsigned int passes, struct peak *pk)
{
	float s[FH_N], t[FH_N], gmax = 0.0f, wsum = 0.0f;
	unsigned int idx[FH_N], np = 0U, out = 0U;
	bool again = true;

	memcpy(s, h, sizeof(s));
	for (unsigned int p = 0; p < passes; p++) {
		memcpy(t, s, sizeof(t));
		for (unsigned int i = 0; i < FH_N; i++) {
			float l = t[i > 0U ? i - 1U : i], r = t[i + 1U < FH_N ? i + 1U : i];

			s[i] = 0.25f * l + 0.5f * t[i] + 0.25f * r;
		}
	}
	for (unsigned int i = 0; i < FH_N; i++) {
		gmax = s[i] > gmax ? s[i] : gmax;
	}
	if (!(gmax > 0.0f)) {
		return 0U;
	}
	for (unsigned int i = 1; i + 1U < FH_N; i++) {
		if (s[i] > s[i - 1U] && s[i] >= s[i + 1U] && s[i] >= PK_REL * gmax) {
			idx[np++] = i;
		}
	}
	/* Neighbours without a valley between them are one peak, the higher one. */
	while (again && np > 1U) {
		again = false;
		for (unsigned int k = 0; k + 1U < np; k++) {
			unsigned int a = idx[k], b = idx[k + 1U];
			float v = s[a], lower = s[a] < s[b] ? s[a] : s[b];

			for (unsigned int i = a; i <= b; i++) {
				v = s[i] < v ? s[i] : v;
			}
			if (v > PK_VALLEY * lower) {
				unsigned int drop = s[a] < s[b] ? k : k + 1U;

				for (unsigned int i = drop; i + 1U < np; i++) {
					idx[i] = idx[i + 1U];
				}
				np--;
				again = true;
				break;
			}
		}
	}
	for (unsigned int k = 0; k < np; k++) {
		unsigned int i = idx[k], lo = k > 0U ? idx[k - 1U] : 0U;
		unsigned int hi = k + 1U < np ? idx[k + 1U] : FH_N - 1U;
		float wt = 0.0f;

		/* The weight: from the valley below to the valley above. */
		for (unsigned int j = lo; j < i; j++) {
			if (s[j] < s[lo]) {
				lo = j;
			}
		}
		for (unsigned int j = i; j <= hi; j++) {
			if (s[j] < s[hi]) {
				hi = j;
			}
		}
		for (unsigned int j = lo; j <= hi; j++) {
			wt += h[j];
		}
		if (out < PK_MAX) {
			pk[out].pos = (float)i + 0.5f + parabola(s[i - 1U], s[i], s[i + 1U]);
			pk[out].wt = wt;
			out++;
		}
	}
	/* Peaks far lighter than the average one are noise or transitions: dropped. */
	np = out;
	out = 0U;
	for (unsigned int k = 0; k < np; k++) {
		wsum += pk[k].wt;
	}
	for (unsigned int k = 0; k < np; k++) {
		if (pk[k].wt >= PK_LIGHT * wsum / (float)np) {
			pk[out++] = pk[k];
		}
	}
	return out;
}

/* ---- transitions and the symbol grid ---- */

static void tr_add(struct gws *ws, float t)
{
	if (ws->ntr < TR_MAX) {
		ws->tr[ws->ntr++] = t;
	}
}

/* Pairs of transitions closer than @p dmin (noise crossing back and forth) are dropped. */
static void tr_clean(struct gws *ws, float dmin)
{
	uint32_t o = 0U;

	for (uint32_t i = 0; i < ws->ntr; i++) {
		if (o > 0U && ws->tr[i] - ws->tr[o - 1U] < dmin) {
			o--;
			continue;
		}
		ws->tr[o++] = ws->tr[i];
	}
	ws->ntr = o;
}

/*
 * Transitions of a value v[k] about 0 with hysteresis: a crossing counts once the value has
 * passed @p hyst on the other side; its time is the interpolated zero crossing less @p delay.
 */
struct xing {
	int state;
	bool have;
	float prev, tz, delay, hyst;
};

static inline void xing_put(struct gws *ws, struct xing *x, uint32_t k, float v)
{
	if (x->have && (v >= 0.0f) != (x->prev >= 0.0f)) {
		x->tz = (float)k - 1.0f + x->prev / (x->prev - v);
	}
	if (x->state >= 0 && v < -x->hyst) {
		if (x->state > 0) {
			tr_add(ws, x->tz - x->delay);
		}
		x->state = -1;
	} else if (x->state <= 0 && v > x->hyst) {
		if (x->state < 0) {
			tr_add(ws, x->tz - x->delay);
		}
		x->state = 1;
	}
	x->prev = v;
	x->have = true;
}

/*
 * Sign changes of the frequency against @p centre: the phase steps over a lag of @p w (binary
 * angles over w, as @p centre and @p hyst) summed over w lags, a triangular window of 2 w - 1
 * steps whose noise is that of w lags' (the phase's at both ends, averaged over w samples).
 */
static void tr_freq(struct gws *ws, const int16_t *st, uint32_t n, uint32_t w, float centre,
		    float hyst)
{
	struct xing x = {.delay = (float)w - 0.5f, .hyst = hyst * (float)w};
	int32_t run = 0, run2 = 0, ring[LAG_MAX];
	uint32_t lags = 0U, ri = 0U;

	ws->ntr = 0U;
	for (uint32_t k = 1; k < n; k++) {
		run += st[k];
		if (k > w) {
			run -= st[k - w];
		}
		if (k >= w) {
			int32_t v = wrap16(run);

			run2 += v - (lags >= w ? ring[ri] : 0);
			ring[ri] = v;
			ri = ri + 1U == w ? 0U : ri + 1U;
			if (++lags >= w) {
				xing_put(ws, &x, k, (float)run2 - centre * (float)w);
			}
		}
	}
	tr_clean(ws, 0.75f * (float)w);
}

/* Crossings of the power smoothed over @p ms samples through @p mid. */
static void tr_env(struct gws *ws, const int16_t *iq, uint32_t n, uint32_t ms, float mid,
		   float hyst)
{
	struct xing x = {.delay = 0.5f * (float)(ms - 1U), .hyst = hyst};
	const float scale = (float)(1U << PW_SHIFT) / (float)ms;
	uint32_t run = 0U;

	ws->ntr = 0U;
	for (uint32_t k = 0; k < n; k++) {
		run += pws(iq, k);
		if (k >= ms) {
			run -= pws(iq, k - ms);
		}
		if (k + 1U >= ms) {
			xing_put(ws, &x, k, (float)run * scale - mid);
		}
	}
	tr_clean(ws, (float)ms);
}

/*
 * Phase steps: runs of samples whose phase step over a lag of @p w, less the drift (binary angle
 * a sample), in magnitude and summed over w lags (a triangular window, as tr_freq()) exceeds
 * @p thr w; a run's time is its middle less w - 1/2.
 */
static void tr_phase(struct gws *ws, const int16_t *st, uint32_t n, uint32_t w, float drift,
		     int32_t thr)
{
	int32_t run = 0, run2 = 0, ring[LAG_MAX], d = (int32_t)lrintf(drift * (float)w);
	uint32_t first = 0U, lags = 0U, ri = 0U;
	bool in = false;

	ws->ntr = 0U;
	for (uint32_t k = 1; k < n; k++) {
		bool on = false;

		run += st[k];
		if (k > w) {
			run -= st[k - w];
		}
		if (k >= w) {
			int32_t v = wrap16(run - d);

			/* Magnitudes: a step of pi wraps to either sign. */
			v = v < 0 ? -v : v;
			run2 += v - (lags >= w ? ring[ri] : 0);
			ring[ri] = v;
			ri = ri + 1U == w ? 0U : ri + 1U;
			on = ++lags >= w && run2 > thr * (int32_t)w;
		}
		if (on && !in) {
			first = k;
		} else if (!on && in) {
			tr_add(ws, 0.5f * (float)(first + k - 1U) - (float)w + 0.5f);
		}
		in = on;
	}
	tr_clean(ws, (float)w);
}

/* Transitions within a quarter period of the grid a + k t. */
static uint32_t grid_on(const struct gws *ws, float t, float a)
{
	uint32_t on = 0U;

	for (uint32_t i = 0; i < ws->ntr; i++) {
		float k = (float)iround((ws->tr[i] - a) / t);

		if (fabsf(ws->tr[i] - a - k * t) < 0.25f * t) {
			on++;
		}
	}
	return on;
}

/*
 * Period and phase against a period @p t: the transitions' phases 2 pi t_i / t drift by
 * 2 pi eps a symbol when the period is t (1 + eps). Their circular means over blocks of @p blk
 * transitions (noise and stray transitions averaged), unwrapped from block to block (a drift
 * below pi a block), and a line through them in time (weights the blocks' coherence) give the
 * corrected period *tn and the first boundary *an (boundaries at *an + k *tn).
 */
static bool grid_phase(struct gws *ws, float t, uint32_t blk, float *tn, float *an)
{
	float sw = 0.0f, st = 0.0f, sp = 0.0f, stt = 0.0f, stp = 0.0f, prev = 0.0f, unw = 0.0f;
	float *tb = ws->bt, *pb = ws->bp, *wb = ws->bc, slope;
	uint32_t ntr = ws->ntr, nb = 0U;

	for (uint32_t b0 = 0; b0 < ntr && nb < TR_MAX / GRID_BLK1; b0 += blk) {
		uint32_t b1 = b0 + blk <= ntr ? b0 + blk : ntr;
		float cr = 0.0f, ci = 0.0f, tm = 0.0f;

		if (b1 - b0 < (blk + 1U) / 2U && nb > 0U) {
			break;
		}
		for (uint32_t i = b0; i < b1; i++) {
			float pc, ps;

			sincos_turn((ws->tr[i] - ws->tr[0]) / t, &pc, &ps);
			cr += pc;
			ci += ps;
			tm += ws->tr[i] - ws->tr[0];
		}
		tb[nb] = tm / (float)(b1 - b0);
		pb[nb] = atan2f(ci, cr);
		wb[nb] = sqrtf(cr * cr + ci * ci) / (float)(b1 - b0);
		nb++;
	}
	for (uint32_t b = 0; b < nb; b++) {
		float d = pb[b] - prev;

		d -= ESP_SDR_TAU_F * (float)iround(d / ESP_SDR_TAU_F);
		unw = b == 0U ? pb[0] : unw + d;
		prev = pb[b];
		pb[b] = unw;
		sw += wb[b];
		st += wb[b] * tb[b];
		sp += wb[b] * unw;
	}
	if (!(sw > 0.0f)) {
		return false;
	}
	st /= sw;
	sp /= sw;
	for (uint32_t b = 0; b < nb; b++) {
		stt += wb[b] * (tb[b] - st) * (tb[b] - st);
		stp += wb[b] * (tb[b] - st) * (pb[b] - sp);
	}
	slope = stt > 0.0f ? stp / stt : 0.0f;
	*tn = t * (1.0f + slope * t / ESP_SDR_TAU_F);
	*an = ws->tr[0] + (sp - slope * st) / ESP_SDR_TAU_F * t;
	return *tn > 0.0f;
}

/*
 * The symbol grid through the transitions. A first period: the highest cluster of the intervals
 * between consecutive transitions (at least @p tmin samples) in a histogram over four times
 * their median, or half of it when intervals of half that length are many too (any one of them
 * for @p clean transitions: the smallest cluster); the intervals of one to four periods refine
 * it. The transitions' phase against that period, averaged over blocks and unwrapped from block
 * to block, drifts with the period's error: a line through it corrects period and phase over the
 * whole burst (the line of the transition signal at the symbol rate, located). A least-squares
 * fit through the transitions on that grid, and the check that the period is neither twice nor
 * half the one they keep, end it; the share of transitions on the grid is its quality. False
 * without GRID_MIN transitions.
 */
static bool grid_fit(struct gws *ws, float tmin, bool clean, struct grid *g)
{
	float lh[LH_N], t0 = 0.0f, t, a, num, den, med, bin, best = 0.0f;
	uint32_t ntr = ws->ntr, cnt = 0U, on;
	unsigned int pk = 0U;

	memset(g, 0, sizeof(*g));
	if (ntr < GRID_MIN) {
		return false;
	}
	if (tmin < 1.0f) {
		tmin = 1.0f;
	}
	for (uint32_t i = 1; i < ntr && cnt < SPEC_N; i++) {
		float d = ws->tr[i] - ws->tr[i - 1U];

		if (d >= tmin) {
			ws->tmp[cnt++] = d;
		}
	}
	if (cnt < 5U) {
		return false;
	}
	med = kth(ws->tmp, cnt, cnt / 2U);
	bin = 4.0f * med / (float)LH_N;
	memset(lh, 0, sizeof(lh));
	for (uint32_t i = 1; i < ntr; i++) {
		float d = ws->tr[i] - ws->tr[i - 1U];

		if (d >= tmin && d < 4.0f * med) {
			lh[(uint32_t)(d / bin)] += 1.0f;
		}
	}
	/* Smoothed over five bins (1 2 3 2 1): the highest cluster. */
	for (unsigned int i = 0; i < LH_N; i++) {
		float v = 3.0f * lh[i];

		v += i >= 1U ? 2.0f * lh[i - 1U] : 0.0f;
		v += i >= 2U ? lh[i - 2U] : 0.0f;
		v += i + 1U < LH_N ? 2.0f * lh[i + 1U] : 0.0f;
		v += i + 2U < LH_N ? lh[i + 2U] : 0.0f;
		if (v > best) {
			best = v;
			pk = i;
		}
	}
	t = ((float)pk + 0.5f) * bin;
	for (int it = 0; it < 2; it++) {
		/* The cluster's mean interval, within 15 % of t; half of it if that one is full. */
		float nh = 0.0f;

		num = 0.0f;
		den = 0.0f;
		for (uint32_t i = 1; i < ntr; i++) {
			float d = ws->tr[i] - ws->tr[i - 1U];

			if (d > 0.85f * t && d < 1.15f * t) {
				num += d;
				den += 1.0f;
			} else if (d > 0.425f * t && d < 0.575f * t && d >= tmin) {
				nh += 1.0f;
			}
		}
		if (den < 2.0f) {
			return false;
		}
		t0 = num / den;
		if (nh < (clean ? 1.0f : fmaxf(3.0f, 0.3f * den))) {
			break;
		}
		t = 0.5f * t0;
	}
	/* Intervals of one to four periods. */
	for (int it = 0; it < 2; it++) {
		num = 0.0f;
		den = 0.0f;
		for (uint32_t i = 1; i < ntr; i++) {
			float d = ws->tr[i] - ws->tr[i - 1U], r = (float)iround(d / t0);

			if (r >= 1.0f && r <= 4.0f && fabsf(d / t0 - r) < 0.3f) {
				num += d;
				den += r;
			}
		}
		if (den < 3.0f) {
			return false;
		}
		t0 = num / den;
	}
	/* The phase's drift: coarse over short blocks (an error of a few percent), then fine. */
	if (!grid_phase(ws, t0, GRID_BLK1, &t, &a) || !grid_phase(ws, t, GRID_BLK2, &t, &a)) {
		return false;
	}
	/* Then least squares through all the transitions on that grid, the others left out. */
	for (int it = 0; it < 2; it++) {
		float mi = 0.0f, mt = 0.0f, sxx = 0.0f, sxy = 0.0f, nn = 0.0f;

		for (uint32_t i = 0; i < ntr; i++) {
			float k = (float)iround((ws->tr[i] - a) / t);

			if (fabsf(ws->tr[i] - a - k * t) < 0.3f * t) {
				mi += k;
				mt += ws->tr[i];
				nn += 1.0f;
			}
		}
		if (nn < 3.0f) {
			return false;
		}
		mi /= nn;
		mt /= nn;
		for (uint32_t i = 0; i < ntr; i++) {
			float k = (float)iround((ws->tr[i] - a) / t);

			if (fabsf(ws->tr[i] - a - k * t) < 0.3f * t) {
				sxx += (k - mi) * (k - mi);
				sxy += (k - mi) * (ws->tr[i] - mt);
			}
		}
		if (!(sxx > 0.0f)) {
			return false;
		}
		t = sxy / sxx;
		a = mt - t * mi;
		if (!(t >= 0.5f * tmin)) {
			return false;
		}
	}
	/*
	 * The largest period the transitions keep: twice it when nine in ten of them fall on
	 * every other symbol; then the smallest: half of it when that holds a tenth more of them
	 * (three at least; one for clean edges or when it holds all of fewer than 16).
	 */
	{
		uint32_t ev = 0U, od = 0U;

		for (uint32_t i = 0; i < ntr; i++) {
			int32_t k = iround((ws->tr[i] - a) / t);

			if (fabsf(ws->tr[i] - a - (float)k * t) < 0.25f * t) {
				if (k % 2 != 0) {
					od++;
				} else {
					ev++;
				}
			}
		}
		if (ev + od >= 12U && (10U * ev >= 9U * (ev + od) || 10U * od >= 9U * (ev + od))) {
			a += od > ev ? t : 0.0f;
			t *= 2.0f;
		}
	}
	on = grid_on(ws, t, a);
	if (t >= 2.0f * tmin) {
		uint32_t half = grid_on(ws, 0.5f * t, a);

		/* Of few transitions, one at a half period that all the others allow is enough. */
		if (half >= on + (clean ? 1U : (ntr >= 30U ? ntr / 10U : 3U)) ||
		    (ntr < 16U && half == ntr && on < ntr)) {
			t *= 0.5f;
			on = half;
		}
	}
	g->t = t;
	g->tau = a - (float)ifloor(a / t) * t;
	g->q = (float)on / (float)ntr;
	return true;
}

/* ---- slicing ---- */

static void put_bit(struct ctx *c, bool bit)
{
	uint32_t i = c->out->hdr.nbits;

	if (i >= c->cap) {
		return;
	}
	if (bit) {
		c->out->bits[i / 8U] |= (uint8_t)(0x80U >> (i % 8U));
	}
	c->out->hdr.nbits = i + 1U;
}

static void drop_bits(struct ctx *c)
{
	memset(c->out->bits, 0, sizeof(c->out->bits));
	c->out->hdr.nbits = 0U;
}

/*
 * The slicing window of symbol @p k on the grid: samples [*i0, *i1] within SLICE_SPAN of a symbol
 * about its centre (the nearest sample when none is), within [0, n). False when it falls
 * outside.
 */
static bool sym_win(const struct ctx *c, int32_t k, int32_t *i0, int32_t *i1)
{
	float mid = c->g.tau + ((float)k + 0.5f) * c->g.t, half = 0.5f * SLICE_SPAN * c->g.t;

	*i0 = iceil(mid - half);
	*i1 = ifloor(mid + half);
	if (*i1 < *i0) {
		*i0 = ifloor(mid + 0.5f);
		*i1 = *i0;
	}
	return *i0 >= 0 && *i1 < (int32_t)c->n;
}

/* The first symbol index whose window can lie within the stream. */
static int32_t sym_first(const struct ctx *c)
{
	return ifloor(-c->g.tau / c->g.t) - 1;
}

/*
 * Symbol @p k's mean frequency (binary angle a sample) in *f: the mean of the phase steps within
 * SLICE_SPAN of a symbol about its centre (step i is the frequency at i - 1/2; the nearest one
 * when none is). Returns 1, 0 when the window falls before the stream, -1 when past its end.
 */
static int sym_freq(const struct ctx *c, int32_t k, float *f)
{
	float mid = c->g.tau + ((float)k + 0.5f) * c->g.t, half = 0.5f * SLICE_SPAN * c->g.t;
	int32_t s0 = iceil(mid - half + 0.5f), s1 = ifloor(mid + half + 0.5f);
	int32_t sum = 0;

	if (s1 < s0) {
		s0 = ifloor(mid + 1.0f);
		s1 = s0;
	}
	if (s1 >= (int32_t)c->n) {
		return -1;
	}
	if (s0 < 1) {
		return 0;
	}
	for (int32_t i = s0; i <= s1; i++) {
		sum += c->step[i];
	}
	*f = (float)sum / (float)(s1 - s0 + 1);
	return 1;
}

/* Every symbol's mean frequency (sym_freq()) on the grid into c->sym, in order: their count. */
static uint32_t sym_all(const struct ctx *c)
{
	uint32_t ns = 0U;
	float f;
	int r;

	for (int32_t k = sym_first(c); ns < c->sym_max && (r = sym_freq(c, k, &f)) >= 0; k++) {
		if (r > 0) {
			c->sym[ns++] = f;
		}
	}
	return ns;
}

/*
 * FSK and MSK: the mean frequency of each of the @p syms symbols (c->sym, binary angle a sample)
 * against @p centre; @p levels 2 or 4 (4FSK: tones at +-dev and +-dev / 3, Gray coded as sign,
 * inner). Returns the share of symbols too near a threshold to tell.
 */
static float slice_fsk(struct ctx *c, uint32_t syms, unsigned int levels, float centre, float dev)
{
	uint32_t bad = 0U;

	for (uint32_t k = 0; k < syms; k++) {
		float f = c->sym[k] - centre;

		if (levels == 4U) {
			float a = fabsf(f);

			if (a < 0.2f * dev || fabsf(a - 2.0f / 3.0f * dev) < 0.13f * dev) {
				bad++;
			}
			put_bit(c, f > 0.0f);
			put_bit(c, a < 2.0f / 3.0f * dev);
		} else {
			if (fabsf(f) < 0.3f * dev) {
				bad++;
			}
			put_bit(c, f > 0.0f);
		}
	}
	return syms > 0U ? (float)bad / (float)syms : 1.0f;
}

/* OOK: each window's mean power against @p mid. Returns the share of symbols near it. */
static float slice_ook(struct ctx *c, float lo, float hi)
{
	float mid = 0.5f * (lo + hi), band = 0.2f * (hi - lo);
	uint32_t syms = 0U, bad = 0U;

	for (int32_t k = sym_first(c);; k++) {
		int32_t i0, i1;
		float p = 0.0f;

		if (!sym_win(c, k, &i0, &i1)) {
			if (i1 >= (int32_t)c->n) {
				break;
			}
			continue;
		}
		for (int32_t i = i0; i <= i1; i++) {
			p += (float)pw(c->iq, (uint32_t)i);
		}
		p /= (float)(i1 - i0 + 1);
		syms++;
		if (fabsf(p - mid) < band) {
			bad++;
		}
		put_bit(c, p > mid);
	}
	return syms > 0U ? (float)bad / (float)syms : 1.0f;
}

/*
 * BPSK and QPSK, differentially: each window's sum against the previous one's, less the drift
 * (radians a sample) over a symbol; @p order 2 (DBPSK: 1 for a phase step near pi) or 4 (DQPSK:
 * steps of 0, pi / 2, pi, -pi / 2 as 00, 01, 11, 10). Returns the share of steps off the
 * constellation by more than a quarter of its spacing, or of windows whose sum is weak (below
 * 0.6 of the mean magnitude: a window across a phase step, the grid off), the larger.
 */
static float slice_psk(struct ctx *c, unsigned int order, float drift)
{
	const float spacing = ESP_SDR_TAU_F / (float)order;
	float pr = 0.0f, pq = 0.0f, cr = cosf(drift * c->g.t), ci = -sinf(drift * c->g.t);
	float mag = 0.0f, mag2;
	uint32_t syms = 0U, bad = 0U, wins = 0U, weak = 0U;
	bool have = false;
	int32_t k0 = sym_first(c), i0, i1;

	/* The windows' mean magnitude first. */
	for (int32_t k = k0;; k++) {
		float zr = 0.0f, zq = 0.0f;

		if (!sym_win(c, k, &i0, &i1)) {
			if (i1 >= (int32_t)c->n) {
				break;
			}
			continue;
		}
		for (int32_t i = i0; i <= i1; i++) {
			zr += (float)c->iq[2 * i];
			zq += (float)c->iq[2 * i + 1];
		}
		mag += sqrtf(zr * zr + zq * zq) / (float)(i1 - i0 + 1);
		wins++;
	}
	if (wins == 0U) {
		return 1.0f;
	}
	mag = 0.6f * mag / (float)wins;
	mag2 = mag * mag;

	for (int32_t k = k0;; k++) {
		float zr = 0.0f, zq = 0.0f, l;

		if (!sym_win(c, k, &i0, &i1)) {
			if (i1 >= (int32_t)c->n) {
				break;
			}
			continue;
		}
		for (int32_t i = i0; i <= i1; i++) {
			zr += (float)c->iq[2 * i];
			zq += (float)c->iq[2 * i + 1];
		}
		l = (float)(i1 - i0 + 1);
		if (zr * zr + zq * zq < mag2 * l * l) {
			weak++;
		}
		if (have) {
			/* z conj(previous), turned back by the drift over a symbol. */
			float tr = zr * pr + zq * pq, tq = zq * pr - zr * pq;
			float dr = tr * cr - tq * ci, dq = tr * ci + tq * cr;
			float ang = atan2f(dq, dr);
			int qi = (int)iround(ang / spacing);
			float q = (float)qi;

			syms++;
			if (fabsf(ang - q * spacing) > 0.25f * spacing) {
				bad++;
			}
			if (order == 2U) {
				put_bit(c, qi != 0);
			} else {
				qi = qi == -2 ? 2 : qi;
				put_bit(c, qi == 2 || qi == -1);
				put_bit(c, qi == 2 || qi == 1);
			}
		}
		pr = zr;
		pq = zq;
		have = true;
	}
	if (syms == 0U) {
		return 1.0f;
	}
	return fmaxf((float)bad / (float)syms, (float)weak / (float)wins);
}

/*
 * The frequency's shape where the symbol changes: its mean over the quarter symbols next to the
 * boundary (an eighth to three eighths of a symbol from it, both sides) against its mean over
 * the quarter symbols about the two symbols' centres, signed by the symbols. Near 1 for
 * rectangular frequency pulses (MSK), about 0.7 for Gaussian ones of BT 0.5 (BLE's GFSK).
 */
static float sharpness(const struct ctx *c, float centre)
{
	float t = c->g.t, edge = 0.0f, mid = 0.0f, f;
	int prev = 0, r;

	for (int32_t k = sym_first(c); (r = sym_freq(c, k, &f)) >= 0; k++) {
		float b = c->g.tau + (float)k * t;
		int sym = f > centre ? 1 : -1;

		if (r == 0) {
			prev = 0;
			continue;
		}
		if (prev != 0 && sym != prev && b - 0.625f * t >= 1.0f &&
		    b + 0.625f * t < (float)c->n - 1.0f) {
			for (int32_t i = iceil(b - 0.625f * t + 0.5f);
			     (float)i - 0.5f < b + 0.625f * t; i++) {
				/* step[i] is the frequency at i - 0.5. */
				float u = ((float)i - 0.5f - b) / t, v = (float)c->step[i] - centre;

				if (u >= -0.375f && u < -0.125f) {
					edge += (float)prev * v;
				} else if (u >= 0.125f && u < 0.375f) {
					edge += (float)sym * v;
				} else if (u >= -0.625f && u < -0.375f) {
					mid += (float)prev * v;
				} else if (u >= 0.375f && u < 0.625f) {
					mid += (float)sym * v;
				}
			}
		}
		prev = sym;
	}
	return mid > 0.0f ? edge / mid : 0.0f;
}

/* ---- classification ---- */

/* Transitions on the symbol grid at least (more of few: grid_ok()), symbols too near a
 * threshold at most. */
#define GRID_Q     0.7f
#define EYE_BAD    0.2f
/*
 * Signal to noise ratio (power) of the smoothed envelope from which an OOK burst's edges count
 * as clean: no stray transitions, and the midpoint crossings of a keyed carrier fall on the
 * symbol grid (an FSK's or PSK's sign changes between unequal levels need not).
 */
#define GRID_CLEAN 30.0f
/* Symbols a stream holds at least for a symbol rate and bits. */
#define GRID_SYMS  16.0f
/* FSK: neighbouring tones' separation over the sum of their spreads at least, plus EYE_FEW over
 * the root of the smaller tone's symbols. */
#define EYE_SEP    1.6f
#define EYE_FEW    2.5f
/* OOK: the levels' separation, the high level over the low one at least. */
#define OOK_SEP    0.8f
#define OOK_RATIO  5.0f
/* CW: share of the signal's power about the strongest bin, bandwidth in bins at most. */
#define CW_CONC    0.85f
#define CW_BINS    6.0f
/* WIDEBAND: the power's squared coefficient of variation at least (Gaussian noise: 1). */
#define WIDE_CV2   0.8f
/* The squared (fourth power) samples' line share for BPSK (QPSK); their spectra's segments. */
#define LINE_BPSK  0.35f
#define LINE_QPSK  0.3f
#define LINE_SEGS  4U
/*
 * MSK: the modulation index from the symbols' mean frequencies (flat over an MSK symbol; a
 * Gaussian pulse's intersymbol interference lowers it, BLE's GFSK measures 0.41 to 0.46) and
 * the sharpness of the frequency steps (MSK 0.88 to 1.08, BLE's GFSK 0.53 to 0.90 at 10 dB).
 */
#define MSK_H_LO   0.475f
#define MSK_H_HI   0.56f
#define MSK_SHARP  0.8f

/*
 * The envelope's smoothing, samples: what brings the power's noise within a tenth of the signal
 * (none at a high signal to noise ratio), up to a third of the shortest symbol the bandwidth
 * allows.
 */
static uint32_t env_win(const struct ctx *c)
{
	float v = c->m.bw > 0.0f ? c->rate / (3.0f * c->m.bw) : 1.0f;
	float need = c->m.sig > 0.0f ? 200.0f * c->m.noise / c->m.sig : v;

	v = need < v ? need : v;
	return v < 1.0f ? 1U : (v > (float)LAG_MAX ? LAG_MAX : (uint32_t)v);
}

/* The share of symbols too near a threshold tolerated: less of a short burst. */
static float eye_bad(const struct ctx *c)
{
	return (float)c->n < 50.0f * c->g.t ? 0.5f * EYE_BAD : EYE_BAD;
}

/*
 * A grid's quality suffices: GRID_Q of its transitions (more of fewer: 0.85 of fewer than 64, all
 * but one of fewer than 16), and GRID_SYMS symbols in the stream at least.
 */
static bool grid_ok(const struct ctx *c)
{
	uint32_t ntr = c->ws->ntr;
	float q = c->g.q;

	return (ntr >= 64U ? q >= GRID_Q
			   : (ntr >= 16U ? q >= 0.85f : q * (float)ntr >= (float)ntr - 1.5f)) &&
	       (float)c->n >= GRID_SYMS * c->g.t;
}

/* The frequency's lag: about a third of the shortest symbol the bandwidth allows. */
static uint32_t freq_lag(const struct ctx *c)
{
	float v = c->m.bw > 0.0f ? c->rate / (3.0f * c->m.bw) + 0.5f : 1.0f;

	return v < 1.0f ? 1U : (v > (float)LAG_MAX ? LAG_MAX : (uint32_t)v);
}

static bool try_ook(struct ctx *c, const struct env *ev, uint32_t ms)
{
	float mid = 0.5f * (ev->lo + ev->hi), head = 0.0f, tail = 0.0f;
	uint32_t edge = ms + 2U < c->n / 4U ? ms + 2U : c->n / 4U, inner;
	bool clean;

	tr_env(c->ws, c->iq, c->n, ms, mid, 0.2f * (ev->hi - ev->lo));
	/* A gap within: transitions besides the burst's own edges (noise left at its ends). */
	for (uint32_t k = 0; k < edge; k++) {
		head += (float)pw(c->iq, k);
		tail += (float)pw(c->iq, c->n - 1U - k);
	}
	inner = c->ws->ntr;
	inner -= inner > 0U && head < mid * (float)edge ? 1U : 0U;
	inner -= inner > 0U && tail < mid * (float)edge ? 1U : 0U;
	if (inner < 2U) {
		return false;
	}
	c->mod = PCAP_MOD_OOK;
	c->centre_hz = c->m.peak;
	clean = c->m.sig * (float)ms >= GRID_CLEAN * c->m.noise;
	if (!grid_fit(c->ws, (float)ms, clean, &c->g) || !grid_ok(c)) {
		return true;
	}
	c->have_grid = true;
	if (slice_ook(c, ev->lo, ev->hi) > eye_bad(c)) {
		drop_bits(c);
		c->have_grid = false;
	}
	return true;
}

/*
 * FSK: the transitions are the frequency's sign changes (about the spectrum's centre, else the
 * mean frequency @p mean) over the lag @p w; @p centre and @p dev, binary angles over w about the
 * spectrum's centre, are the tones' first guess (the deviation sets the hysteresis). On the
 * transitions' grid the histogram of the symbols' mean frequencies tells the tones, two or four,
 * the eye open between them. UNKNOWN without a grid or an open eye.
 */
static void try_fsk(struct ctx *c, float centre, float dev, float mean, uint32_t w)
{
	float h[FH_N], span, lo, hi, rs, hh, tone[PK_MAX], tsum[PK_MAX], tsq[PK_MAX];
	float tcnt[PK_MAX];
	uint32_t wt = w < 2U ? 2U : w, ns;
	struct peak pk[PK_MAX];
	unsigned int np;

	/*
	 * Transitions about the spectrum's centre, over a lag of two samples at least (at two
	 * samples a symbol a lone symbol keeps half its swing); about the mean frequency
	 * (@p mean, binary angle over w) when they make no grid.
	 */
	for (int at = 0; at < 2; at++) {
		float ref = at == 0 ? 0.0f : mean / (float)w;

		tr_freq(c->ws, c->step, c->n, wt, ref * (float)wt,
			0.3f * dev * (float)wt / (float)w);
		c->have_grid = grid_fit(c->ws, 0.75f * (float)wt, false, &c->g) && grid_ok(c);
		if (c->have_grid) {
			break;
		}
	}
	/* Without a symbol grid the tones are not told apart (a 4FSK's merge in pairs). */
	c->mod = PCAP_MOD_UNKNOWN;
	if (!c->have_grid) {
		return;
	}
	/* Per sample from here on: the symbols' histogram over +-2.5 times their RMS about the
	 * centre (four tones +-1.9 deviations). */
	centre /= (float)w;
	ns = sym_all(c);
	span = 0.0f;
	for (uint32_t k = 0; k < ns; k++) {
		span += (c->sym[k] - centre) * (c->sym[k] - centre);
	}
	if (!(span > 0.0f)) {
		c->have_grid = false;
		return;
	}
	span = 2.5f * sqrtf(span / (float)ns);
	memset(h, 0, sizeof(h));
	for (uint32_t k = 0; k < ns; k++) {
		float x = ((c->sym[k] - centre) / span + 1.0f) * 0.5f * (float)FH_N;

		x = x < 0.0f ? 0.0f : (x > (float)FH_N - 1.0f ? (float)FH_N - 1.0f : x);
		h[(uint32_t)x] += 1.0f;
	}
	/* A few hundred symbols: smoothed more than the samples' histogram. */
	np = fpeaks(h, 3U, pk);
	if (np != 2U && np != 4U) {
		c->have_grid = false;
		return;
	}
	/* Each tone: the mean of the symbols nearer to its peak than to the others. */
	for (unsigned int j = 0; j < np; j++) {
		tone[j] = centre + (pk[j].pos / (float)FH_N * 2.0f - 1.0f) * span;
		tsum[j] = 0.0f;
		tsq[j] = 0.0f;
		tcnt[j] = 0.0f;
	}
	for (uint32_t k = 0; k < ns; k++) {
		float f = c->sym[k];
		unsigned int j = 0U;

		while (j + 1U < np && f > 0.5f * (tone[j] + tone[j + 1U])) {
			j++;
		}
		tsum[j] += f;
		tsq[j] += f * f;
		tcnt[j] += 1.0f;
	}
	for (unsigned int j = 0; j < np; j++) {
		/* Three symbols a tone at least. */
		if (tcnt[j] < 3.0f) {
			c->have_grid = false;
			return;
		}
		tone[j] = tsum[j] / tcnt[j];
		tsq[j] = sqrtf(fmaxf(tsq[j] / tcnt[j] - tone[j] * tone[j], 0.0f));
	}
	/*
	 * The eye open: neighbouring tones apart by EYE_SEP times their spreads (a unimodal spread
	 * cut in two is apart by 1.3), more for few symbols (the spreads' own uncertainty).
	 */
	for (unsigned int j = 0; j + 1U < np; j++) {
		float few = fminf(tcnt[j], tcnt[j + 1U]);

		if (tone[j + 1U] - tone[j] <
		    (EYE_SEP + EYE_FEW / sqrtf(few)) * (tsq[j] + tsq[j + 1U])) {
			c->have_grid = false;
			return;
		}
	}
	if (np == 4U) {
		float d1 = tone[1] - tone[0], d2 = tone[2] - tone[1], d3 = tone[3] - tone[2];
		float dm = (d1 + d2 + d3) / 3.0f;

		if (fabsf(d1 - dm) > 0.4f * dm || fabsf(d2 - dm) > 0.4f * dm ||
		    fabsf(d3 - dm) > 0.4f * dm) {
			c->have_grid = false;
			return;
		}
	}
	lo = tone[0];
	hi = tone[np - 1U];
	centre = 0.5f * (lo + hi);
	dev = 0.5f * (hi - lo);
	c->centre_hz = c->m.centre + centre * c->rate / 65536.0f;
	c->dev_hz = dev * c->rate / 65536.0f;
	c->mod = np == 4U ? PCAP_MOD_4FSK : PCAP_MOD_2FSK;
	rs = c->rate / c->g.t;
	hh = 2.0f * c->dev_hz / rs;
	if (np == 2U && hh >= MSK_H_LO && hh <= MSK_H_HI && c->g.t >= 3.0f &&
	    sharpness(c, centre) >= MSK_SHARP) {
		c->mod = PCAP_MOD_MSK;
	}
	if (slice_fsk(c, ns, np, centre, dev) > eye_bad(c)) {
		drop_bits(c);
		c->have_grid = false;
		c->mod = PCAP_MOD_UNKNOWN;
	}
}

static bool try_psk(struct ctx *c, uint32_t w0)
{
	unsigned int order = 2U;
	uint32_t segs, l, wp;
	float f, q, carrier;

	l = spectrum(c->ws, c->iq, c->n, 1, LINE_SEGS, &segs);
	if (l == 0U) {
		return false;
	}
	q = line(c->ws, l, c->rate, &f);
	carrier = 0.5f * f;
	if (q < LINE_BPSK) {
		l = spectrum(c->ws, c->iq, c->n, 2, LINE_SEGS, &segs);
		q = line(c->ws, l, c->rate, &f);
		if (q < LINE_QPSK) {
			return false;
		}
		order = 4U;
		carrier = 0.25f * f;
	}
	/* The carrier the line gives lies at the spectrum's centre (MSK's lines do not). */
	if (fabsf(carrier - c->m.centre) > 0.2f * c->m.bw + c->m.bin) {
		return false;
	}
	/* Without a symbol grid a line alone does not tell PSK: UNKNOWN. */
	c->centre_hz = carrier;
	wp = w0 / 2U < 2U ? 2U : w0 / 2U;
	tr_phase(c->ws, c->step, c->n, wp, 65536.0f * (carrier - c->m.centre) / c->rate,
		 order == 2U ? 16384 : 8192);
	/* Three samples a symbol at least: the steps' detector does not resolve fewer. */
	if (!grid_fit(c->ws, (float)wp, false, &c->g) || !grid_ok(c) || c->g.t < 3.0f) {
		return true;
	}
	c->mod = order == 2U ? PCAP_MOD_BPSK : PCAP_MOD_QPSK;
	c->have_grid = true;
	if (slice_psk(c, order, ESP_SDR_TAU_F * carrier / c->rate) > eye_bad(c)) {
		drop_bits(c);
		c->have_grid = false;
		c->mod = PCAP_MOD_UNKNOWN;
	}
	return true;
}

static void classify(struct ctx *c)
{
	float h[FH_N], snr, pos[PK_MAX], sw = 0.0f, sm = 0.0f, sv = 0.0f;
	struct peak pk[PK_MAX];
	struct env ev;
	uint32_t ms, w;
	unsigned int np;
	bool keyed;

	c->mod = PCAP_MOD_UNKNOWN;
	c->centre_hz = c->m.centre;
	snr = c->m.sig > 0.0f ? c->m.sig / (c->m.noise * fmaxf(c->m.bw, 3.0f * c->m.bin) / c->rate)
			      : 0.0f;
	if (c->n < SPEC_MIN || snr < SNR_MIN) {
		/* Nothing clearly above the noise: no centre, no bandwidth. */
		c->centre_hz = 0.0f;
		c->m.bw = 0.0f;
		return;
	}
	ms = env_win(c);
	if (ms > c->n / 4U) {
		ms = c->n / 4U > 0U ? c->n / 4U : 1U;
	}
	/* The frequency about the spectrum's centre from here on (no lag wraps for an offset). */
	steps(c->iq, c->n, (int32_t)lrintf(65536.0f * c->m.centre / c->rate), ms, c->step, &ev);
	envelope(c->iq, c->n, ms, &ev);
	keyed = ev.sep >= OOK_SEP && ev.wlo >= 0.05f && ev.wlo <= 0.95f &&
		ev.hi >= OOK_RATIO * ev.lo;
	/* The low level near the noise. */
	if (keyed && ev.lo <= 3.0f * c->m.noise + 0.1f * ev.hi && try_ook(c, &ev, ms)) {
		return;
	}
	w = freq_lag(c);
	fhist(c->iq, c->step, c->n, w, h);
	np = fpeaks(h, 1U, pk);
	if (c->m.conc >= CW_CONC && c->m.bw <= CW_BINS * c->m.bin && !keyed && np <= 1U) {
		c->mod = PCAP_MOD_CW;
		c->centre_hz = c->m.peak;
		return;
	}
	/* Binary angles over the lag, about the spectrum's centre: the peaks; the histogram's mean
	 * and spread. */
	for (unsigned int i = 0; i < np; i++) {
		pos[i] = pk[i].pos * (float)(1U << FH_SHIFT) - 32768.0f;
	}
	for (unsigned int i = 0; i < FH_N; i++) {
		float d = ((float)i + 0.5f) * (float)(1U << FH_SHIFT) - 32768.0f;

		sw += h[i];
		sm += h[i] * d;
		sv += h[i] * d * d;
	}
	sm = sw > 0.0f ? sm / sw : 0.0f;
	/* Two to four tones placed about the centre. */
	if (np >= 2U && np <= 4U) {
		float mid = 0.5f * (pos[0] + pos[np - 1U]), half = 0.5f * (pos[np - 1U] - pos[0]);
		float wl = pk[0].wt, wh = pk[np - 1U].wt;

		if (fabsf(mid) <= 0.35f * half && fminf(wl, wh) >= 0.15f * fmaxf(wl, wh)) {
			try_fsk(c, mid, half, sm, w);
			return;
		}
	}
	/* One frequency at the centre: a PSK's carrier line (a keyed carrier has one too). */
	if (np == 1U && fabsf(pos[0]) < 8192.0f && !keyed && try_psk(c, w)) {
		return;
	}
	/* No carrier line: FSK whose tones the samples' histogram does not show apart (few
	 * samples a symbol, noise), the deviation from the frequency's spread. */
	if (!keyed) {
		if (sw > 0.0f && sv > 4.0f * (float)(1U << (2U * FH_SHIFT)) * sw) {
			try_fsk(c, 0.0f, sqrtf(sv / sw), sm, w);
			if (c->mod != PCAP_MOD_UNKNOWN) {
				return;
			}
		}
	}
	/* Noise-like: the power's spread that of Gaussian noise (a constant envelope at the
	 * least signal to noise ratio required has 0.75). */
	if (c->m.bw >= 0.5f * c->rate && c->m.sig >= c->m.noise && ev.sep < OOK_SEP &&
	    ev.cv2 > WIDE_CV2) {
		c->mod = PCAP_MOD_WIDEBAND;
	}
}

/* ---- the decoder ---- */

static uint16_t map_flags(uint16_t f)
{
	static const uint16_t map[][2] = {
		{BURST_F_TRUNCATED, PCAP_BF_TRUNCATED},
		{BURST_F_WIDEBAND, PCAP_BF_WIDEBAND},
		{BURST_F_TRANSIENT, PCAP_BF_TRANSIENT},
		{BURST_F_NEIGHBOUR, PCAP_BF_NEIGHBOUR},
		{BURST_F_DROPPED, PCAP_BF_DROPPED},
		{BURST_F_CLIPPED, PCAP_BF_CLIPPED},
		{BURST_F_CUT, PCAP_BF_CUT},
		{BURST_F_HOLE, PCAP_BF_HOLE},
	};
	uint16_t o = 0U;

	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		if ((f & map[i][0]) != 0U) {
			o |= map[i][1];
		}
	}
	return o;
}

size_t dec_generic_scratch(uint32_t n)
{
	return 8U + al8(sizeof(struct gws)) + al8(refine_scratch(n)) +
	       al8(4U * ((size_t)n / 2U + 1U)) + al8(2U * (size_t)n + 2U);
}

int dec_generic_run(const struct burst *b, const struct dec_generic_cfg *cfg, void *scratch,
		    size_t scratch_len, struct dec_generic_out *out)
{
	struct pcap_burst_hdr *h;
	struct burst cur;
	struct gws *ws;
	struct ctx c;
	uint8_t *p;
	int16_t *sbuf;
	void *rs;
	size_t rlen;
	uint32_t a, e, total = 1U, peak = 0U;
	uint64_t sum = 0U;
	bool refined = false;

	if (b == NULL || out == NULL) {
		return -EINVAL;
	}
	memset(out, 0, sizeof(*out));
	h = &out->hdr;
	h->freq_hz = b->freq_hz;
	h->flags = map_flags(b->flags);
	h->rx_gain = b->gain;
	h->channel = (uint8_t)b->chan;
	h->channels = (uint8_t)b->channels;
	h->floor_dbfs_x10 = dec_dbfs_x10(b->floor);
	h->sample_rate_hz = b->rate_hz;
	h->modulation = PCAP_MOD_UNKNOWN;
	if (b->n == 0U || b->iq == NULL || b->rate_hz == 0U) {
		/* Metadata only (a wideband record's). */
		if ((b->flags & BURST_F_WIDEBAND) != 0U) {
			h->modulation = PCAP_MOD_WIDEBAND;
		}
		h->power_dbfs_x10 = dec_dbfs_x10(b->power);
		h->peak_dbfs_x10 = h->power_dbfs_x10;
		if (b->rate_hz > 0U && b->loud_to >= b->loud_from) {
			h->duration_us = (uint32_t)((float)(b->loud_to - b->loud_from + 1U) * 1e6f /
							    (float)b->rate_hz +
						    0.5f);
		}
		out->start = b->loud_from;
		return 0;
	}
	if (scratch == NULL || scratch_len < dec_generic_scratch(b->n)) {
		return -ENOMEM;
	}
	p = align8(scratch);
	ws = (struct gws *)p;
	p += al8(sizeof(struct gws));
	rs = p;
	rlen = al8(refine_scratch(b->n));
	p += rlen;
	sbuf = (int16_t *)p;
	p += al8(4U * ((size_t)b->n / 2U + 1U));
	memset(&c, 0, sizeof(c));
	c.sym = ws->sym;
	c.sym_max = SYM_MAX;
	c.step = (int16_t *)p;
	c.ws = ws;
	c.out = out;
	c.cap = PCAP_BURST_BITS_MAX;
	if (cfg != NULL && cfg->max_bits > 0U && cfg->max_bits < PCAP_BURST_BITS_MAX) {
		c.cap = cfg->max_bits;
	}

	/* The loud part (loud_to is its last sample), trimmed to the envelope. */
	a = b->loud_from < b->n ? b->loud_from : b->n - 1U;
	e = (b->loud_to < b->n ? b->loud_to : b->n - 1U) + 1U;
	if (e <= a) {
		a = 0U;
		e = b->n;
	}
	trim(b->iq, b->floor, &a, &e);
	for (uint32_t k = a; k < e; k++) {
		uint32_t v = pw(b->iq, k);

		sum += v;
		peak = v > peak ? v : peak;
	}
	out->start = a;
	h->duration_us = (uint32_t)((float)(e - a) * 1e6f / (float)b->rate_hz + 0.5f);
	h->power_dbfs_x10 = dec_dbfs_x10((float)sum / (float)(e - a));
	h->peak_dbfs_x10 = dec_dbfs_x10((float)peak);

	/* Measure, refine while the signal is narrow enough, measure again. */
	twiddles(ws);
	cur = *b;
	cur.iq = b->iq + 2U * a;
	cur.n = e - a;
	if (cfg != NULL && cfg->max_samples > 0U && cur.n > cfg->max_samples) {
		cur.n = cfg->max_samples;
	}
	cur.loud_from = 0U;
	cur.loud_to = cur.n - 1U;
	for (;;) {
		unsigned int d;
		float snr, f0, off;

		if (!measure(ws, cur.iq, cur.n, (float)cur.rate_hz, cur.floor, refined, &c.m) ||
		    !(c.m.sig > 0.0f) || !(c.m.bw > 0.0f)) {
			break;
		}
		snr = c.m.sig / (c.m.noise * fmaxf(c.m.bw, 3.0f * c.m.bin) / (float)cur.rate_hz);
		if (snr < SNR_MIN) {
			break;
		}
		d = refine_decim(cur.rate_hz, (uint32_t)c.m.bw);
		while (d > 1U && (total * d > REFINE_DECIM_MAX || cur.n / d < REFINE_KEEP)) {
			d /= 2U;
		}
		f0 = cur.floor;
		off = (float)lrintf(c.m.centre);
		if (d < 2U || refine_run(&cur, (int32_t)off, d, sbuf, rs, rlen, &cur) != 0) {
			break;
		}
		total *= d;
		refined = true;
		/*
		 * No further refinement can follow and the bins coarsen little: the measurement
		 * carried over to the refined stream rather than repeated (the spectra are a short
		 * burst's main cost): the signal passes, the noise as the floor does.
		 */
		if (d <= 4U && (2U * total > REFINE_DECIM_MAX || cur.n / 2U < REFINE_KEEP)) {
			c.m.centre -= off;
			c.m.peak -= off;
			c.m.noise *= f0 > 0.0f ? cur.floor / f0 : 1.0f / (float)d;
			break;
		}
	}

	c.iq = cur.iq;
	c.n = cur.n;
	c.rate = (float)cur.rate_hz;
	classify(&c);

	h->modulation = c.mod;
	h->sample_rate_hz = cur.rate_hz;
	h->bandwidth_hz = c.m.bw > 0.0f ? (uint32_t)(c.m.bw + 0.5f) : 0U;
	h->offset_hz = (int32_t)((int64_t)cur.freq_hz - (int64_t)b->freq_hz) +
		       (int32_t)lrintf(c.centre_hz);
	if (c.have_grid && c.g.t > 0.0f) {
		h->symbol_rate = (uint32_t)(c.rate / c.g.t + 0.5f);
	}
	if (c.mod == PCAP_MOD_2FSK || c.mod == PCAP_MOD_4FSK || c.mod == PCAP_MOD_MSK) {
		h->deviation_hz = (uint32_t)(c.dev_hz + 0.5f);
	}
	if (c.mod == PCAP_MOD_UNKNOWN || c.mod == PCAP_MOD_WIDEBAND || c.mod == PCAP_MOD_CW) {
		drop_bits(&c);
		h->symbol_rate = 0U;
	}
	if (h->nbits > 0U) {
		h->flags |= PCAP_BF_BITS;
	}
	return 0;
}
