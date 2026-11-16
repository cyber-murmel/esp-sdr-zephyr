/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Signal processing of the channelized burst capture (esp_sdr_chancap.h), plain C on integers so
 * that the same code runs in the unit test on the host and is the reference the ESP32-S3 vector
 * kernels (esp_sdr_chancap_pie.S) must match bit for bit.
 *
 * Input: planar int16 I/Q at the ring rate, raw 10-bit words times 64 (the ring's unpack scale).
 * Two engines split the band into channels:
 *
 * - The polyphase filter bank (PFB): M channels spaced fs / M apart, one prototype low pass of
 *   K * M taps, outputs at fs / D with D = M (critically sampled) or M / 2. Odd stacking shifts
 *   every channel by half a spacing, so the LO (the DC spur) sits on a channel edge. Frame j
 *   takes the newest input pair n = j * D + M - 1; the branch sums use lane i = M - 1 - rho for
 *   branch rho, so that a lane reads the input in natural order; the DFT twiddles absorb that.
 * - The DDC bank: one mixer and decimating FIR per channel for grids that do not divide fs,
 *   outputs at fs / D_c.
 *
 * Integer model, the same as the vector unit's: products of int16 accumulate in a 40-bit
 * accumulator, the result is shifted right arithmetically and saturated to int16
 * (chancap_sat16()). Complex twiddles are Q14 (CHANCAP_TW_ONE).
 */

#ifndef ESP_SDR_CHANCAP_DSP_H_
#define ESP_SDR_CHANCAP_DSP_H_

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <esp_sdr/esp_sdr_dsp.h>

#define CHANCAP_M_MAX     16U /* channels of one PFB */
#define CHANCAP_K_MAX     16U /* taps per branch */
#define CHANCAP_L_MAX     (CHANCAP_M_MAX * CHANCAP_K_MAX)
#define CHANCAP_TW_SHIFT  14
#define CHANCAP_TW_ONE    (1 << CHANCAP_TW_SHIFT)
/* Prototype taps: DC gain CHANCAP_H_ONE, so the filter shift returns the input scale. */
#define CHANCAP_H_SHIFT   14
#define CHANCAP_H_ONE     (1 << CHANCAP_H_SHIFT)
/* Input and output full scale: the ring's 10-bit words times 64. */
#define CHANCAP_FULL_SCALE 32768

static inline int16_t chancap_sat16(int64_t v)
{
	return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}

/* ---- prototype filter design (Kaiser window), float on the host and at start ---- */

static inline float chancap_bessel_i0(float x)
{
	float s = 1.0f, t = 1.0f;

	for (int k = 1; k < 40; k++) {
		t *= (x / (2.0f * (float)k)) * (x / (2.0f * (float)k));
		s += t;
		if (t < 1e-9f * s) {
			break;
		}
	}
	return s;
}

/*
 * Kaiser-windowed sinc low pass of @p l taps: cutoff @p fc (fraction of the input rate, 0.5 =
 * Nyquist), window parameter @p beta; the taps are scaled to a DC gain of CHANCAP_H_ONE as
 * int16 (rounded, the rounding error spread on the centre tap).
 */
static inline void chancap_design_lowpass(int16_t *h, unsigned int l, float fc, float beta)
{
	float w[CHANCAP_L_MAX], sum = 0.0f;
	float i0b = chancap_bessel_i0(beta), mid = ((float)l - 1.0f) / 2.0f;
	int32_t isum = 0;

	for (unsigned int n = 0; n < l; n++) {
		float t = (float)n - mid, r = 2.0f * t / ((float)l - 1.0f);
		float sinc = t == 0.0f ? 2.0f * fc : sinf(ESP_SDR_TAU_F * fc * t) /
								((ESP_SDR_TAU_F / 2.0f) * t);
		float win = chancap_bessel_i0(beta * sqrtf(1.0f - r * r)) / i0b;

		w[n] = sinc * win;
		sum += w[n];
	}
	for (unsigned int n = 0; n < l; n++) {
		h[n] = (int16_t)lroundf(w[n] / sum * (float)CHANCAP_H_ONE);
		isum += h[n];
	}
	/* The rounding remainder on the centre tap(s), keeping the taps symmetric within 1. */
	if ((l & 1U) != 0U) {
		h[l / 2] = (int16_t)(h[l / 2] + (CHANCAP_H_ONE - isum));
	} else {
		int32_t e = CHANCAP_H_ONE - isum, half = e / 2;

		h[l / 2 - 1] = (int16_t)(h[l / 2 - 1] + half);
		h[l / 2] = (int16_t)(h[l / 2] + (e - half));
	}
}

/* Kaiser beta for a stop band attenuation in dB (Kaiser's formula). */
static inline float chancap_kaiser_beta(float atten_db)
{
	if (atten_db > 50.0f) {
		return 0.1102f * (atten_db - 8.7f);
	}
	if (atten_db >= 21.0f) {
		return 0.5842f * powf(atten_db - 21.0f, 0.4f) + 0.07886f * (atten_db - 21.0f);
	}
	return 0.0f;
}

/* ---- polyphase filter bank ---- */

struct chancap_pfb {
	unsigned int m, d, k, l;
	/* Odd stacking: channel rho is centred at (rho + 1/2) fs / m instead of rho fs / m. */
	bool odd;
	/*
	 * Per-lane taps: hk[k][i] = h'[(m - 1 - i) + k m], h' = h with the sign of every other
	 * block of m taps flipped when odd (the half-spacing mix folded into the taps).
	 */
	int16_t hk[CHANCAP_K_MAX][CHANCAP_M_MAX];
	/* Branch twiddle exp(-j pi i / m) per lane i (odd stacking only), Q14. */
	int16_t br[CHANCAP_M_MAX], bi[CHANCAP_M_MAX];
	/* m = 8, odd: the branch twiddle merged into the DFT's stage 1 (chancap_dft8_odd()). */
	int16_t t8r[8], t8i[8];
	/*
	 * DFT: channel c (c = 0 .. m - 1, centre c - m / 2 spacings from the LO, plus a half if odd)
	 * sums lane i times exp(-j 2 pi mc i / m), mc = c - m / 2; wr/wi hold those twiddles, Q14.
	 */
	int16_t wr[CHANCAP_M_MAX][CHANCAP_M_MAX], wi[CHANCAP_M_MAX][CHANCAP_M_MAX];
	/*
	 * Output rotation per frame phase: frame j multiplies channel c by
	 * exp(-j 2 pi (mc + odd / 2) j d / m), periodic in j with period rot_n (at most 4).
	 */
	unsigned int rot_n;
	int16_t rr[4][CHANCAP_M_MAX], ri[4][CHANCAP_M_MAX];
	/* Frames produced; the next frame ends at input pair frames * d + m - 1. */
	uint64_t frames;
};

/*
 * Set up a bank of @p m channels, decimation @p d (m or m / 2), @p k taps per branch, the
 * prototype's cutoff @p fc (fraction of the input rate) and stop band @p atten_db. Returns
 * false for an unsupported geometry.
 */
static inline bool chancap_pfb_init(struct chancap_pfb *p, unsigned int m, unsigned int d,
				    unsigned int k, bool odd, float fc, float atten_db)
{
	int16_t h[CHANCAP_L_MAX];

	if (m < 2U || m > CHANCAP_M_MAX || (m & (m - 1U)) != 0U || k == 0U || k > CHANCAP_K_MAX ||
	    (d != m && d != m / 2U) || fc <= 0.0f || fc >= 0.5f) {
		return false;
	}
	memset(p, 0, sizeof(*p));
	p->m = m;
	p->d = d;
	p->k = k;
	p->l = k * m;
	p->odd = odd;
	chancap_design_lowpass(h, p->l, fc, chancap_kaiser_beta(atten_db));
	for (unsigned int kk = 0; kk < k; kk++) {
		for (unsigned int i = 0; i < m; i++) {
			int16_t t = h[(m - 1U - i) + kk * m];

			if (odd && (kk & 1U) != 0U) {
				t = (int16_t)-t;
			}
			/* m = 8 runs the staged DFT: its "c - 4" is (-1)^i on the lanes. */
			if (m == 8U && (i & 1U) != 0U) {
				t = (int16_t)-t;
			}
			p->hk[kk][i] = t;
		}
	}
	/*
	 * Mixing by minus the centre of channel c, (mc + odd / 2) fs / m, at input pair
	 * n - t with t = (m - 1 - i) + k m and n = j d + m - 1 gives exp(-j 2 pi (mc + odd / 2)
	 * (j d + i - k m) / m): a per-frame rotation, a per-lane twiddle, and (-1)^k for odd
	 * stacking (in the taps above).
	 */
	for (unsigned int i = 0; i < m; i++) {
		double a = odd ? -(ESP_SDR_TAU / 2.0) * (double)i / (double)m : 0.0;

		p->br[i] = (int16_t)lround(cos(a) * CHANCAP_TW_ONE);
		p->bi[i] = (int16_t)lround(sin(a) * CHANCAP_TW_ONE);
		for (unsigned int c = 0; c < m; c++) {
			int mc = (int)c - (int)(m / 2U);
			double w = -ESP_SDR_TAU * (double)mc * (double)i / (double)m;

			p->wr[c][i] = (int16_t)lround(cos(w) * CHANCAP_TW_ONE);
			p->wi[c][i] = (int16_t)lround(sin(w) * CHANCAP_TW_ONE);
		}
	}
	/* m = 8: b_i on lanes 0..3, b_i W^i = exp(-j 3 pi i / 8) on lanes 4..7. */
	for (unsigned int k = 0; k < 8; k++) {
		double a = k < 4U ? -(ESP_SDR_TAU / 2.0) * (double)k / 8.0
				  : -3.0 * (ESP_SDR_TAU / 2.0) * (double)(k - 4U) / 8.0;

		p->t8r[k] = (int16_t)lround(cos(a) * CHANCAP_TW_ONE);
		p->t8i[k] = (int16_t)lround(sin(a) * CHANCAP_TW_ONE);
	}
	/* 2 m / gcd(d (2 mc + odd), 2 m) frames per period: at most 2 for d = m, 4 for d = m / 2. */
	p->rot_n = d == m ? (odd ? 2U : 1U) : (odd ? 4U : 2U);
	for (unsigned int j = 0; j < p->rot_n; j++) {
		for (unsigned int c = 0; c < m; c++) {
			double mc2 = 2.0 * ((double)c - (double)(m / 2U)) + (odd ? 1.0 : 0.0);
			double w = -(ESP_SDR_TAU / 2.0) * mc2 * (double)j * (double)d / (double)m;

			p->rr[j][c] = (int16_t)lround(cos(w) * CHANCAP_TW_ONE);
			p->ri[j][c] = (int16_t)lround(sin(w) * CHANCAP_TW_ONE);
		}
	}
	return true;
}

/* Taps the frame needs before its newest pair: the window is [n - l + 1, n]. */
static inline unsigned int chancap_pfb_history(const struct chancap_pfb *p)
{
	return p->l - 1U;
}

/* ---- the 8-point DFT as the vector unit computes it ---- */

static inline int16_t chancap_add16(int32_t a, int32_t b)
{
	return chancap_sat16((int64_t)a + b);
}

/*
 * Y[c] = sum_i u[i] exp(-j 2 pi (c - 4) i / 8) as three radix-2 stages on 8 lanes, with the
 * (-1)^i of the "c - 4" folded into the taps by the caller (chancap_pfb_init() negates the
 * odd lanes' taps for m = 8). Stage 1 pairs lanes (i, i + 4): sums to lanes 0 to 3, differences
 * times W^i (Q14, one rounding) to lanes 4 to 7; stage 2 pairs (i, i + 2) within each half the
 * same way, with -j on lanes 5 and 7; stage 3 pairs (i, i + 1). The bins come out in natural
 * order. Saturating 16-bit adds throughout, as EE.VADDS.S16 does.
 */
/*
 * Stages 2 and 3 of the 8-point DFT on stage 1's output @p ti / @p tq (sums on lanes 0..3,
 * twiddled differences on 4..7); the bins to @p vi / @p vq in natural order.
 */
static inline void chancap_dft8_tail(const int16_t *ti, const int16_t *tq, int16_t *vi,
				     int16_t *vq)
{
	static const unsigned int a[4] = {0, 1, 4, 5};
	int16_t si[8], sq[8];

	/* Stage 2: pairs (0,2) (1,3) (4,6) (5,7); sums to lanes 0..3, differences to 4..7. */
	for (unsigned int k = 0; k < 4; k++) {
		unsigned int p = a[k], q = a[k] + 2U;

		si[k] = chancap_add16(ti[p], ti[q]);
		sq[k] = chancap_add16(tq[p], tq[q]);
		si[k + 4] = chancap_sat16((int32_t)ti[p] - ti[q]);
		sq[k + 4] = chancap_sat16((int32_t)tq[p] - tq[q]);
	}
	/* -j on lanes 5 and 7: (x + jy)(-j) = y - jx, the negation saturating as EE.VSUBS does. */
	for (unsigned int k = 5; k < 8; k += 2U) {
		int16_t x = si[k];

		si[k] = sq[k];
		sq[k] = chancap_sat16(-(int32_t)x);
	}
	/* Stage 3: pairs (0,1) (2,3) (4,5) (6,7). */
	for (unsigned int k = 0; k < 4; k++) {
		unsigned int p = 2U * k, q = 2U * k + 1U;

		vi[k] = chancap_add16(si[p], si[q]);
		vq[k] = chancap_add16(sq[p], sq[q]);
		vi[k + 4] = chancap_sat16((int32_t)si[p] - si[q]);
		vq[k + 4] = chancap_sat16((int32_t)sq[p] - sq[q]);
	}
}

static inline void chancap_dft8_staged(int16_t *vi, int16_t *vq)
{
	/* W^i = exp(-j 2 pi i / 8), i = 0..3, Q14. */
	static const int16_t wr[4] = {16384, 11585, 0, -11585};
	static const int16_t wi[4] = {0, -11585, -16384, -11585};
	int16_t ti[8], tq[8];

	/* Stage 1. */
	for (unsigned int i = 0; i < 4; i++) {
		int16_t di = chancap_sat16((int32_t)vi[i] - vi[i + 4]);
		int16_t dq = chancap_sat16((int32_t)vq[i] - vq[i + 4]);

		ti[i] = chancap_add16(vi[i], vi[i + 4]);
		tq[i] = chancap_add16(vq[i], vq[i + 4]);
		ti[i + 4] = chancap_sat16(((int64_t)di * wr[i] - (int64_t)dq * wi[i]) >>
					  CHANCAP_TW_SHIFT);
		tq[i + 4] = chancap_sat16(((int64_t)di * wi[i] + (int64_t)dq * wr[i]) >>
					  CHANCAP_TW_SHIFT);
	}
	chancap_dft8_tail(ti, tq, vi, vq);
}

/*
 * The frame's first half: the branch sums, lane i, tap kk reading
 * x[n - (m - 1 - i) - kk m] = window[(k - 1 - kk) m + i]. On the ESP32-S3 the ring's CPU computes
 * these (esp_sdr_chancap_sums_n()).
 */
static inline void chancap_pfb_sums(const struct chancap_pfb *p, const int16_t *xi,
				    const int16_t *xq, int16_t *ui, int16_t *uq)
{
	const unsigned int m = p->m, k = p->k;

	for (unsigned int i = 0; i < m; i++) {
		int64_t ai = 0, aq = 0;

		for (unsigned int kk = 0; kk < k; kk++) {
			unsigned int at = (k - 1U - kk) * m + i;

			ai += (int64_t)p->hk[kk][i] * xi[at];
			aq += (int64_t)p->hk[kk][i] * xq[at];
		}
		ui[i] = chancap_sat16(ai >> CHANCAP_H_SHIFT);
		uq[i] = chancap_sat16(aq >> CHANCAP_H_SHIFT);
	}
}

/*
 * Stage 1 of the 8-point DFT with the odd-stacking twiddle b_i = exp(-j pi i / 8) merged in:
 * b_{i+4} = -j b_i, so t_i = b_i (u_i - j u_{i+4}) and t_{i+4} = b_i W^i (u_i + j u_{i+4}): the
 * saturating combinations first, then one Q14 multiply of every lane by t8 (b_i on lanes 0..3,
 * b_i W^i on lanes 4..7), then stages 2 and 3 (chancap_dft8_tail()).
 */
static inline void chancap_dft8_odd(int16_t *vi, int16_t *vq, const int16_t *t8r,
				    const int16_t *t8i)
{
	int16_t ti[8], tq[8];

	for (unsigned int i = 0; i < 4; i++) {
		/* u_i - j u_{i+4} = (I_i + Q_{i+4}) + j (Q_i - I_{i+4}); u_i + j u_{i+4} the other way. */
		ti[i] = chancap_add16(vi[i], vq[i + 4]);
		tq[i] = chancap_sat16((int32_t)vq[i] - vi[i + 4]);
		ti[i + 4] = chancap_sat16((int32_t)vi[i] - vq[i + 4]);
		tq[i + 4] = chancap_add16(vq[i], vi[i + 4]);
	}
	for (unsigned int k = 0; k < 8; k++) {
		int32_t a = ti[k], b = tq[k];

		ti[k] = chancap_sat16((a * t8r[k] - b * t8i[k]) >> CHANCAP_TW_SHIFT);
		tq[k] = chancap_sat16((a * t8i[k] + b * t8r[k]) >> CHANCAP_TW_SHIFT);
	}
	chancap_dft8_tail(ti, tq, vi, vq);
}

/*
 * The frame's second half: the branch twiddle (odd stacking), the DFT, the per-frame rotation.
 * On the ESP32-S3 the channelizer's CPU computes these for 8 channels at decimation 8
 * (esp_sdr_chancap_post_n()).
 */
static inline void chancap_pfb_post(struct chancap_pfb *p, int16_t *ui, int16_t *uq, int16_t *oi,
				    int16_t *oq)
{
	const unsigned int m = p->m;
	/* rot_n is a power of two: no 64-bit division per frame. */
	unsigned int ph = (unsigned int)p->frames & (p->rot_n - 1U);

	if (m == 8U && p->odd) {
		chancap_dft8_odd(ui, uq, p->t8r, p->t8i);
	} else if (p->odd) {
		for (unsigned int i = 0; i < m; i++) {
			int32_t a = ui[i], b = uq[i];

			ui[i] = chancap_sat16(((int64_t)a * p->br[i] - (int64_t)b * p->bi[i]) >>
					      CHANCAP_TW_SHIFT);
			uq[i] = chancap_sat16(((int64_t)a * p->bi[i] + (int64_t)b * p->br[i]) >>
					      CHANCAP_TW_SHIFT);
		}
	}
	/* DFT; the prototype's unity DC gain already holds the 1 / m of the branches. */
	if (m == 8U && !p->odd) {
		chancap_dft8_staged(ui, uq);
	}
	for (unsigned int c = 0; c < m; c++) {
		int64_t yi = 0, yq = 0;
		int32_t a, b;

		if (m == 8U) {
			a = ui[c];
			b = uq[c];
		} else {
			for (unsigned int i = 0; i < m; i++) {
				yi += (int64_t)p->wr[c][i] * ui[i] - (int64_t)p->wi[c][i] * uq[i];
				yq += (int64_t)p->wi[c][i] * ui[i] + (int64_t)p->wr[c][i] * uq[i];
			}
			a = chancap_sat16(yi >> CHANCAP_TW_SHIFT);
			b = chancap_sat16(yq >> CHANCAP_TW_SHIFT);
		}
		if (p->rot_n > 1U) {
			oi[c] = chancap_sat16(((int64_t)a * p->rr[ph][c] - (int64_t)b * p->ri[ph][c]) >>
					      CHANCAP_TW_SHIFT);
			oq[c] = chancap_sat16(((int64_t)a * p->ri[ph][c] + (int64_t)b * p->rr[ph][c]) >>
					      CHANCAP_TW_SHIFT);
		} else {
			oi[c] = (int16_t)a;
			oq[c] = (int16_t)b;
		}
	}
	p->frames++;
}

/*
 * One frame: @p xi / @p xq point at the frame's window, the input pair n - l + 1 first
 * (n = frames * d + m - 1 is the newest pair). Writes the m channel samples to @p oi / @p oq
 * in channel order (channel c centred (c - m / 2 [+ 1/2]) spacings above the LO).
 */
static inline void chancap_pfb_frame(struct chancap_pfb *p, const int16_t *xi, const int16_t *xq,
				     int16_t *oi, int16_t *oq)
{
	int16_t ui[CHANCAP_M_MAX], uq[CHANCAP_M_MAX];

	chancap_pfb_sums(p, xi, xq, ui, uq);
	chancap_pfb_post(p, ui, uq, oi, oq);
}

/*
 * Frame-major runs: a frame is 16 int16, the 8 lanes' I then their Q. The power of 8 frames
 * per lane, sum of I^2 + Q^2 divided by 16 (pack it with chancap_run8_pack()).
 */
static inline void chancap_power8(const int16_t fr[8][16], uint32_t p[8])
{
	for (unsigned int c = 0; c < 8; c++) {
		uint64_t s = 0;

		for (unsigned int f = 0; f < 8; f++) {
			int32_t i = fr[f][c], q = fr[f][8U + c];

			s += (uint64_t)(i * i) + (uint64_t)(q * q);
		}
		p[c] = (uint32_t)(s >> 4);
	}
}

/* ---- a run: 8 frames of the bank to the channels ---- */

/*
 * 8 frames of 8 lanes (lane = channel), transposed: lane c's 8 samples, I/Q interleaved, to
 * dst[c] (16-byte aligned, 32 bytes), and its power p[c] = sum over the 8 samples of
 * I^2 + Q^2, divided by 16 (so two runs add up to the mean over 16 samples).
 */
static inline void chancap_run8(const int16_t fi[8][8], const int16_t fq[8][8],
				int16_t *const dst[8], uint32_t p[8])
{
	for (unsigned int c = 0; c < 8; c++) {
		uint64_t s = 0;

		for (unsigned int f = 0; f < 8; f++) {
			int32_t i = fi[f][c], q = fq[f][c];

			dst[c][2U * f] = (int16_t)i;
			dst[c][2U * f + 1U] = (int16_t)q;
			s += (uint64_t)(i * i) + (uint64_t)(q * q);
		}
		p[c] = (uint32_t)(s >> 4);
	}
}

/*
 * The vector unit extracts a run's power as two saturated 16-bit values, sum >> 9 and
 * sum >> 19 (about 3 % resolution from the switch point up, full scale at 2^34); this packs
 * chancap_run8()'s power (p = sum >> 4) the same way, so both paths agree bit for bit.
 */
/*
 * The power of @p n frames per lane as the vector unit's esp_sdr_chancap_power() delivers it:
 * the sum of I^2 + Q^2 over the frames as fine = sat16(sum >> sf) and
 * coarse = sat16(sum >> (sf + sc)).
 */
static inline void chancap_power_n(const int16_t (*fr)[16], unsigned int n, unsigned int sf,
				   unsigned int sc, int16_t fine[8], int16_t coarse[8])
{
	for (unsigned int c = 0; c < 8; c++) {
		uint64_t s = 0;

		for (unsigned int f = 0; f < n; f++) {
			int32_t i = fr[f][c], q = fr[f][8U + c];

			s += (uint64_t)(i * i) + (uint64_t)(q * q);
		}
		fine[c] = chancap_sat16((int64_t)(s >> sf));
		coarse[c] = chancap_sat16((int64_t)(s >> (sf + sc)));
	}
}

/*
 * The energy of @p n frames over all 8 lanes, sat32(sum of I^2 + Q^2 >> shift), as
 * esp_sdr_chancap_energy() accumulates it.
 */
static inline uint32_t chancap_energy(const int16_t (*fr)[16], unsigned int n, unsigned int shift)
{
	uint64_t s = 0;

	for (unsigned int f = 0; f < n; f++) {
		for (unsigned int k = 0; k < 16; k++) {
			s += (uint64_t)((int32_t)fr[f][k] * fr[f][k]);
		}
	}
	s >>= shift;
	return s > INT32_MAX ? (uint32_t)INT32_MAX : (uint32_t)s;
}

static inline uint32_t chancap_run8_pack(uint32_t p)
{
	uint32_t fine = p >> 5, coarse = p >> 15;

	if (fine < 32767U) {
		return fine << 5;
	}
	return (coarse > 32767U ? 32767U : coarse) << 15;
}

/* ---- DDC bank: one channel ---- */

#define CHANCAP_DDC_L     32U /* taps of the decimating FIR */
#define CHANCAP_DDC_PMAX  128U /* longest mixer period in input pairs */

struct chancap_ddc {
	unsigned int d, period;
	/* Mixer exp(-j 2 pi f n / fs) over one period, Q14. */
	int16_t cr[CHANCAP_DDC_PMAX], ci[CHANCAP_DDC_PMAX];
	/* Taps in window order (newest input last), DC gain CHANCAP_H_ONE. */
	int16_t h[CHANCAP_DDC_L];
	uint64_t outputs;
};

/*
 * A channel @p num / @p den of the input rate above the LO (the mixer period is den pairs,
 * at most CHANCAP_DDC_PMAX), output rate fs / @p d, prototype cutoff @p fc of the input rate.
 */
static inline bool chancap_ddc_init(struct chancap_ddc *c, int num, unsigned int den,
				    unsigned int d, float fc, float atten_db)
{
	int16_t h[CHANCAP_DDC_L];

	int32_t habs = 0;

	/* A power-of-two period: the phase is the pair index masked, no division. */
	if (den == 0U || den > CHANCAP_DDC_PMAX || (den & (den - 1U)) != 0U || d == 0U ||
	    d > CHANCAP_DDC_L || fc <= 0.0f || fc >= 0.5f) {
		return false;
	}
	memset(c, 0, sizeof(*c));
	c->d = d;
	c->period = den;
	for (unsigned int n = 0; n < den; n++) {
		double w = -ESP_SDR_TAU * (double)num * (double)n / (double)den;

		c->cr[n] = (int16_t)lround(cos(w) * CHANCAP_TW_ONE);
		c->ci[n] = (int16_t)lround(sin(w) * CHANCAP_TW_ONE);
	}
	chancap_design_lowpass(h, CHANCAP_DDC_L, fc, chancap_kaiser_beta(atten_db));
	/* h is symmetric; kept in window order for the dot product with the input. */
	for (unsigned int n = 0; n < CHANCAP_DDC_L; n++) {
		c->h[n] = h[CHANCAP_DDC_L - 1U - n];
		habs += h[n] < 0 ? -h[n] : h[n];
	}
	/* The filter's 32-bit accumulators: sum |h| x 32768 must stay below 2^31. */
	return habs < 65536;
}

static inline unsigned int chancap_ddc_history(const struct chancap_ddc *c)
{
	return CHANCAP_DDC_L - 1U;
}

/*
 * Mix @p n input pairs, the first at pair index @p n0, to baseband: m = sat16(x c >> 14) with
 * the Q14 mixer, 16 bits kept as the vector unit does. Products and their sum stay within
 * 2^30, so 32-bit arithmetic is exact.
 */
static inline void chancap_ddc_mix(const struct chancap_ddc *c, const int16_t *xi,
				   const int16_t *xq, uint64_t n0, unsigned int n, int16_t *mi,
				   int16_t *mq)
{
	unsigned int ph = (unsigned int)n0 & (c->period - 1U);

	for (unsigned int t = 0; t < n; t++) {
		int32_t a = xi[t], b = xq[t], cr = c->cr[ph], ci = c->ci[ph];

		mi[t] = chancap_sat16((a * cr - b * ci) >> CHANCAP_TW_SHIFT);
		mq[t] = chancap_sat16((a * ci + b * cr) >> CHANCAP_TW_SHIFT);
		ph = (ph + 1U) & (c->period - 1U);
	}
}

/*
 * The decimating filter on a mixed window of CHANCAP_DDC_L pairs, the oldest first. 32-bit
 * accumulators: sum |h| < 2^16 (chancap_ddc_init()) keeps them exact.
 */
static inline void chancap_ddc_fir(const struct chancap_ddc *c, const int16_t *mi,
				   const int16_t *mq, int16_t *oi, int16_t *oq)
{
	int32_t ai = 0, aq = 0;

	for (unsigned int n = 0; n < CHANCAP_DDC_L; n++) {
		ai += (int32_t)c->h[n] * mi[n];
		aq += (int32_t)c->h[n] * mq[n];
	}
	*oi = chancap_sat16(ai >> CHANCAP_H_SHIFT);
	*oq = chancap_sat16(aq >> CHANCAP_H_SHIFT);
}

/*
 * One output: @p xi / @p xq point at the window [n - L + 1, n] of the newest input pair
 * n = outputs * d + d - 1; @p n0 is the pair index of the window's first sample (for the
 * mixer phase). A mixed sample depends only on its pair index, so mixing once per input and
 * filtering the mixed history (chancap_ddc_mix(), chancap_ddc_fir()) gives the same outputs.
 */
static inline void chancap_ddc_output(struct chancap_ddc *c, const int16_t *xi, const int16_t *xq,
				      uint64_t n0, int16_t *oi, int16_t *oq)
{
	int16_t mi[CHANCAP_DDC_L], mq[CHANCAP_DDC_L];

	chancap_ddc_mix(c, xi, xq, n0, CHANCAP_DDC_L, mi, mq);
	chancap_ddc_fir(c, mi, mq, oi, oq);
	c->outputs++;
}

/* ---- power, noise floor and detector ---- */

/*
 * Mean power of @p n complex samples in units of full scale squared times 2^-30 (a full
 * scale tone of amplitude 32767 gives about 2^30 per sample): sum of I^2 + Q^2 over n, then
 * divided by n; n a power of two keeps it exact as the vector unit's shift does.
 */
static inline uint32_t chancap_power(const int16_t *i, const int16_t *q, unsigned int n)
{
	uint64_t s = 0;

	for (unsigned int j = 0; j < n; j++) {
		s += (uint64_t)((int32_t)i[j] * i[j]) + (uint64_t)((int32_t)q[j] * q[j]);
	}
	return (uint32_t)(s / n);
}

/* Power in dB relative to a full scale tone (amplitude 32767 on both axes). */
static inline float chancap_power_db(uint32_t p)
{
	return 10.0f * log10f(((float)p + 0.5f) / (2.0f * 32767.0f * 32767.0f));
}

/*
 * A detector on one power stream (per block of samples): a slowly tracked noise floor, a
 * threshold a fixed number of dB above it, and a hang time after the power drops.
 *
 * The floor follows power below 4 times the floor at 1/64 per block (down quickly, up slowly),
 * and otherwise creeps up by 1/1024 per block, so that a burst never captures it.
 */
struct chancap_det {
	uint32_t floor;
	/* Threshold factor x256 (open) and its 3 dB lower close factor. */
	uint32_t open_x256, close_x256;
	uint32_t hang_blocks, hang_left;
	bool open;
	/* Blocks seen while the floor settles: the first ones only set it. */
	uint32_t warmup;
	/* Peak power of the current burst. */
	uint32_t peak;
};

static inline void chancap_det_init(struct chancap_det *d, int16_t threshold_db,
				    uint32_t hang_blocks)
{
	float f = powf(10.0f, (float)threshold_db / 10.0f);

	memset(d, 0, sizeof(*d));
	d->open_x256 = (uint32_t)lroundf(f * 256.0f);
	d->close_x256 = (uint32_t)lroundf(f * 256.0f / 2.0f);
	if (d->close_x256 < 256U) {
		d->close_x256 = 256U;
	}
	d->hang_blocks = hang_blocks;
	d->warmup = 16;
	d->floor = 1;
}

/* Feed one block's power; returns true when the detector is open after it. */
static inline bool chancap_det_update(struct chancap_det *d, uint32_t p)
{
	/* 32-bit arithmetic but for the threshold products: a few cycles per block and channel. */
	uint32_t fl = d->floor;

	if (d->warmup != 0U) {
		d->warmup--;
		d->floor = d->warmup == 0U ? (d->floor + p) / 2U : (d->floor > p ? p : d->floor);
		if (d->floor == 0U) {
			d->floor = 1U;
		}
		return false;
	}
	if (p < 4U * fl) {
		/* A 64th of the way to p, rounded down as an arithmetic shift would. */
		if (p >= fl) {
			d->floor = fl + ((p - fl) >> 6);
		} else {
			uint32_t dn = fl - p;

			d->floor = fl - (dn >> 6) - ((dn & 63U) != 0U ? 1U : 0U);
		}
	} else {
		d->floor = fl + (fl >> 10) + 1U;
	}
	if (d->floor == 0U) {
		d->floor = 1U;
	}
	if (d->open) {
		if ((uint64_t)p << 8 >= (uint64_t)fl * d->close_x256) {
			d->hang_left = d->hang_blocks;
			if (p > d->peak) {
				d->peak = p;
			}
		} else if (d->hang_left == 0U) {
			d->open = false;
		} else {
			d->hang_left--;
		}
	} else if ((uint64_t)p << 8 >= (uint64_t)fl * d->open_x256) {
		d->open = true;
		d->hang_left = d->hang_blocks;
		d->peak = p;
	}
	return d->open;
}

#endif /* ESP_SDR_CHANCAP_DSP_H_ */
