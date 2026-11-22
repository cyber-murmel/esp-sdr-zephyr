/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit test of esp_sdr_sweep_fill() (lib/esp_sdr/src/esp_sdr_sweep.h). The oscillator state z is
 * traced before quantization and compared with the exact double phase over full 16380-sample
 * banks of the fastest and slowest sweeps the API allows. old_fill() is the float recursion the
 * library used before (no renormalization, w started a chirp half-step short); it documents the
 * drift and gives the reference for the words of short sweeps. Runs on native_sim.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <esp_sdr/esp_sdr_dsp.h>

static double tr_amp, tr_ph;
static double tr_t, tr_fs, tr_f0, tr_k;
static bool tr_on;

static void trace(size_t j, float zr, float zi)
{
	double tj, ph, a, d;

	if (!tr_on) {
		return;
	}
	tj = tr_t + (double)j / tr_fs;
	ph = ESP_SDR_TAU * (tr_f0 * tj + 0.5 * tr_k * tj * tj);
	a = fabs(sqrt((double)zr * (double)zr + (double)zi * (double)zi) - 1.0);
	d = atan2((double)zi, (double)zr) - ph;
	d -= ESP_SDR_TAU * round(d / ESP_SDR_TAU);
	tr_amp = MAX(tr_amp, a);
	tr_ph = MAX(tr_ph, fabs(d));
}

#define ESP_SDR_SWEEP_TRACE(j, zr, zi) trace(j, zr, zi)
#include "esp_sdr_sweep.h"

/* The pre-fix implementation, verbatim apart from the trace hook. */
static void old_fill(volatile uint32_t *bank, size_t n, double t, double fs, double f0, double k,
		     float amp)
{
	double phase = fmod(ESP_SDR_TAU * (f0 * t + 0.5 * k * t * t), ESP_SDR_TAU);
	double f = f0 + k * t;
	float zr = cosf((float)phase), zi = sinf((float)phase);
	float wr = cosf((float)(ESP_SDR_TAU * f / fs)), wi = sinf((float)(ESP_SDR_TAU * f / fs));
	float cr = cosf((float)(ESP_SDR_TAU * k / (fs * fs)));
	float ci = sinf((float)(ESP_SDR_TAU * k / (fs * fs)));

	for (size_t j = 0; j < n; j++) {
		float tr;
		float vi = amp * zr, vq = amp * zi;

		trace(j, zr, zi);
		bank[j] = esp_sdr_tx_word(
			(int16_t)CLAMP((int)(vi + (vi < 0.0f ? -0.5f : 0.5f)), -511, 511),
			(int16_t)CLAMP((int)(vq + (vq < 0.0f ? -0.5f : 0.5f)), -511, 511));
		tr = zr * wr - zi * wi;
		zi = zr * wi + zi * wr;
		zr = tr;
		tr = wr * cr - wi * ci;
		wi = wr * ci + wi * cr;
		wr = tr;
	}
}

#define NMAXS 16380U
static uint32_t bank_new[NMAXS], bank_old[NMAXS];

struct sweep {
	const char *name;
	double fs, f0, f1, T;
};

/* Fastest: nearly the whole Nyquist band in the shortest time (1 ms); slowest: 1 Hz in 5 s. */
static const struct sweep sweeps[] = {
	{"80 MS/s +-39.9 MHz 1 ms", 80e6, -39.9e6, 39.9e6, 1e-3},
	{"40 MS/s +-19.9 MHz 1 ms", 40e6, -19.9e6, 19.9e6, 1e-3},
	{"40 MS/s +-15 MHz 10 ms", 40e6, -15e6, 15e6, 10e-3},
	{"80 MS/s +-30 MHz 100 ms", 80e6, -30e6, 30e6, 100e-3},
	{"80 MS/s +-20 MHz 4 s", 80e6, -20e6, 20e6, 4.0},
	{"80 MS/s 0 to 1 Hz 5 s", 80e6, 0.0, 1.0, 5.0},
	{"40 MS/s 1 MHz + 1 kHz 5 s", 40e6, 1e6, 1.001e6, 5.0},
	{"40 MS/s constant 3 MHz", 40e6, 3e6, 3e6, 0.1},
};
#define NBANKS 12

static double bank_t(const struct sweep *s, int b)
{
	double room = s->T - (double)NMAXS / s->fs;

	return room > 0.0 ? room * b / (NBANKS - 1) : 0.0;
}

/* Worst amplitude and phase error over every sweep and bank, for the old or the new fill. */
static void worst(bool old, double *amp, double *ph, const char **where)
{
	*amp = 0.0;
	*ph = 0.0;
	for (size_t i = 0; i < ARRAY_SIZE(sweeps); i++) {
		const struct sweep *s = &sweeps[i];
		double k = (s->f1 - s->f0) / s->T;

		double sa = 0.0, sp = 0.0;

		for (int b = 0; b < NBANKS; b++) {
			tr_t = bank_t(s, b);
			tr_fs = s->fs;
			tr_f0 = s->f0;
			tr_k = k;
			tr_amp = 0.0;
			tr_ph = 0.0;
			tr_on = true;
			if (old) {
				old_fill(bank_old, NMAXS, tr_t, s->fs, s->f0, k, 511.0f);
			} else {
				esp_sdr_sweep_fill(bank_new, NMAXS, tr_t, s->fs, s->f0, k, 511.0f);
			}
			tr_on = false;
			if (tr_amp > *amp || tr_ph > *ph) {
				*where = s->name;
			}
			*amp = MAX(*amp, tr_amp);
			*ph = MAX(*ph, tr_ph);
			sa = MAX(sa, tr_amp);
			sp = MAX(sp, tr_ph);
		}
		printf("  %s %-28s amplitude %.2e, phase %.2e rad\n", old ? "old" : "new", s->name, sa,
		       sp);
	}
}

