/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Reference tools for checking the GFSK modulator (gfsk_mod.c), shared by the unit test
 * and apps/ubertooth/tools/txcheck.c. They do not use the modulator's code or tables:
 * a double precision ideal GFSK model from the Gaussian filter's closed form, an FM
 * discriminator with integrate and dump (the independent receiver), and a spectrum helper.
 */

#ifndef TXREF_H_
#define TXREF_H_

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_sdr/esp_sdr_dsp.h>

/* DAC word fields (esp_sdr_tx_word(): I in bits 9:0, Q in bits 19:10). */
static inline int txref_i(uint32_t w)
{
	return (int)(w << 22) >> 22;
}

static inline int txref_q(uint32_t w)
{
	return (int)(w << 12) >> 22;
}

/* Bit i of a packed array. */
static inline int txref_bit(const uint8_t *bits, unsigned int i, int msb_first)
{
	return (bits[i / 8U] >> (msb_first ? 7U - i % 8U : i % 8U)) & 1;
}

/* Integrated frequency pulse of a Gaussian filter (BT) and a one symbol rectangle: 0 to 1, 0.5 at x
 * = 0 (T = 1). */
static inline double txref_pulse(double x, double bt)
{
	const double s = sqrt(log(2.0)) / (ESP_SDR_TAU * bt);
	double r = 0;

	for (int k = 0; k < 2; k++) {
		double y = x + (k == 0 ? 0.5 : -0.5);
		double v = y * 0.5 * erfc(-y / s / sqrt(2.0)) +
			   s * exp(-0.5 * y * y / (s * s)) / sqrt(ESP_SDR_TAU);

		r += k == 0 ? v : -v;
	}
	return r;
}

/* What follows the bits of an ideal burst. */
struct txref_burst {
	const uint8_t *bits;
	unsigned int nbits;
	int msb_first;
	/* Carrier symbols before the bits, and after: 0 = centre, 1 = hold the last bit. */
	unsigned int lead, trail;
	int hold;
	/* Periodic: bit -1 is the last, bit n the first. */
	int loop;
};

/* Symbol i as +1, -1 or 0. */
static inline int txref_sym(const struct txref_burst *b, long i)
{
	long d = i - (long)b->lead;

	if (b->loop) {
		long n = (long)b->nbits;

		i %= n;
		if (i < 0) {
			i += n;
		}
		return txref_bit(b->bits, (unsigned int)i, b->msb_first) ? 1 : -1;
	}
	if (d < 0) {
		return 0;
	}
	if (d >= (long)b->nbits) {
		return b->hold ? (txref_bit(b->bits, b->nbits - 1U, b->msb_first) ? 1 : -1) : 0;
	}
	return txref_bit(b->bits, (unsigned int)d, b->msb_first) ? 1 : -1;
}

/*
 * Phase in turns of the ideal GFSK signal at samples [0, n) (40 per symbol): the IF, plus dev /
 * 1 MHz times the sum over bits of b_j (pulse(t - j - 1/2) - [j < 0]), each pulse the integrated
 * Gaussian centred in its symbol. The step at j < 0 puts the origin before bit 0, which also fixes
 * the phase of a balanced loop pattern (its past nets to zero). Bits further than 6 symbols back
 * add their full step.
 */
static inline void txref_phase(const struct txref_burst *b, double dev_hz, double if_hz, double bt,
			       size_t n, double *phase)
{
	double base = 0;
	long s0 = 0;

	for (size_t k = 0; k < n; k++) {
		long s = (long)(k / 40U);
		double t = (double)k / 40.0, sum;

		for (; s0 < s - 6; s0++) {
			base += txref_sym(b, s0);
		}
		sum = base;
		for (long j = s - 6; j <= s + 6; j++) {
			sum += txref_sym(b, j) *
			       (txref_pulse(t - ((double)j + 0.5), bt) - (j < 0 ? 1.0 : 0.0));
		}
		phase[k] = if_hz * (double)k / 40e6 + dev_hz / 1e6 * sum;
	}
}

