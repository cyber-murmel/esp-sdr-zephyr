/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GFSK generator of the suite: Gaussian filtered frequency at 16 samples a symbol, every 4th
 * sample, the channel at DC as the ring delivers it, white noise. Static functions: each
 * test file that includes it draws from its own test_rng state (test_rng.h keeps one per file),
 * so a file's random sequence does not depend on the others.
 */

#ifndef UBT_DEMOD_TEST_GEN_H_
#define UBT_DEMOD_TEST_GEN_H_

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <test_rng.h>

#include <esp_sdr/esp_sdr_dsp.h>

#define HI_SPS   16   /* generator samples per symbol (16 MS/s) */
#define GAUSS_HW 32   /* half width of the Gaussian filter, generator samples (2 symbols) */
#define FS_HI    16.0e6
#define FULL     8192.0

struct burst {
	size_t start;   /* 4 MS/s index of the first symbol's start */
	const uint8_t *bits;
	size_t nsym;
	double h;       /* modulation index: 0.5 LE, 0.32 BR */
	double dbfs;    /* carrier level, full scale 8192 per axis */
	double cfo_hz;
};

static double gtaps[2 * GAUSS_HW + 1];

/* Gaussian frequency pulse filter of bandwidth-time product @p bt, unit sum. */
static void gauss_init(double bt)
{
	double sum = 0;

	for (int k = -GAUSS_HW; k <= GAUSS_HW; k++) {
		double t = (double)k / HI_SPS;
		double g = exp(-ESP_SDR_TAU * ESP_SDR_TAU / 2.0 * bt * bt * t * t / log(2.0));

		gtaps[k + GAUSS_HW] = g;
		sum += g;
	}
	for (int k = 0; k <= 2 * GAUSS_HW; k++) {
		gtaps[k] /= sum;
	}
}



/*
 * @p n samples at 4 MS/s: the bursts (symbol k of a burst centred at sample start + 4 k + 2) in
 * noise of @p noise_dbfs total power (-200: none).
 */
static void gen(int16_t *i, int16_t *q, size_t n, const struct burst *b, size_t nb,
		double noise_dbfs)
{
	double sigma = noise_dbfs > -199 ? FULL * pow(10.0, noise_dbfs / 20.0) / sqrt(2.0) : 0;

	for (size_t k = 0; k < n; k++) {
		i[k] = 0;
		q[k] = 0;
	}
	for (size_t u = 0; u < nb; u++) {
		const struct burst *x = &b[u];
		double amp = FULL * pow(10.0, x->dbfs / 20.0);
		double dev = x->h / 2.0 * 1.0e6;
		double phase = 0;
		size_t m0 = x->start * 4U;
		size_t m1 = m0 + x->nsym * HI_SPS;

		for (size_t m = m0 - GAUSS_HW; m < m1 + GAUSS_HW && m / 4U < n; m++) {
			double f = 0;

			for (int k = -GAUSS_HW; k <= GAUSS_HW; k++) {
				size_t mm = m - (size_t)k;

				if (mm >= m0 && mm < m1) {
					f += gtaps[k + GAUSS_HW] *
					     (x->bits[(mm - m0) / HI_SPS] ? 1.0 : -1.0);
				}
			}
			phase += ESP_SDR_TAU * (dev * f + x->cfo_hz) / FS_HI;
			if (m % 4U == 0U && m >= m0 && m < m1) {
				double vi = i[m / 4U] + amp * cos(phase);
				double vq = q[m / 4U] + amp * sin(phase);

				i[m / 4U] = (int16_t)lrint(vi);
				q[m / 4U] = (int16_t)lrint(vq);
			}
		}
	}
	for (size_t k = 0; k < n && sigma > 0; k++) {
		double vi = i[k] + sigma * test_rng_gauss();
		double vq = q[k] + sigma * test_rng_gauss();

		vi = vi > 32767 ? 32767 : (vi < -32768 ? -32768 : vi);
		vq = vq > 32767 ? 32767 : (vq < -32768 ? -32768 : vq);
		i[k] = (int16_t)lrint(vi);
		q[k] = (int16_t)lrint(vq);
	}
}

/* Bits of a BLE packet start: the preamble for @p aa, the AA, then random bits. */
static void ble_bits(uint8_t *bits, size_t n, uint32_t aa)
{
	for (size_t k = 0; k < n; k++) {
		if (k < 8U) {
			bits[k] = (uint8_t)((k & 1U) ^ (aa & 1U));
		} else if (k < 40U) {
			bits[k] = (uint8_t)((aa >> (k - 8U)) & 1U);
		} else {
			bits[k] = (uint8_t)(test_rng_u32() >> 31);
		}
	}
}

/* Wideband bursts (OFDM looks like Gaussian noise) of @p dbfs: @p on samples, @p off apart. */
static void add_bursts(int16_t *si, int16_t *sq, size_t n, double dbfs, size_t on, size_t off)
{
	double sigma = FULL * pow(10.0, dbfs / 20.0) / sqrt(2.0);

	for (size_t k = 0; k < n; k++) {
		if (k % (on + off) < on) {
			double vi = si[k] + sigma * test_rng_gauss();
			double vq = sq[k] + sigma * test_rng_gauss();

			si[k] = (int16_t)lrint(vi > 32767 ? 32767 : (vi < -32768 ? -32768 : vi));
			sq[k] = (int16_t)lrint(vq > 32767 ? 32767 : (vq < -32768 ? -32768 : vq));
		}
	}
}


#endif /* UBT_DEMOD_TEST_GEN_H_ */
