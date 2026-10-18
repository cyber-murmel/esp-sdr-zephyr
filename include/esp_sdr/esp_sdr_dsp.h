/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file
 * @brief Small signal processing helpers shared by the library, the apps, the tests and the
 * host tools.
 *
 * The turn constant, windows, sinc, an in-place radix-2 FFT and whole-cycle tone tables. Header
 * only, plain C and libm without Zephyr, so that host builds (apps/link/tools,
 * apps/osmosdr/tools) and the native_sim unit tests use the same code.
 */

#ifndef ESP_SDR_ESP_SDR_DSP_H_
#define ESP_SDR_ESP_SDR_DSP_H_

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup esp_sdr_dsp
 * @{
 */

/** Radians per turn; pi is ESP_SDR_TAU / 2. */
#define ESP_SDR_TAU   6.28318530717958647692
/** ESP_SDR_TAU as a float. */
#define ESP_SDR_TAU_F 6.28318530717958647692f

/** Window shapes, in the order of gnuradio.fft.window's kinds. */
enum esp_sdr_window {
	/** 0.54 - 0.46 cos. */
	ESP_SDR_WINDOW_HAMMING,
	/** 0.5 - 0.5 cos. */
	ESP_SDR_WINDOW_HANN,
	/** 0.42 - 0.5 cos + 0.08 cos 2. */
	ESP_SDR_WINDOW_BLACKMAN,
	/** All ones. */
	ESP_SDR_WINDOW_RECT,
	/** Four-term Blackman-Harris. */
	ESP_SDR_WINDOW_BLACKMAN_HARRIS,
};

/**
 * @brief One window coefficient.
 *
 * @param kind Window shape.
 * @param k Index, 0 to @p span.
 * @param span The index of the window's far end: n - 1 for a symmetric window of n
 *        coefficients (filter design), n for a periodic one (spectra).
 * @return The coefficient at @p k.
 */
static inline float esp_sdr_window(enum esp_sdr_window kind, uint32_t k, uint32_t span)
{
	float x = ESP_SDR_TAU_F * (float)k / (float)span;

	switch (kind) {
	case ESP_SDR_WINDOW_HAMMING:
		return 0.54f - 0.46f * cosf(x);
	case ESP_SDR_WINDOW_HANN:
		return 0.5f - 0.5f * cosf(x);
	case ESP_SDR_WINDOW_BLACKMAN:
		return 0.42f - 0.5f * cosf(x) + 0.08f * cosf(2.0f * x);
	case ESP_SDR_WINDOW_RECT:
		return 1.0f;
	default:
		return 0.35875f - 0.48829f * cosf(x) + 0.14128f * cosf(2.0f * x) -
		       0.01168f * cosf(3.0f * x);
	}
}

/**
 * @brief Normalized sinc.
 *
 * @param x Argument.
 * @return sin(pi x) / (pi x), 1 near 0.
 */
static inline float esp_sdr_sincf(float x)
{
	float a = ESP_SDR_TAU_F / 2.0f * x;

	return fabsf(x) < 1e-6f ? 1.0f : sinf(a) / a;
}

/**
 * @brief Normalized sinc in double precision (host tools, filter design).
 *
 * @param x Argument.
 * @return sin(pi x) / (pi x), 1 near 0.
 */
static inline double esp_sdr_sinc(double x)
{
	double a = ESP_SDR_TAU / 2.0 * x;

	return fabs(x) < 1e-9 ? 1.0 : sin(a) / a;
}

/**
 * @brief Twiddle factors for esp_sdr_fft().
 *
 * @param tw_cos Out: cos(-tau k / n) for k below n / 2.
 * @param tw_sin Out: sin(-tau k / n) for k below n / 2.
 * @param n The largest transform the tables serve, a power of two.
 */
static inline void esp_sdr_fft_twiddles(float *tw_cos, float *tw_sin, uint32_t n)
{
	for (uint32_t k = 0; k < n / 2U; k++) {
		float a = -ESP_SDR_TAU_F * (float)k / (float)n;

		tw_cos[k] = cosf(a);
		tw_sin[k] = sinf(a);
	}
}

/**
 * @brief Forward FFT in place, radix 2, output in natural order (DC first).
 *
 * @param re Real parts, @p n entries.
 * @param im Imaginary parts, @p n entries.
 * @param n Points, a power of two up to @p tw_n.
 * @param tw_cos Twiddles of esp_sdr_fft_twiddles() for @p tw_n.
 * @param tw_sin Twiddles of esp_sdr_fft_twiddles() for @p tw_n.
 * @param tw_n The tables' transform size.
 */
static inline void esp_sdr_fft(float *re, float *im, uint32_t n, const float *tw_cos,
			       const float *tw_sin, uint32_t tw_n)
{
	for (uint32_t i = 1, j = 0; i < n; i++) {
		uint32_t bit = n >> 1;

		for (; (j & bit) != 0U; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			float t = re[i];

			re[i] = re[j];
			re[j] = t;
			t = im[i];
			im[i] = im[j];
			im[j] = t;
		}
	}
	for (uint32_t len = 2; len <= n; len <<= 1) {
		uint32_t half = len >> 1;
		uint32_t step = tw_n / len;

		for (uint32_t i = 0; i < n; i += len) {
			for (uint32_t k = 0; k < half; k++) {
				float wr = tw_cos[k * step];
				float wi = tw_sin[k * step];
				uint32_t a = i + k;
				uint32_t b = a + half;
				float xr = re[b] * wr - im[b] * wi;
				float xi = re[b] * wi + im[b] * wr;

				re[b] = re[a] - xr;
				im[b] = im[a] - xi;
				re[a] += xr;
				im[a] += xi;
			}
		}
	}
}

/**
 * @brief The period of a seamless tone loop.
 *
 * Searches the lengths from @p n_max down to @p n_min for the one whose whole number of
 * cycles comes closest to @p hz, and stops at the first within 1 Hz.
 *
 * @param hz Tone frequency, Hz, either sign.
 * @param fs Sample rate, Hz.
 * @param n_min Shortest period, at least 1.
 * @param n_max Longest period, at least @p n_min.
 * @param cycles Out: whole cycles in the period (negative for negative @p hz).
 * @return The period length in samples.
 */
static inline size_t esp_sdr_tone_period(double hz, double fs, size_t n_min, size_t n_max,
					 long *cycles)
{
	double best = INFINITY;
	size_t n = n_max;

	*cycles = 0;
	for (size_t len = n_max; len >= n_min && len > 0U; len--) {
		long c = lround(hz * (double)len / fs);
		double err = fabs((double)c * fs / (double)len - hz);

		if (err < best) {
			best = err;
			n = len;
			*cycles = c;
			if (err < 1.0) {
				break;
			}
		}
	}
	return n;
}

/**
 * @brief Phase of one sample of a tone period.
 *
 * @param cycles Whole cycles in the period, from esp_sdr_tone_period().
 * @param k Sample index.
 * @param n Period length.
 * @return The phase of sample @p k in radians, 0 to tau, reduced in turns first so that
 *         long periods keep their precision.
 */
static inline double esp_sdr_tone_phase(long cycles, size_t k, size_t n)
{
	double t = (double)cycles * (double)k / (double)n;

	return ESP_SDR_TAU * (t - floor(t));
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ESP_SDR_ESP_SDR_DSP_H_ */
