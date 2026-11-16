/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit test of the gapless receiver's width sorting (lib/esp_sdr/src/esp_sdr_gapless_sort.h):
 * chunks of synthetic 80 MS/s dump words (10-bit I and Q in a word, noise added, quantized)
 * judged wide or narrow: band-limited noise 17 MHz wide (Wi-Fi's occupied band) wide, Bluetooth
 * LE GFSK, 802.15.4 O-QPSK (MSK) and two equal tones at any spacing narrow, quiet chunks
 * inheriting, a DC offset taken out, the mean frequency from the lag-one correlation, and the
 * sums the same whether a chunk is judged in one go or in pieces.
 */

#include <math.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <esp_sdr/esp_sdr_dsp.h>
#include <test_rng.h>

#include "esp_sdr_gapless_sort.h"

#define FS     80e6
#define N      SORT_CHUNK
/* Noise power per sample (raw units squared, I^2 + Q^2): 4 raw units rms a component. */
#define NOISE  32.0
/* Taps of the band-limiting filter. */
#define TAPS   129

static double si[N + TAPS], sq[N + TAPS];
static uint32_t words[N];

static uint32_t word(int32_t i, int32_t q)
{
	i = CLAMP(i, -512, 511);
	q = CLAMP(q, -512, 511);
	return (((uint32_t)i & 0x3ffU) << 10) | ((uint32_t)q & 0x3ffU);
}

/* The signal in si/sq scaled to @p power per sample, noise and @p dc added, into words[]. */
static void quantize(double power, double dc_i, double dc_q)
{
	double p = 0.0, g;

	for (int k = 0; k < N; k++) {
		p += si[k] * si[k] + sq[k] * sq[k];
	}
	g = p > 0.0 ? sqrt(power / (p / N)) : 0.0;
	for (int k = 0; k < N; k++) {
		double ni = sqrt(NOISE / 2.0) * test_rng_gauss();
		double nq = sqrt(NOISE / 2.0) * test_rng_gauss();

		words[k] = word((int32_t)lrint(g * si[k] + ni + dc_i),
				(int32_t)lrint(g * sq[k] + nq + dc_q));
	}
}

/* White noise through a windowed-sinc low pass @p width wide, shifted to @p offset Hz. */
static void band_noise(double width, double offset)
{
	static double wi[N + TAPS], wq[N + TAPS], h[TAPS];
	double fc = width / 2.0 / FS;

	for (int k = 0; k < TAPS; k++) {
		double x = k - (TAPS - 1) / 2.0;

		h[k] = 2.0 * fc * esp_sdr_sinc(2.0 * fc * x) *
		       (0.54 - 0.46 * cos(ESP_SDR_TAU * k / (TAPS - 1)));
	}
	for (int k = 0; k < N + TAPS; k++) {
		wi[k] = test_rng_gauss();
		wq[k] = test_rng_gauss();
	}
	for (int k = 0; k < N; k++) {
		double a = 0.0, b = 0.0, ph = ESP_SDR_TAU * offset / FS * k;

		for (int j = 0; j < TAPS; j++) {
			a += h[j] * wi[k + j];
			b += h[j] * wq[k + j];
		}
		si[k] = a * cos(ph) - b * sin(ph);
		sq[k] = a * sin(ph) + b * cos(ph);
	}
}

/* Continuous-phase FSK of random symbols: @p rate symbols a second, index @p h, Gaussian
 * shaped with BT @p bt (0: rectangular), at @p offset Hz. */
static void fsk(double rate, double h, double bt, double offset)
{
	double sps = FS / rate, phase = 0.0;
	int nsym = (int)(N / sps) + 8;
	static double nrz[N + 1024], f[N + 1024];

	for (int k = 0; k < N + 1024; k++) {
		int s = (int)(k / sps);

		nrz[k] = s < nsym && ((test_rng_u32() >> 7) & 1U) ? 1.0 : -1.0;
		if (k > 0 && (int)((k - 1) / sps) == s) {
			nrz[k] = nrz[k - 1];
		}
	}
	if (bt > 0.0) {
		double sigma = sqrt(log(2.0)) / (ESP_SDR_TAU * bt) * sps, sum = 0.0;
		int half = (int)(3.0 * sigma);

		for (int k = 0; k < N + 512; k++) {
			double a = 0.0;

			sum = 0.0;
			for (int j = -half; j <= half; j++) {
				double g = exp(-(double)j * j / (2.0 * sigma * sigma));

				a += g * nrz[k + half + j];
				sum += g;
			}
			f[k] = a / sum;
		}
	} else {
		memcpy(f, nrz, sizeof(f));
	}
	for (int k = 0; k < N; k++) {
		phase += ESP_SDR_TAU * (offset / FS) + ESP_SDR_TAU / 2.0 * h * f[k] / sps;
		si[k] = cos(phase);
		sq[k] = sin(phase);
	}
}

static void tones(double f1, double f2)
{
	double p1 = ESP_SDR_TAU * test_rng_uniform(), p2 = ESP_SDR_TAU * test_rng_uniform();

	for (int k = 0; k < N; k++) {
		si[k] = cos(p1 + ESP_SDR_TAU * f1 / FS * k) + cos(p2 + ESP_SDR_TAU * f2 / FS * k);
		sq[k] = sin(p1 + ESP_SDR_TAU * f1 / FS * k) + sin(p2 + ESP_SDR_TAU * f2 / FS * k);
	}
}

/* The verdict on words[] in one go; @p dci, @p dcq the DC the receiver reports. */
static bool judge(int32_t dci, int32_t dcq, uint32_t thr, bool inherit, struct sort_result *r)
{
	struct sort_acc a = {0};

	(void)sort_points(&a, words, SORT_LAG_C, N, UINT32_MAX, dci, dcq);
	return sort_decide(&a, (uint32_t)NOISE, thr, inherit, r);
}

/* The keep threshold of a ratio of 2 (3 dB) over the noise. */
#define THR ((uint32_t)(2.0 * NOISE))

ZTEST(gapless_sort, test_raw_fields)
{
	zassert_equal(sort_raw_i(word(-512, 0)), -512);
	zassert_equal(sort_raw_i(word(511, -1)), 511);
	zassert_equal(sort_raw_q(word(3, -512)), -512);
	zassert_equal(sort_raw_q(word(-3, 7)), 7);
	zassert_equal(sort_raw_i(word(-3, 7) | 0xfff00000U), -3, "bits above 19 ignored");
}

ZTEST(gapless_sort, test_wifi_wide)
{
	struct sort_result r;
	int wide = 0;

	test_rng_seed(1);
	for (int t = 0; t < 40; t++) {
		/* 17 MHz of band, anywhere in the 80, 6 to 30 dB over the noise. */
		double off = -30e6 + 60e6 * test_rng_uniform();

		band_noise(17e6, off);
		quantize(NOISE * pow(10.0, (6.0 + 24.0 * test_rng_uniform()) / 10.0), 0, 0);
		wide += judge(0, 0, THR, false, &r) ? 1 : 0;
		zassert_true(r.loud);
	}
	printk("band noise 17 MHz: %d of 40 wide\n", wide);
	zassert_true(wide >= 39, "%d of 40", wide);
}

ZTEST(gapless_sort, test_narrow_signals)
{
	struct sort_result r;

	test_rng_seed(2);
	for (int t = 0; t < 40; t++) {
		double off = -38e6 + 76e6 * test_rng_uniform();
		double snr = pow(10.0, (3.0 + 27.0 * test_rng_uniform()) / 10.0);

		/* Bluetooth LE 1M: GFSK BT 0.5, h 0.5. */
		fsk(1e6, 0.5, 0.5, off);
		quantize(NOISE * snr, 0, 0);
		zassert_false(judge(0, 0, THR, false, &r), "LE 1M at %.1f MHz: rho %u %u %u",
			      off / 1e6, r.rho[0], r.rho[1], r.rho[2]);
		/* LE 2M. */
		fsk(2e6, 0.5, 0.5, off);
		quantize(NOISE * snr, 0, 0);
		zassert_false(judge(0, 0, THR, false, &r), "LE 2M at %.1f MHz", off / 1e6);
		/* 802.15.4 O-QPSK with half-sine pulses: MSK at 2 Mchip/s. */
		fsk(2e6, 0.5, 0.0, off);
		quantize(NOISE * snr, 0, 0);
		zassert_false(judge(0, 0, THR, false, &r), "O-QPSK at %.1f MHz", off / 1e6);
	}
}

ZTEST(gapless_sort, test_two_tones_narrow)
{
	struct sort_result r;

	test_rng_seed(3);
	for (double df = 0.5e6; df < 40e6; df += 0.25e6) {
		double f1 = -20e6 + 10e6 * test_rng_uniform();

		tones(f1, f1 + df);
		quantize(NOISE * 100.0, 0, 0);
		zassert_false(judge(0, 0, THR, false, &r), "two tones %.2f MHz apart: rho %u %u %u",
			      df / 1e6, r.rho[0], r.rho[1], r.rho[2]);
	}
}

ZTEST(gapless_sort, test_quiet_inherits)
{
	struct sort_result r;

	test_rng_seed(4);
	memset(si, 0, sizeof(si));
	memset(sq, 0, sizeof(sq));
	quantize(0.0, 0, 0);
	zassert_true(judge(0, 0, THR, true, &r));
	zassert_false(r.loud);
	zassert_false(judge(0, 0, THR, false, &r));
	/* Before the baseline is learnt the threshold is the largest power: nothing is loud. */
	band_noise(17e6, 5e6);
	quantize(NOISE * 100.0, 0, 0);
	zassert_false(judge(0, 0, UINT32_MAX, false, &r));
	zassert_false(r.loud);
}

ZTEST(gapless_sort, test_dc_removed)
{
	struct sort_result r;

	test_rng_seed(5);
	/* A strong DC would correlate at every lag: given as the receiver's, it is taken out. */
	band_noise(17e6, -12e6);
	quantize(NOISE * 30.0, 40.0, -25.0);
	zassert_true(judge(40, -25, THR, false, &r));
	/* The receiver's estimate may lag: the chunk's own mean takes out what it left. */
	zassert_true(judge(0, 0, THR, false, &r), "the DC the estimate left: rho %u %u %u",
		     r.rho[0], r.rho[1], r.rho[2]);
	zassert_true(judge(30, -15, THR, false, &r));
	fsk(1e6, 0.5, 0.5, 9e6);
	quantize(NOISE * 30.0, 40.0, -25.0);
	zassert_false(judge(40, -25, THR, false, &r));
}

ZTEST(gapless_sort, test_mean_frequency)
{
	struct sort_result r;

	test_rng_seed(6);
	for (double off = -30e6; off <= 30e6; off += 7.5e6) {
		double ci = 0.0, cq = 0.0, f;

		/*
		 * One chunk is 10 us of a random 17 MHz band: its own spectrum's centre wanders by
		 * a megahertz or two. A burst's records averaged (the P4 does) pin it down.
		 */
		for (int c = 0; c < 16; c++) {
			band_noise(17e6, off);
			quantize(NOISE * 100.0, 0, 0);
			(void)judge(0, 0, THR, false, &r);
			f = atan2(r.r1_q, r.r1_i) / ESP_SDR_TAU * FS;
			zassert_within(f, off, 3.5e6, "at %.1f MHz: %.2f MHz", off / 1e6, f / 1e6);
			ci += r.r1_i / hypot(r.r1_i, r.r1_q);
			cq += r.r1_q / hypot(r.r1_i, r.r1_q);
		}
		f = atan2(cq, ci) / ESP_SDR_TAU * FS;
		zassert_within(f, off, 0.75e6, "at %.1f MHz, 16 chunks: %.2f MHz", off / 1e6,
			       f / 1e6);
	}
}

ZTEST(gapless_sort, test_pieces_equal_one_go)
{
	struct sort_acc one = {0}, part = {0};
	uint32_t pos = SORT_LAG_C;

	test_rng_seed(7);
	band_noise(17e6, 3e6);
	quantize(NOISE * 50.0, 3, -2);
	(void)sort_points(&one, words, SORT_LAG_C, N, UINT32_MAX, 3, -2);
	while (pos < N) {
		pos = sort_points(&part, words, pos, N, 7, 3, -2);
	}
	zassert_equal(one.points, (N - SORT_LAG_C + SORT_STRIDE - 1) / SORT_STRIDE);
	zassert_mem_equal(&one, &part, sizeof(one));
}

ZTEST(gapless_sort, test_full_scale_fits)
{
	struct sort_acc a = {0};
	struct sort_result r;

	/* Every point at full scale: the sums stay within 32 bits. */
	for (int k = 0; k < N; k++) {
		words[k] = word(-512, -512);
	}
	(void)sort_points(&a, words, SORT_LAG_C, N, UINT32_MAX, 0, 0);
	zassert_equal(a.r0, (int32_t)(2 * 512 * 512 * a.points));
	zassert_false(sort_decide(&a, (uint32_t)NOISE, THR, false, &r));
	zassert_equal(r.rho[0], 255);
}

ZTEST_SUITE(gapless_sort, NULL, NULL, NULL, NULL, NULL);
