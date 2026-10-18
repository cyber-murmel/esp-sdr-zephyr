/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit test of include/esp_sdr/esp_sdr_dsp.h: the turn constant, the windows' defining values,
 * sinc, the radix-2 FFT against a direct DFT (whole tables and a stride through larger ones),
 * and the whole-cycle tone period and phase. Runs on native_sim.
 */
#include <math.h>

#include <zephyr/ztest.h>

#include <esp_sdr/esp_sdr_dsp.h>
#include <test_rng.h>

#define RNG_SEED 0x853c49e6748fea9bULL
#define NMAX     256U

static float tw_cos[NMAX / 2], tw_sin[NMAX / 2];
static float re[NMAX], im[NMAX];
static double ref_re[NMAX], ref_im[NMAX];

static void before_each(void *f)
{
	ARG_UNUSED(f);
	test_rng_seed(RNG_SEED);
}

ZTEST_SUITE(dsp, NULL, NULL, before_each, NULL, NULL);

ZTEST(dsp, test_tau)
{
	zassert_within(sin(ESP_SDR_TAU / 2.0), 0.0, 1e-15);
	zassert_within(cos(ESP_SDR_TAU), 1.0, 1e-15);
	zassert_within(ESP_SDR_TAU / 4.0, atan2(1.0, 0.0), 1e-15);
	zassert_equal(ESP_SDR_TAU_F, (float)ESP_SDR_TAU);
}

ZTEST(dsp, test_window_values)
{
	const uint32_t span = 100;

	/* Ends, middle and symmetry of each shape. */
	zassert_within(esp_sdr_window(ESP_SDR_WINDOW_HANN, 0, span), 0.0f, 1e-7f);
	zassert_within(esp_sdr_window(ESP_SDR_WINDOW_HANN, span / 2U, span), 1.0f, 1e-7f);
	zassert_within(esp_sdr_window(ESP_SDR_WINDOW_HAMMING, 0, span), 0.08f, 1e-6f);
	zassert_within(esp_sdr_window(ESP_SDR_WINDOW_HAMMING, span / 2U, span), 1.0f, 1e-6f);
	zassert_within(esp_sdr_window(ESP_SDR_WINDOW_BLACKMAN, 0, span), 0.0f, 1e-6f);
	zassert_within(esp_sdr_window(ESP_SDR_WINDOW_BLACKMAN, span / 2U, span), 1.0f, 1e-6f);
	zassert_within(esp_sdr_window(ESP_SDR_WINDOW_BLACKMAN_HARRIS, 0, span), 6e-5f, 1e-5f);
	zassert_within(esp_sdr_window(ESP_SDR_WINDOW_BLACKMAN_HARRIS, span / 2U, span), 1.0f,
		       1e-6f);
	for (uint32_t k = 0; k <= span; k++) {
		zassert_equal(esp_sdr_window(ESP_SDR_WINDOW_RECT, k, span), 1.0f);
		for (int kind = ESP_SDR_WINDOW_HAMMING; kind <= ESP_SDR_WINDOW_BLACKMAN_HARRIS;
		     kind++) {
			float a = esp_sdr_window((enum esp_sdr_window)kind, k, span);
			float b = esp_sdr_window((enum esp_sdr_window)kind, span - k, span);

			zassert_within(a, b, 1e-6f, "kind %d k %u: %f vs %f", kind, k,
				       (double)a, (double)b);
			zassert_true(a >= -1e-6f && a <= 1.0f + 1e-6f, "kind %d k %u: %f", kind,
				     k, (double)a);
		}
	}
}

ZTEST(dsp, test_window_periodic_hann_sum)
{
	/* A periodic Hann window of n points sums to n / 2 (its mean is one half). */
	double sum = 0.0;

	for (uint32_t k = 0; k < 64U; k++) {
		sum += (double)esp_sdr_window(ESP_SDR_WINDOW_HANN, k, 64U);
	}
	zassert_within(sum, 32.0, 1e-5);
}

ZTEST(dsp, test_sinc)
{
	zassert_equal(esp_sdr_sincf(0.0f), 1.0f);
	zassert_equal(esp_sdr_sinc(0.0), 1.0);
	zassert_within(esp_sdr_sincf(0.5f), (float)(4.0 / ESP_SDR_TAU), 1e-6f);
	zassert_within(esp_sdr_sinc(0.5), 4.0 / ESP_SDR_TAU, 1e-15);
	zassert_within(esp_sdr_sinc(-0.5), esp_sdr_sinc(0.5), 1e-15);
	for (int k = 1; k <= 5; k++) {
		zassert_within(esp_sdr_sinc((double)k), 0.0, 1e-15, "sinc(%d)", k);
		zassert_within(esp_sdr_sincf((float)k), 0.0f, 1e-6f, "sincf(%d)", k);
	}
	/* Just off zero, on the other branch: still continuous. */
	zassert_within(esp_sdr_sinc(1e-6), 1.0, 1e-11);
	zassert_within(esp_sdr_sincf(1e-5f), 1.0f, 1e-6f);
}

static void dft(uint32_t n)
{
	for (uint32_t k = 0; k < n; k++) {
		double sr = 0.0, si = 0.0;

		for (uint32_t t = 0; t < n; t++) {
			double a = -ESP_SDR_TAU * (double)((k * t) % n) / (double)n;

			sr += (double)re[t] * cos(a) - (double)im[t] * sin(a);
			si += (double)re[t] * sin(a) + (double)im[t] * cos(a);
		}
		ref_re[k] = sr;
		ref_im[k] = si;
	}
}

