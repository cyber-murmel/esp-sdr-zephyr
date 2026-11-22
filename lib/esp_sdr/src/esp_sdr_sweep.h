/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Chirp synthesis for esp_sdr_tx_sweep(), header only so the unit test
 * (tests/unit/tx_sweep) builds the same code.
 */
#ifndef ESP_SDR_SWEEP_H_
#define ESP_SDR_SWEEP_H_

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include <esp_sdr/esp_sdr_dsp.h>
#include <esp_sdr/esp_sdr_tx.h>

/* Samples per rotation step: w is constant over a block (error dl * 4^2 / 8, below 3e-4 rad). */
#define SWEEP_BLK  4U
/* Samples between exact reseeds of w from double (bounds the float rounding of w). */
#define SWEEP_SEED 1024U
/* Samples between |z| renormalizations. */
#define SWEEP_NORM 128U

#ifndef ESP_SDR_SWEEP_TRACE
/* Test hook: sample j's z before it is quantized. */
#define ESP_SDR_SWEEP_TRACE(j, zr, zi)
#endif

/* cos, sin of th (|th| <= pi) to float accuracy: the rounding of th itself is carried in lo. */
static inline void sweep_sincos(double th, float *c, float *s)
{
	float hi = (float)th, lo = (float)(th - (double)hi);
	float ch = cosf(hi), sh = sinf(hi);

	*c = ch - sh * lo;
	*s = sh + ch * lo;
}

static inline int16_t sweep_round(float v)
{
	/* Inline rounding: lroundf() is a library call and dominated the loop. */
	return (int16_t)CLAMP((int)(v + (v < 0.0f ? -0.5f : 0.5f)), -511, 511);
}

/*
 * Fill bank[0..n) with a chirp starting at t (s): frequency f0 + k t. Phase
 * is taken from absolute time, so segments line up across the synthesis
 * gaps. Per sample a complex rotation z *= w. w follows the chirp as
 * w = wl * exp(j psi), wl reseeded from double every SWEEP_SEED samples and
 * exp(j psi) a short polynomial (psi stays below 0.2), so nothing
 * accumulates in w; z is renormalized (first-order Newton) every SWEEP_NORM.
 */
static inline void esp_sdr_sweep_fill(volatile uint32_t *bank, size_t n, double t, double fs,
				      double f0, double k, float amp)
{
	double fk = k / fs;
	double a0 = ESP_SDR_TAU * (f0 + k * t + 0.5 * fk * SWEEP_BLK) / fs;
	double a1 = ESP_SDR_TAU * fk / fs;
	float dpsi = (float)(a1 * SWEEP_BLK);
	double ph = fmod(ESP_SDR_TAU * (f0 * t + 0.5 * k * t * t), ESP_SDR_TAU);
	float zr, zi;
	size_t j = 0;

	sweep_sincos(ph > ESP_SDR_TAU / 2 ? ph - ESP_SDR_TAU : ph, &zr, &zi);

	while (j < n) {
		size_t seed_end = MIN(n, j + SWEEP_SEED);
		double th = a0 + a1 * (double)j;
		float wlr, wli, step = 0.0f;

		sweep_sincos(th > ESP_SDR_TAU / 2 ? th - ESP_SDR_TAU
				: th < -ESP_SDR_TAU / 2 ? th + ESP_SDR_TAU : th,
			     &wlr, &wli);
		while (j < seed_end) {
			size_t norm_end = MIN(seed_end, j + SWEEP_NORM);
			float g = 1.5f - 0.5f * (zr * zr + zi * zi);

			zr *= g;
			zi *= g;
			while (j < norm_end) {
				/* A product, not a running sum: the sum's rounding would add up. */
				float psi = step * dpsi, p2 = psi * psi;
				float c = 1.0f - p2 * (0.5f - p2 * (1.0f / 24 - p2 * (1.0f / 720)));
				float s = psi * (1.0f - p2 * (1.0f / 6 - p2 * (1.0f / 120 - p2 * (1.0f / 5040))));
				float wr = wlr * c - wli * s, wi = wlr * s + wli * c;

				step += 1.0f;
				for (unsigned int b = 0; b < SWEEP_BLK && j < norm_end; b++, j++) {
					float tr, vi = amp * zr, vq = amp * zi;

					ESP_SDR_SWEEP_TRACE(j, zr, zi);
					bank[j] = esp_sdr_tx_word(sweep_round(vi), sweep_round(vq));
					tr = zr * wr - zi * wi;
					zi = zr * wi + zi * wr;
					zr = tr;
				}
			}
		}
	}
}

#endif /* ESP_SDR_SWEEP_H_ */
