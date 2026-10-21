/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The channelizer's prototype low pass (esdr_pfb.h).
 */

#include <math.h>

#include <esp_sdr/esp_sdr_dsp.h>

#include "esdr_pfb.h"

void esdr_pfb_proto(float *h, unsigned int m, unsigned int t, float stop_db, float gain)
{
	const unsigned int l = m * t;
	const float beta = esp_sdr_kaiser_beta(stop_db), i0b = esp_sdr_bessel_i0f(beta);
	const float mid = 0.5f * (float)(l - 1U), span = 2.0f / (float)(l - 1U);
	double sum = 0.0;

	/* One half, mirrored: L is even, so the offset from the centre is never 0. */
	for (unsigned int n = 0; n < l / 2U; n++) {
		float off = (float)n - mid, a = (ESP_SDR_TAU_F / 2.0f) * off / (float)m;
		float r = off * span;

		h[n] = sinf(a) / a * esp_sdr_bessel_i0f(beta * sqrtf(fmaxf(1.0f - r * r, 0.0f))) /
		       i0b;
		h[l - 1U - n] = h[n];
		sum += 2.0 * (double)h[n];
	}
	for (unsigned int n = 0; n < l; n++) {
		h[n] = (float)((double)h[n] * (double)gain / sum);
	}
}