static void check_fft(uint32_t n, uint32_t tw_n)
{
	double worst = 0.0;

	for (uint32_t k = 0; k < n; k++) {
		re[k] = (float)(test_rng_uniform() * 2.0 - 1.0);
		im[k] = (float)(test_rng_uniform() * 2.0 - 1.0);
	}
	dft(n);
	esp_sdr_fft_twiddles(tw_cos, tw_sin, tw_n);
	esp_sdr_fft(re, im, n, tw_cos, tw_sin, tw_n);
	for (uint32_t k = 0; k < n; k++) {
		double e = hypot((double)re[k] - ref_re[k], (double)im[k] - ref_im[k]);

		worst = MAX(worst, e);
	}
	/* Float rounding: about log2(n) steps of a few ulps of values up to sqrt(n). */
	zassert_true(worst < 1e-5 * (double)n, "n %u tables %u: error %g", n, tw_n, worst);
}

ZTEST(dsp, test_fft_matches_dft)
{
	for (uint32_t n = 2; n <= NMAX; n *= 2U) {
		check_fft(n, n);
	}
}

ZTEST(dsp, test_fft_stride_through_larger_tables)
{
	for (uint32_t n = 2; n < NMAX; n *= 2U) {
		check_fft(n, NMAX);
	}
}

ZTEST(dsp, test_fft_tone_and_impulse)
{
	const uint32_t n = 64, bin = 5;

	/* A unit impulse at 0: flat spectrum of ones. */
	for (uint32_t k = 0; k < n; k++) {
		re[k] = k == 0 ? 1.0f : 0.0f;
		im[k] = 0.0f;
	}
	esp_sdr_fft_twiddles(tw_cos, tw_sin, n);
	esp_sdr_fft(re, im, n, tw_cos, tw_sin, n);
	for (uint32_t k = 0; k < n; k++) {
		zassert_within(re[k], 1.0f, 1e-6f);
		zassert_within(im[k], 0.0f, 1e-6f);
	}
	/* exp(j tau bin t / n): everything in bin "bin", natural order. */
	for (uint32_t t = 0; t < n; t++) {
		double a = ESP_SDR_TAU * (double)(bin * t) / (double)n;

		re[t] = (float)cos(a);
		im[t] = (float)sin(a);
	}
	esp_sdr_fft(re, im, n, tw_cos, tw_sin, n);
	for (uint32_t k = 0; k < n; k++) {
		zassert_within(re[k], k == bin ? (float)n : 0.0f, 1e-4f, "bin %u", k);
		zassert_within(im[k], 0.0f, 1e-4f, "bin %u", k);
	}
}

ZTEST(dsp, test_tone_period_exact)
{
	long cycles;
	/* fs / 8 fits exactly in any multiple of 8: the longest one below the maximum. */
	size_t n = esp_sdr_tone_period(10e6, 80e6, 256, 16380, &cycles);

	zassert_equal(n, 16376U);
	zassert_equal(cycles, 2047);
	n = esp_sdr_tone_period(-10e6, 80e6, 256, 16380, &cycles);
	zassert_equal(n, 16376U);
	zassert_equal(cycles, -2047);
	/* DC: no cycles, the longest period. */
	n = esp_sdr_tone_period(0.0, 80e6, 256, 4000, &cycles);
	zassert_equal(n, 4000U);
	zassert_equal(cycles, 0);
}

ZTEST(dsp, test_tone_period_best)
{
	static const double tones[] = {1e6, 1234567.0, -3.3e6, 100e3, 39.9e6};

	for (size_t i = 0; i < ARRAY_SIZE(tones); i++) {
		long cycles;
		size_t n = esp_sdr_tone_period(tones[i], 80e6, 256, 16380, &cycles);
		double got = (double)cycles * 80e6 / (double)n;

		zassert_true(n >= 256U && n <= 16380U, "%g Hz: n %zu", tones[i], n);
		/* No period in the range does better (brute force over all of them). */
		for (size_t len = 256; len <= 16380; len++) {
			double c = round(tones[i] * (double)len / 80e6);

			zassert_true(fabs(c * 80e6 / (double)len - tones[i]) >=
					     fabs(got - tones[i]) || fabs(got - tones[i]) < 1.0,
				     "%g Hz: %zu beats %zu", tones[i], len, n);
		}
	}
}

ZTEST(dsp, test_tone_period_window)
{
	long cycles = 99;
	/* A one-length window returns that length; cycles always written. */
	size_t n = esp_sdr_tone_period(1e6, 80e6, 300, 300, &cycles);

	zassert_equal(n, 300U);
	zassert_equal(cycles, 4);
	/* n_min 0 must not wrap the search below zero. */
	n = esp_sdr_tone_period(1.5e6, 80e6, 0, 3, &cycles);
	zassert_true(n >= 1U && n <= 3U, "n %zu", n);
}

ZTEST(dsp, test_tone_phase)
{
	const size_t n = 1000;

	zassert_equal(esp_sdr_tone_phase(7, 0, n), 0.0);
	zassert_within(esp_sdr_tone_phase(1, n / 4U, n), ESP_SDR_TAU / 4.0, 1e-12);
	zassert_within(esp_sdr_tone_phase(-1, n / 4U, n), 3.0 * ESP_SDR_TAU / 4.0, 1e-12);
	for (size_t k = 0; k < n; k++) {
		double ph = esp_sdr_tone_phase(123457, k, n);

		zassert_true(ph >= 0.0 && ph < ESP_SDR_TAU, "k %zu: %f", k, ph);
	}
	/* Reduced in turns first: a huge cycle count keeps the phase of a small one. */
	zassert_within(esp_sdr_tone_phase(1000000001L, 1, 4), esp_sdr_tone_phase(1, 1, 4), 1e-6);
}
