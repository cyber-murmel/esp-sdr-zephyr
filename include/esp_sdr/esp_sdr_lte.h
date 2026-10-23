/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file
 * @brief LTE downlink synchronization signals as a frequency reference.
 *
 * A base station's carrier is accurate to a small fraction of a ppm and sits on the 100 kHz
 * channel raster, so where a cell's carrier shows up in a capture tells how far the receiver's
 * crystal is off. This finds the primary synchronization signal (PSS) in a capture of about
 * 2 MS/s, decodes the cell with the secondary one (SSS) and measures the carrier offset from
 * the phase between the two symbols (frequency division duplex, normal cyclic prefix).
 *
 * Header only, plain C and libm without Zephyr, like esp_sdr_dsp.h, so that the library, host
 * tools and the native_sim tests share it. The caller supplies the work memory.
 */

#ifndef ESP_SDR_ESP_SDR_LTE_H_
#define ESP_SDR_ESP_SDR_LTE_H_

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <esp_sdr/esp_sdr_dsp.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup esp_sdr_lte
 * @{
 */

/** Subcarrier spacing in Hz. */
#define ESP_SDR_LTE_DF            15000.0
/** Period of the synchronization signals in seconds (twice per 10 ms frame). */
#define ESP_SDR_LTE_SYNC_PERIOD_S 5e-3
/** Time from the SSS symbol's useful part to the PSS symbol's, normal cyclic prefix, in seconds. */
#define ESP_SDR_LTE_SSS_LEAD_S    ((2048.0 + 144.0) / 30.72e6)
/** The synchronization signals fill 62 subcarriers either side of the unused DC one. */
#define ESP_SDR_LTE_SC            62
/** Longest symbol useful part in samples: sample rates up to 4 MS/s. */
#define ESP_SDR_LTE_L_MAX         272
/** Channel raster in Hz. */
#define ESP_SDR_LTE_RASTER_HZ     100e3

/** One complex sample, layout compatible with struct esp_sdr_iq16. */
struct esp_sdr_lte_iq {
	int16_t i;
	int16_t q;
};

/** Work memory: carve it from the floats of esp_sdr_lte_work_init(). */
struct esp_sdr_lte_work {
	/** Transform size, a power of two of at least the capture length plus a symbol. */
	uint32_t n_fft;
	/** The capture's spectrum. */
	float *xr, *xi;
	/** A template spectrum. */
	float *hr, *hi;
	/** Correlation product, then the correlation. */
	float *wr, *wi;
	/** Energy of each symbol-long window of the capture. */
	float *en;
	/** FFT twiddles. */
	float *twc, *tws;
};

/** Floats esp_sdr_lte_work_init() needs for a transform of @p n_fft. */
#define ESP_SDR_LTE_WORK_FLOATS(n_fft) (8U * (n_fft))

/** Result of esp_sdr_lte_detect(). */
struct esp_sdr_lte_cell {
	/** Sector (PSS root index 0 to 2). */
	int n_id2;
	/** Cell group (0 to 167). */
	int n_id1;
	/** Physical cell id, 3 * n_id1 + n_id2. */
	int cell_id;
	/** Half frame of the first synchronization signal found (0 or 1). */
	int half;
	/** Carrier offset from the receiver's centre frequency in Hz. */
	double offset_hz;
	/** Start of the first PSS symbol's useful part, in samples (fractional). */
	double timing;
	/** Strongest PSS correlation over the mean of the capture (a detection ratio). */
	float pss_ratio;
	/** Normalised PSS correlation at the final timing and frequency (1 = perfect). */
	float pss_metric;
	/** Normalised SSS correlation of the decoded cell (1 = perfect). */
	float sss_metric;
	/** Best SSS correlation of any other cell group. */
	float sss_second;
	/** Synchronization signal pairs used. */
	int pairs;
	/** Cell decoded with a clear margin. */
	int valid;
};

/** One PSS found by esp_sdr_lte_pss_search(). */
struct esp_sdr_lte_cand {
	/** Sector. */
	int n_id2;
	/** Carrier offset from the capture centre in Hz. */
	double offset_hz;
	/** Start of the PSS symbol's useful part, in samples. */
	size_t pos;
	/** Detection ratio of the correlation peak. */
	float ratio;
	/** Normalised correlation of the demodulated subcarriers (1 = perfect). */
	float metric;
};

static inline size_t esp_sdr_lte_symbol_len(double fs)
{
	return (size_t)(fs / ESP_SDR_LTE_DF + 0.5);
}

/**
 * @brief Set up the work memory.
 *
 * @param w Work structure to fill.
 * @param mem ESP_SDR_LTE_WORK_FLOATS(n_fft) floats.
 * @param n_fft Transform size, a power of two.
 */
static inline void esp_sdr_lte_work_init(struct esp_sdr_lte_work *w, float *mem, uint32_t n_fft)
{
	w->n_fft = n_fft;
	w->xr = mem;
	w->xi = mem + n_fft;
	w->hr = mem + 2U * n_fft;
	w->hi = mem + 3U * n_fft;
	w->wr = mem + 4U * n_fft;
	w->wi = mem + 5U * n_fft;
	w->en = mem + 6U * n_fft;
	w->twc = mem + 7U * n_fft;
	w->tws = w->twc + n_fft / 2U;
	esp_sdr_fft_twiddles(w->twc, w->tws, n_fft);
}

/** Subcarrier offset in units of 15 kHz of the idx-th of the 62 synchronization subcarriers. */
static inline int esp_sdr_lte_k(int idx)
{
	return idx < 31 ? idx - 31 : idx - 30;
}

/**
 * @brief The PSS Zadoff-Chu sequence of a sector.
 *
 * @param n_id2 Sector 0 to 2 (roots 25, 29, 34).
 * @param re Real parts, ESP_SDR_LTE_SC of them.
 * @param im Imaginary parts.
 */
static inline void esp_sdr_lte_pss(int n_id2, float *re, float *im)
{
	static const int root[3] = {25, 29, 34};
	int u = root[n_id2];

	for (int n = 0; n < ESP_SDR_LTE_SC; n++) {
		int m = n < 31 ? n * (n + 1) : (n + 1) * (n + 2);
		float a = -(float)(ESP_SDR_TAU / 2.0 / 63.0) * (float)((u * m) % 126);

		re[n] = cosf(a);
		im[n] = sinf(a);
	}
}

/* |re + j im|^2 in double: the running window sums need it. */
static inline double lte_pow(float re, float im)
{
	double a = (double)re, b = (double)im;

	return a * a + b * b;
}

static inline void lte_lfsr(const int *taps, int n_taps, int *out)
{
	int x[31] = {0, 0, 0, 0, 1};

	for (int i = 0; i + 5 < 31; i++) {
		int v = 0;

		for (int t = 0; t < n_taps; t++) {
			v ^= x[i + taps[t]];
		}
		x[i + 5] = v;
	}
	for (int i = 0; i < 31; i++) {
		out[i] = 1 - 2 * x[i];
	}
}

/**
 * @brief The SSS sequence of a cell group (3GPP TS 36.211, 6.11.2), +-1 per subcarrier.
 *
 * @param n_id1 Cell group 0 to 167.
 * @param n_id2 Sector 0 to 2.
 * @param half 0 for subframe 0, 1 for subframe 5.
 * @param d ESP_SDR_LTE_SC values.
 */
static inline void esp_sdr_lte_sss(int n_id1, int n_id2, int half, float *d)
{
	static const int s_taps[] = {2, 0};
	static const int c_taps[] = {3, 0};
	static const int z_taps[] = {4, 2, 1, 0};
	int s[31], c[31], z[31];
	int qp = n_id1 / 30;
	int q = (n_id1 + qp * (qp + 1) / 2) / 30;
	int m = n_id1 + q * (q + 1) / 2;
	int m0 = m % 31;
	int m1 = (m0 + m / 31 + 1) % 31;

	lte_lfsr(s_taps, 2, s);
	lte_lfsr(c_taps, 2, c);
	lte_lfsr(z_taps, 4, z);
	for (int n = 0; n < 31; n++) {
		float s0 = (float)s[(n + m0) % 31];
		float s1 = (float)s[(n + m1) % 31];
		float c0 = (float)c[(n + n_id2) % 31];
		float c1 = (float)c[(n + n_id2 + 3) % 31];
		float z0 = (float)z[(n + m0 % 8) % 31];
		float z1 = (float)z[(n + m1 % 8) % 31];

		if (half == 0) {
			d[2 * n] = s0 * c0;
			d[2 * n + 1] = s1 * c1 * z0;
		} else {
			d[2 * n] = s1 * c0;
			d[2 * n + 1] = s0 * c1 * z1;
		}
	}
}

/**
 * @brief Demodulate the 62 synchronization subcarriers of one symbol.
 *
 * Sums the symbol's useful part against the subcarriers of a carrier at @p f_hz. The subcarrier
 * phase is relative to the symbol start, the carrier phase to the capture start, so the phase
 * between two symbols carries the frequency error of @p f_hz.
 *
 * @param x Capture.
 * @param n Capture length.
 * @param fs Sample rate in Hz, 1.92 to 4 MS/s.
 * @param start Start of the useful part in samples, fractional.
 * @param f_hz Carrier offset to test, from the capture's centre.
 * @param yr Real parts of the 62 subcarrier values.
 * @param yi Imaginary parts.
 * @retval 0 on success, -ERANGE if the symbol is not inside the capture.
 */
static inline int esp_sdr_lte_demod(const struct esp_sdr_lte_iq *x, size_t n, double fs,
				    double start, double f_hz, float *yr, float *yi)
{
	size_t len = esp_sdr_lte_symbol_len(fs);
	long first = (long)ceil(start - 1e-9);
	float zr[ESP_SDR_LTE_L_MAX], zi[ESP_SDR_LTE_L_MAX];

	if (first < 0 || (size_t)first + len > n || len > ESP_SDR_LTE_L_MAX) {
		return -ERANGE;
	}
	double w = -ESP_SDR_TAU * f_hz / fs;
	float cs = cosf((float)w), sn = sinf((float)w);
	double p0 = fmod(w * (double)first, ESP_SDR_TAU);
	float br = cosf((float)p0), bi = sinf((float)p0);

	for (size_t i = 0; i < len; i++) {
		float xr = (float)x[first + (long)i].i;
		float xi = (float)x[first + (long)i].q;

		zr[i] = xr * br - xi * bi;
		zi[i] = xr * bi + xi * br;
		float t = br * cs - bi * sn;

		bi = br * sn + bi * cs;
		br = t;
	}
	for (int idx = 0; idx < ESP_SDR_LTE_SC; idx++) {
		double ws = -ESP_SDR_TAU * (double)esp_sdr_lte_k(idx) * ESP_SDR_LTE_DF / fs;
		double ph = fmod(ws * ((double)first - start), ESP_SDR_TAU);
		float rc = cosf((float)ws), rs = sinf((float)ws);
		float pr = cosf((float)ph), pi_ = sinf((float)ph);
		float ar = 0.0f, ai = 0.0f;

		for (size_t i = 0; i < len; i++) {
			ar += zr[i] * pr - zi[i] * pi_;
			ai += zr[i] * pi_ + zi[i] * pr;
			float t = pr * rc - pi_ * rs;

			pi_ = pr * rs + pi_ * rc;
			pr = t;
		}
		yr[idx] = ar / (float)len;
		yi[idx] = ai / (float)len;
	}
	return 0;
}

/** Normalised correlation of a demodulated PSS with its sequence: 1 for a perfect match. */
static inline float lte_pss_metric(const float *yr, const float *yi, const float *dr,
				   const float *di)
{
	float cr = 0.0f, ci = 0.0f, a = 0.0f;

	for (int k = 0; k < ESP_SDR_LTE_SC; k++) {
		cr += dr[k] * yr[k] + di[k] * yi[k];
		ci += dr[k] * yi[k] - di[k] * yr[k];
		a += sqrtf(yr[k] * yr[k] + yi[k] * yi[k]);
	}
	return a > 0.0f ? sqrtf(cr * cr + ci * ci) / a : 0.0f;
}

static inline void lte_ifft(struct esp_sdr_lte_work *w)
{
	uint32_t n = w->n_fft;

	for (uint32_t i = 0; i < n; i++) {
		w->wi[i] = -w->wi[i];
	}
	esp_sdr_fft(w->wr, w->wi, n, w->twc, w->tws, n);
	for (uint32_t i = 0; i < n; i++) {
		w->wr[i] /= (float)n;
		w->wi[i] /= -(float)n;
	}
}

/* Coarse PSS search candidates kept for the final choice. */
#define LTE_CANDIDATES 6
#define ARRAY_SIZE_LTE(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* Spectrum of the sector's useful-part template, carrier at the capture centre, in w->hr/hi. */
static inline void lte_template_spectrum(struct esp_sdr_lte_work *w, int u, double fs, size_t len)
{
	uint32_t nf = w->n_fft;
	float dr[ESP_SDR_LTE_SC], di[ESP_SDR_LTE_SC];
	float norm = 0.0f;

	esp_sdr_lte_pss(u, dr, di);
	for (uint32_t i = 0; i < nf; i++) {
		w->hr[i] = 0.0f;
		w->hi[i] = 0.0f;
	}
	for (size_t i = 0; i < len; i++) {
		float sr = 0.0f, si = 0.0f;

		for (int idx = 0; idx < ESP_SDR_LTE_SC; idx++) {
			float a = (float)(ESP_SDR_TAU * esp_sdr_lte_k(idx) * ESP_SDR_LTE_DF / fs *
					  (double)i);
			float c = cosf(a), s = sinf(a);

			sr += dr[idx] * c - di[idx] * s;
			si += dr[idx] * s + di[idx] * c;
		}
		w->hr[i] = sr;
		w->hi[i] = si;
		norm += sr * sr + si * si;
	}
	norm = sqrtf(norm);
	for (size_t i = 0; i < len; i++) {
		w->hr[i] /= norm;
		w->hi[i] /= norm;
	}
	esp_sdr_fft(w->hr, w->hi, nf, w->twc, w->tws, nf);
}

/* Correlate the capture with the template at a carrier offset of `bins` transform bins. */
static inline float lte_corr_peak(struct esp_sdr_lte_work *w, int bins, size_t n, size_t len,
				  size_t *at, float *ratio)
{
	uint32_t nf = w->n_fft;
	uint32_t sh = (uint32_t)(bins & (int)(nf - 1U));
	float peak = 0.0f, sum = 0.0f;
	size_t cnt = n - len + 1U;

	for (uint32_t b = 0; b < nf; b++) {
		uint32_t xb = (b + sh) & (nf - 1U);

		w->wr[b] = w->xr[xb] * w->hr[b] + w->xi[xb] * w->hi[b];
		w->wi[b] = w->xi[xb] * w->hr[b] - w->xr[xb] * w->hi[b];
	}
	lte_ifft(w);
	for (size_t i = 0; i < cnt; i++) {
		float r = (w->wr[i] * w->wr[i] + w->wi[i] * w->wi[i]) / w->en[i];

		sum += r;
		if (r > peak) {
			peak = r;
			*at = i;
		}
	}
	*ratio = peak / (sum / (float)cnt + 1e-20f);
	return peak;
}

/**
 * @brief Search the capture for a PSS.
 *
 * Correlates the three sequences at carrier offsets in steps of about 15 kHz up to
 * @p max_offset_hz either side, normalised by the energy of each symbol-long window. A
 * Zadoff-Chu sequence trades frequency error for timing error, so a coarse step a few kHz off
 * gives a peak tens of samples from the true start, and several such ridge points can outdo the
 * true one. So the few strongest coarse points are each refined in steps of about 2.5 kHz and
 * the one whose demodulated subcarriers best match the sequence wins.
 *
 * @param x Capture, at least a PSS symbol long.
 * @param n Capture length; n + one symbol must fit the transform of @p w.
 * @param fs Sample rate in Hz, 1.92 to 4 MS/s.
 * @param max_offset_hz Largest carrier offset to try.
 * @param w Work memory.
 * @param out Candidates, best demodulated metric first.
 * @param max_out Capacity of @p out.
 * @return The number of candidates, or -EINVAL for a capture the transform cannot hold.
 */
static inline int esp_sdr_lte_pss_search(const struct esp_sdr_lte_iq *x, size_t n, double fs,
					 double max_offset_hz, struct esp_sdr_lte_work *w,
					 struct esp_sdr_lte_cand *out, int max_out)
{
	size_t len = esp_sdr_lte_symbol_len(fs);
	uint32_t nf = w->n_fft;

	if (n < len + 1U || n + len > nf || len > ESP_SDR_LTE_L_MAX) {
		return -EINVAL;
	}
	double mx = 0.0, my = 0.0;

	for (size_t i = 0; i < n; i++) {
		mx += x[i].i;
		my += x[i].q;
	}
	mx /= (double)n;
	my /= (double)n;
	for (uint32_t i = 0; i < nf; i++) {
		w->xr[i] = i < n ? (float)((double)x[i].i - mx) : 0.0f;
		w->xi[i] = i < n ? (float)((double)x[i].q - my) : 0.0f;
	}
	/* Energy of each window of one symbol, by a running sum (kept in double). */
	double e = 0.0;

	for (size_t i = 0; i < len; i++) {
		e += lte_pow(w->xr[i], w->xi[i]);
	}
	for (size_t i = 0; i + len <= n; i++) {
		w->en[i] = (float)e + 1e-3f;
		if (i + len < n) {
			e += lte_pow(w->xr[i + len], w->xi[i + len]);
			e -= lte_pow(w->xr[i], w->xi[i]);
			if (e < 0.0) {
				e = 0.0;
			}
		}
	}
	esp_sdr_fft(w->xr, w->xi, nf, w->twc, w->tws, nf);

	double bin_hz = fs / (double)nf;
	int step = (int)(ESP_SDR_LTE_DF / bin_hz + 0.5);
	int fine = (int)(2500.0 / bin_hz + 0.5);
	int max_s = (int)(max_offset_hz / (step * bin_hz));
	struct {
		float peak;
		int u, bins;
	} cand[LTE_CANDIDATES] = {{0}};

	/*
	 * Coarse pass. Keep the strongest few points apart from each other: neither the peak
	 * height nor the detection ratio is reliable on its own along the ambiguity ridge.
	 */
	for (int u = 0; u < 3; u++) {
		lte_template_spectrum(w, u, fs, len);
		for (int s = -max_s; s <= max_s; s++) {
			size_t at;
			float rat, peak = lte_corr_peak(w, s * step, n, len, &at, &rat);
			int worst = 0, near = -1;

			for (int k = 0; k < LTE_CANDIDATES; k++) {
				if (cand[k].peak > 0.0f && cand[k].u == u &&
				    abs(cand[k].bins - s * step) < 2 * step) {
					near = k;
				}
				if (cand[k].peak < cand[worst].peak) {
					worst = k;
				}
			}
			if (near >= 0) {
				if (peak > cand[near].peak) {
					cand[near].peak = peak;
					cand[near].bins = s * step;
				}
			} else if (peak > cand[worst].peak) {
				cand[worst].peak = peak;
				cand[worst].u = u;
				cand[worst].bins = s * step;
			}
		}
	}
	/* Fine pass for each, then the demodulated PSS metric orders them. */
	struct esp_sdr_lte_cand res[LTE_CANDIDATES];
	int count = 0;

	for (int k = 0; k < LTE_CANDIDATES; k++) {
		if (cand[k].peak <= 0.0f) {
			continue;
		}
		float dr[ESP_SDR_LTE_SC], di[ESP_SDR_LTE_SC], yr[ESP_SDR_LTE_SC], yi[ESP_SDR_LTE_SC];
		float fpeak = 0.0f, frat = 0.0f;
		int fbins = cand[k].bins;
		size_t fat = 0;
		struct esp_sdr_lte_cand r = {.n_id2 = cand[k].u, .metric = -1.0f};

		lte_template_spectrum(w, cand[k].u, fs, len);
		for (int j = -6; j <= 6; j++) {
			size_t at;
			float rat, peak = lte_corr_peak(w, cand[k].bins + j * fine, n, len, &at, &rat);

			if (peak > fpeak) {
				fpeak = peak;
				frat = rat;
				fbins = cand[k].bins + j * fine;
				fat = at;
			}
		}
		esp_sdr_lte_pss(cand[k].u, dr, di);
		for (int dt = -2; dt <= 2; dt++) {
			for (int df = -1; df <= 1; df++) {
				double f = (double)fbins * bin_hz + 1250.0 * df;

				if (esp_sdr_lte_demod(x, n, fs, (double)fat + 0.25 * dt, f, yr, yi) != 0) {
					continue;
				}
				float m = lte_pss_metric(yr, yi, dr, di);

				if (m > r.metric) {
					r.metric = m;
					r.ratio = frat;
					r.pos = fat;
					r.offset_hz = f;
				}
			}
		}
		if (r.metric < 0.0f) {
			continue;
		}
		int at = count++;

		while (at > 0 && res[at - 1].metric < r.metric) {
			res[at] = res[at - 1];
			at--;
		}
		res[at] = r;
	}
	for (int k = 0; k < count && k < max_out; k++) {
		out[k] = res[k];
	}
	return count < max_out ? count : max_out;
}

/* Refine one PSS candidate and decode its cell with the SSS in front of each of its repeats. */
static inline int lte_decode(const struct esp_sdr_lte_iq *x, size_t n, double fs,
			     const struct esp_sdr_lte_cand *cd, struct esp_sdr_lte_cell *cell)
{
	size_t len = esp_sdr_lte_symbol_len(fs);
	double lead = ESP_SDR_LTE_SSS_LEAD_S * fs;
	double period = ESP_SDR_LTE_SYNC_PERIOD_S * fs;
	int n_id2 = cd->n_id2;
	double c_hz = cd->offset_hz, first;
	size_t at = cd->pos;
	float dpr[ESP_SDR_LTE_SC], dpi[ESP_SDR_LTE_SC];

	*cell = (struct esp_sdr_lte_cell){0};
	cell->pss_ratio = cd->ratio;
	esp_sdr_lte_pss(n_id2, dpr, dpi);
	first = (double)at;
	/* Move to the earliest synchronization signal pair the capture holds. */
	while (first - period - lead >= 0.0) {
		first -= period;
	}

	/*
	 * Timing and frequency refinement on the strongest pair: the search left the timing within
	 * a sample and the frequency within 1.25 kHz; the metric falls from 0.9 to 0.6 a sample off,
	 * so a quarter-sample grid, then an eighth.
	 */
	double s_best = (double)at, f_best = c_hz;
	float m_best = 0.0f;
	float yr[ESP_SDR_LTE_SC], yi[ESP_SDR_LTE_SC];

	for (int pass = 0; pass < 2; pass++) {
		double s0 = s_best, f0 = f_best;
		int dt_max = pass == 0 ? 6 : 4;
		int df_max = pass == 0 ? 4 : 4;
		double dt_step = pass == 0 ? 0.5 : 0.125;
		double df_step = pass == 0 ? 750.0 : 250.0;

		for (int dt = -dt_max; dt <= dt_max; dt++) {
			for (int df = -df_max; df <= df_max; df++) {
				double s = s0 + dt_step * dt, f = f0 + df_step * df;

				if (esp_sdr_lte_demod(x, n, fs, s, f, yr, yi) != 0) {
					continue;
				}
				float m = lte_pss_metric(yr, yi, dpr, dpi);

				if (m > m_best) {
					m_best = m;
					s_best = s;
					f_best = f;
				}
			}
		}
	}
	cell->n_id2 = n_id2;
	cell->pss_metric = m_best;
	first = s_best;
	while (first - period - lead >= 0.0) {
		first -= period;
	}

	/* The SSS of every pair: z = Y_sss conj(Y_pss) removes the channel. */
	float best = 0.0f, second = 0.0f;
	int best_n1 = 0, best_half = 0;
	double best_f = f_best, best_phase = 0.0;
	int pairs = 0;

	for (double s = first; s + (double)len <= (double)n; s += period) {
		if (s - lead >= 0.0) {
			pairs++;
		}
	}
	cell->pairs = pairs;
	if (pairs == 0) {
		return -ENOENT;
	}
	for (int dfi = -4; dfi <= 4; dfi++) {
		double f = f_best + 5000.0 * dfi;
		float zr[4][ESP_SDR_LTE_SC], zi[4][ESP_SDR_LTE_SC];
		float zabs = 0.0f;
		int occ[4];
		int np = 0, idx = 0;

		for (double s = first; s + (double)len <= (double)n && np < 4; s += period, idx++) {
			float sr[ESP_SDR_LTE_SC], si[ESP_SDR_LTE_SC];

			if (s - lead < 0.0 || esp_sdr_lte_demod(x, n, fs, s, f, yr, yi) != 0 ||
			    esp_sdr_lte_demod(x, n, fs, s - lead, f, sr, si) != 0) {
				continue;
			}
			for (int k = 0; k < ESP_SDR_LTE_SC; k++) {
				zr[np][k] = sr[k] * yr[k] + si[k] * yi[k];
				zi[np][k] = si[k] * yr[k] - sr[k] * yi[k];
				zabs += sqrtf(zr[np][k] * zr[np][k] + zi[np][k] * zi[np][k]);
			}
			occ[np++] = idx;
		}
		if (np == 0 || zabs <= 0.0f) {
			continue;
		}
		for (int n1 = 0; n1 < 168; n1++) {
			for (int h = 0; h < 2; h++) {
				float cr = 0.0f, ci = 0.0f;

				for (int j = 0; j < np; j++) {
					float d[ESP_SDR_LTE_SC];

					esp_sdr_lte_sss(n1, n_id2, (h + occ[j]) & 1, d);
					for (int k = 0; k < ESP_SDR_LTE_SC; k++) {
						/* z * dS * dP: real and positive for a match. */
						float ar = zr[j][k] * dpr[k] - zi[j][k] * dpi[k];
						float ai = zr[j][k] * dpi[k] + zi[j][k] * dpr[k];

						cr += d[k] * ar;
						ci += d[k] * ai;
					}
				}
				float m = sqrtf(cr * cr + ci * ci) / zabs;

				if (m > best) {
					if (n1 != best_n1) {
						second = best;
					}
					best = m;
					best_n1 = n1;
					best_half = h;
					best_f = f;
					best_phase = atan2((double)ci, (double)cr);
				} else if (n1 != best_n1 && m > second) {
					second = m;
				}
			}
		}
	}
	double delta = best_phase / (ESP_SDR_TAU * ESP_SDR_LTE_SSS_LEAD_S);

	cell->n_id1 = best_n1;
	cell->cell_id = 3 * best_n1 + n_id2;
	cell->half = best_half;
	cell->offset_hz = best_f - delta;
	cell->timing = first;
	cell->sss_metric = best;
	cell->sss_second = second;
	cell->valid = best >= 0.55f && best > 1.25f * second;
	return 0;
}

/**
 * @brief Find a cell in a capture and measure its carrier offset.
 *
 * Runs esp_sdr_lte_pss_search(), then for each of the best few PSS candidates refines the
 * timing (an eighth of a sample) and frequency (250 Hz), decodes the cell group with the SSS
 * in front of every PSS in the capture and takes the fine offset from the phase between the
 * SSS and PSS symbols (range +-7 kHz, about 1 kHz accurate). The PSS alone cannot tell the
 * candidates on a Zadoff-Chu ambiguity ridge apart; the one whose SSS matches best wins. The
 * synchronization signals repeat every 5 ms, so a capture of 5 ms or more holds several
 * pairs, which are combined.
 *
 * @param x Capture, any gain that does not clip.
 * @param n Capture length.
 * @param fs Sample rate in Hz, 1.92 to 4 MS/s.
 * @param max_offset_hz Largest carrier offset from the capture centre to look for.
 * @param min_ratio Smallest PSS detection ratio to go on with (about 20 for a 1 ms capture).
 * @param w Work memory.
 * @param cell Result.
 * @retval 0 on success; cell->valid tells whether the SSS decoded with margin.
 * @retval -ENOENT if no PSS stood out, -EINVAL for a capture the work memory cannot hold.
 */
static inline int esp_sdr_lte_detect(const struct esp_sdr_lte_iq *x, size_t n, double fs,
				     double max_offset_hz, float min_ratio,
				     struct esp_sdr_lte_work *w, struct esp_sdr_lte_cell *cell)
{
	struct esp_sdr_lte_cand cd[4];
	int count = esp_sdr_lte_pss_search(x, n, fs, max_offset_hz, w, cd, ARRAY_SIZE_LTE(cd));
	int found = 0;

	*cell = (struct esp_sdr_lte_cell){0};
	if (count < 0) {
		return count;
	}
	for (int k = 0; k < count; k++) {
		struct esp_sdr_lte_cell c;

		if (cd[k].ratio < min_ratio || lte_decode(x, n, fs, &cd[k], &c) != 0) {
			continue;
		}
		if (!found || (c.valid && !cell->valid) ||
		    (c.valid == cell->valid && c.sss_metric > cell->sss_metric)) {
			*cell = c;
			found = 1;
		}
	}
	return found ? 0 : -ENOENT;
}

/**
 * @brief Crystal error from a measured carrier.
 *
 * The carrier snaps to the 100 kHz raster; the snap is only right while the receiver is off by
 * less than 50 kHz.
 *
 * @param lo_hz Frequency the receiver was asked for (the LO it believes it has), in Hz.
 * @param offset_hz Measured carrier offset from it.
 * @param raster_hz Set to the raster frequency the carrier was taken to be on.
 * @return The error of the receiver's clock in ppb: positive means its LO runs high.
 */
static inline double esp_sdr_lte_ppb(double lo_hz, double offset_hz, double *raster_hz)
{
	double carrier = lo_hz + offset_hz;
	double nominal = ESP_SDR_LTE_RASTER_HZ * floor(carrier / ESP_SDR_LTE_RASTER_HZ + 0.5);

	if (raster_hz != NULL) {
		*raster_hz = nominal;
	}
	return 1e9 * (nominal - carrier) / lo_hz;
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ESP_SDR_ESP_SDR_LTE_H_ */