ZTEST(tx_sweep, test_old_drifts)
{
	double amp, ph;
	const char *where = "";

	worst(true, &amp, &ph, &where);
	printf("old: worst amplitude error %.3e, phase error %.3e rad (%s)\n", amp, ph, where);
	zassert_true(amp > 1e-2 && ph > 0.1, "the reference no longer shows the drift");
}

ZTEST(tx_sweep, test_new_bounds)
{
	double amp, ph;
	const char *where = "";

	worst(false, &amp, &ph, &where);
	printf("new: worst amplitude error %.3e, phase error %.3e rad (%s)\n", amp, ph, where);
	zassert_true(amp < 1e-4, "amplitude %e", amp);
	zassert_true(ph < 1e-3, "phase %e", ph);
}

static int w_i(uint32_t w)
{
	return ((int)(w & 0x3ffU) ^ 0x200) - 0x200;
}

static int w_q(uint32_t w)
{
	return ((int)((w >> 10) & 0x3ffU) ^ 0x200) - 0x200;
}

/* Short banks, where the old recursion has not drifted yet: the words agree to a LSB. */
ZTEST(tx_sweep, test_words_match_old)
{
	int worst_d = 0;
	static const size_t lens[] = {256, 300, 512, 1000, 1024, 1500};

	for (size_t i = 0; i < ARRAY_SIZE(sweeps); i++) {
		const struct sweep *s = &sweeps[i];
		double k = (s->f1 - s->f0) / s->T;

		for (size_t l = 0; l < ARRAY_SIZE(lens); l++) {
			for (int b = 0; b < 4; b++) {
				double t = b * s->T / 8.0;

				/* Skip where even the old one is off by more than rounding. */
				if (fabs(k) * lens[l] * lens[l] / (s->fs * s->fs) * ESP_SDR_TAU > 1e-4) {
					continue;
				}
				esp_sdr_sweep_fill(bank_new, lens[l], t, s->fs, s->f0, k, 511.0f);
				old_fill(bank_old, lens[l], t, s->fs, s->f0, k, 511.0f);
				for (size_t j = 0; j < lens[l]; j++) {
					worst_d = MAX(worst_d, abs(w_i(bank_new[j]) - w_i(bank_old[j])));
					worst_d = MAX(worst_d, abs(w_q(bank_new[j]) - w_q(bank_old[j])));
				}
			}
		}
	}
	printf("short banks: max word difference %d LSB\n", worst_d);
	zassert_true(worst_d <= 1, "%d LSB", worst_d);
}

/* Full banks against the rounded exact phase: phase error times 511 plus rounding. */
ZTEST(tx_sweep, test_words_vs_reference)
{
	int worst_d = 0;

	for (size_t i = 0; i < ARRAY_SIZE(sweeps); i++) {
		const struct sweep *s = &sweeps[i];
		double k = (s->f1 - s->f0) / s->T;

		for (int b = 0; b < NBANKS; b += 3) {
			double t = bank_t(s, b);

			esp_sdr_sweep_fill(bank_new, NMAXS, t, s->fs, s->f0, k, 511.0f);
			for (size_t j = 0; j < NMAXS; j++) {
				double tj = t + (double)j / s->fs;
				double ph = ESP_SDR_TAU * (s->f0 * tj + 0.5 * k * tj * tj);

				worst_d = MAX(worst_d, abs(w_i(bank_new[j]) -
							    (int)lround(511.0 * cos(ph))));
				worst_d = MAX(worst_d, abs(w_q(bank_new[j]) -
							    (int)lround(511.0 * sin(ph))));
			}
		}
	}
	printf("full banks: max word difference to the exact phase %d LSB\n", worst_d);
	zassert_true(worst_d <= 2, "%d LSB", worst_d);
}

/* A shorter bank is a prefix of a longer one, whatever the length modulo the step sizes. */
ZTEST(tx_sweep, test_odd_lengths)
{
	static const size_t lens[] = {256, 257, 258, 259, 1023, 1025, 1153, 16379};

	esp_sdr_sweep_fill(bank_old, NMAXS, 0.0, 40e6, 1e6, 2e9, 511.0f);
	for (size_t l = 0; l < ARRAY_SIZE(lens); l++) {
		esp_sdr_sweep_fill(bank_new, lens[l], 0.0, 40e6, 1e6, 2e9, 511.0f);
		zassert_equal(0, memcmp(bank_new, bank_old, lens[l] * sizeof(uint32_t)));
	}
}

ZTEST_SUITE(tx_sweep, NULL, NULL, NULL, NULL, NULL);