/* Instantaneous frequency in Hz at samples 1 to n - 1 (phase step of consecutive words at 40 MS/s);
 * f[0] = f[1]. */
static inline void txref_freq(const uint32_t *w, size_t n, double *f)
{
	for (size_t k = 1; k < n; k++) {
		double a = txref_i(w[k]), b = txref_q(w[k]), c = txref_i(w[k - 1]),
		       d = txref_q(w[k - 1]);

		f[k] = atan2(b * c - a * d, a * c + b * d) * 40e6 / (ESP_SDR_TAU);
	}
	f[0] = n > 1 ? f[1] : 0.0;
}

/*
 * Integrate and dump: symbols from sample @p first, 40 samples each, mean frequency minus @p if_hz
 * in
 * @p mean[0..nsym).
 */
static inline void txref_dump(const double *f, size_t first, size_t nsym, double if_hz,
			      double *mean)
{
	for (size_t s = 0; s < nsym; s++) {
		double acc = 0;

		for (size_t k = 0; k < 40; k++) {
			acc += f[first + 40U * s + k];
		}
		mean[s] = acc / 40.0 - if_hz;
	}
}

/* Complex FFT in place, n a power of two (decimation in time). */
static inline void txref_fft(double *re, double *im, size_t n)
{
	for (size_t i = 1, j = 0; i < n; i++) {
		size_t bit = n >> 1;

		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			double t = re[i];

			re[i] = re[j];
			re[j] = t;
			t = im[i];
			im[i] = im[j];
			im[j] = t;
		}
	}
	for (size_t len = 2; len <= n; len <<= 1) {
		double ang = -ESP_SDR_TAU / (double)len;
		double wr = cos(ang), wi = sin(ang);

		for (size_t i = 0; i < n; i += len) {
			double cr = 1.0, ci = 0.0;

			for (size_t k = 0; k < len / 2; k++) {
				size_t a = i + k, b = i + k + len / 2;
				double tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;

				re[b] = re[a] - tr;
				im[b] = im[a] - ti;
				re[a] += tr;
				im[a] += ti;
				{
					double nr = cr * wr - ci * wi;

					ci = cr * wi + ci * wr;
					cr = nr;
				}
			}
		}
	}
}

/*
 * Power spectrum of DAC words (Blackman-Harris window, zero padded to @p nfft): p[k] for the bin at
 * frequency k fs / nfft (k >= nfft / 2 negative). re and im are scratch of nfft doubles.
 */
static inline void txref_psd(const uint32_t *w, size_t n, size_t nfft, double *re, double *im,
			     double *p)
{
	for (size_t k = 0; k < nfft; k++) {
		re[k] = 0;
		im[k] = 0;
	}
	for (size_t k = 0; k < n; k++) {
		double x = ESP_SDR_TAU * (double)k / (double)(n - 1);
		double win =
			0.35875 - 0.48829 * cos(x) + 0.14128 * cos(2 * x) - 0.01168 * cos(3 * x);

		re[k] = txref_i(w[k]) * win;
		im[k] = txref_q(w[k]) * win;
	}
	txref_fft(re, im, nfft);
	for (size_t k = 0; k < nfft; k++) {
		p[k] = re[k] * re[k] + im[k] * im[k];
	}
}

/* Sum of p over frequencies [f0, f1) Hz at 40 MS/s. */
static inline double txref_band(const double *p, size_t nfft, double f0, double f1)
{
	double sum = 0;

	for (size_t k = 0; k < nfft; k++) {
		double f =
			(k < nfft / 2 ? (double)k : (double)k - (double)nfft) * 40e6 / (double)nfft;

		if (f >= f0 && f < f1) {
			sum += p[k];
		}
	}
	return sum;
}

#endif /* TXREF_H_ */
