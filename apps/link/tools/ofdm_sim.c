/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host simulator for ofdm.c. Checks the FFT against a direct DFT, then loops
 * a frame into a capture window at a random offset (as the DAC plays it),
 * through carrier offset, optional multipath, noise and 10 bit quantization,
 * and decodes it; or decodes a real capture (link_perf.py dump).
 *
 *   cc -O2 -Wall -Isrc src/ofdm.c tools/ofdm_sim.c -lm -o /tmp/ofdm_sim
 *   BW=16 CH=52 MOD=qpsk /tmp/ofdm_sim [capture.bin]
 *
 * Environment: BW (MHz), CH, CPDIV, MOD (bpsk|qpsk), AMP, SYMS, CFO (Hz),
 * MP (1: multipath), GAIN (receive scale), RXDIV (1, 2: receive at 40 MS/s),
 * TRIALS. SNR is over the receive bandwidth.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ofdm.h"

#define FS     80000000U
#define WIN    16380U
#define TWO_PI 6.28318530717958647692

static uint32_t frame[WIN], win[WIN];
static uint8_t bits[WIN / 4], got[WIN / 4];
static struct ofdm_ctx ctx __attribute__((aligned(16)));
static struct ofdm_ctx ref __attribute__((aligned(16)));

static double env_f(const char *k, double d)
{
	const char *v = getenv(k);

	return v ? atof(v) : d;
}

static double randn(void)
{
	double u1 = (rand() + 1.0) / ((double)RAND_MAX + 2.0), u2 = rand() / (double)RAND_MAX;

	return sqrt(-2.0 * log(u1)) * cos(TWO_PI * u2);
}

static unsigned int bit_at(const uint8_t *b, size_t i)
{
	return (b[i >> 3] >> (7U - (i & 7U))) & 1U;
}

/* The FFT against a direct DFT: a build and decode round trip could hide an error both share. */
static int check_fft(void)
{
	unsigned int n = ctx.nfft;
	struct ofdm_cf x[OFDM_NFFT_MAX];
	double worst = 0.0;

	for (unsigned int i = 0; i < n; i++) {
		x[i] = (struct ofdm_cf){(float)randn(), (float)randn()};
	}
	ref = ctx;
	memcpy(ref.buf, x, n * sizeof(x[0]));
	ofdm_test_fft(&ref);
	for (unsigned int k = 0; k < n; k++) {
		double re = 0.0, im = 0.0;

		for (unsigned int t = 0; t < n; t++) {
			double a = -TWO_PI * (double)k * (double)t / (double)n;

			re += x[t].re * cos(a) - x[t].im * sin(a);
			im += x[t].re * sin(a) + x[t].im * cos(a);
		}
		{
			struct ofdm_cf y = ref.buf[ref.rev[k]];
			double e = hypot(y.re - re, y.im - im);

			worst = e > worst ? e : worst;
		}
	}
	printf("fft %u vs DFT: max error %.2e (of ~%.0f)\n", n, worst, sqrt((double)n));
	return worst < 1e-3 * sqrt((double)n) ? 0 : -1;
}

/*
 * Loop the frame into the window from @p off: multipath and carrier offset at
 * the transmit rate, decimation by rx_div (a windowed sinc low pass, as the
 * radio's receive filter), then noise and 10 bits at the receive rate.
 */
static void channel(size_t len, unsigned int off, double snr_db, double cfo_hz, int mp,
		    double gain, unsigned int rx_div, unsigned int *clipped)
{
	enum { TAPS = 31 };
	static const double taps_mp[3][2] = {{1.0, 0.0}, {0.25, -0.1}, {0.0, 0.08}};
	static double fr[2 * WIN + TAPS], fq[2 * WIN + TAPS];
	double h[TAPS], p = 0.0, ns, ph = TWO_PI * cfo_hz / FS, hs = 0.0;
	unsigned int nfull = rx_div * WIN + TAPS;
	int ntaps = mp ? 3 : 1;

	/* Low pass at 0.45 of the receive rate (Hann window), unity gain. */
	for (int k = 0; k < TAPS; k++) {
		double x = k - (TAPS - 1) / 2.0, fc = 0.45 / rx_div;

		h[k] = (x == 0.0 ? 2.0 * fc : sin(TWO_PI * fc * x) / (M_PI * x)) *
		       (0.5 - 0.5 * cos(TWO_PI * k / (TAPS - 1)));
		hs += h[k];
	}
	for (int k = 0; k < TAPS; k++) {
		h[k] /= hs;
	}
	for (unsigned int i = 0; i < nfull; i++) {
		double yi = 0.0, yq = 0.0, c = cos(ph * i), s = sin(ph * i);

		for (int t = 0; t < ntaps; t++) {
			uint32_t f = frame[(i + off + len - (unsigned int)t) % len];
			double xi = (int32_t)(f << 22) >> 22, xq = (int32_t)(f << 12) >> 22;
			double hr = taps_mp[t][0], hi = taps_mp[t][1];

			yi += xi * hr - xq * hi;
			yq += xi * hi + xq * hr;
		}
		fr[i] = gain * (yi * c - yq * s);
		fq[i] = gain * (yi * s + yq * c);
	}
	for (unsigned int i = 0; i < WIN; i++) {
		double ri = 0.0, rq = 0.0;

		if (rx_div == 1) {
			ri = fr[i];
			rq = fq[i];
		} else {
			for (int k = 0; k < TAPS; k++) {
				ri += h[k] * fr[i * rx_div + k];
				rq += h[k] * fq[i * rx_div + k];
			}
		}
		fr[i] = ri;
		fq[i] = rq;
		p += ri * ri + rq * rq;
	}
	p /= WIN;
	ns = sqrt(p / pow(10.0, snr_db / 10.0) / 2.0);
	*clipped = 0;
	for (unsigned int i = 0; i < WIN; i++) {
		int qi = (int)lrint(fr[i] + ns * randn() + 3.0), qq = (int)lrint(fq[i] + ns * randn() - 2.0);

		if (qi > 511 || qi < -512 || qq > 511 || qq < -512) {
			(*clipped)++;
		}
		qi = qi > 511 ? 511 : (qi < -512 ? -512 : qi);
		qq = qq > 511 ? 511 : (qq < -512 ? -512 : qq);
		win[i] = (((uint32_t)qi & 0x3ffU) << 10) | ((uint32_t)qq & 0x3ffU);
	}
}

int main(int argc, char **argv)
{
	const char *mod = getenv("MOD");
	struct ofdm_cfg cfg = {
		.sample_rate_hz = FS,
		.bandwidth_hz = (uint32_t)(env_f("BW", 16.0) * 1e6),
		.channels = (unsigned int)env_f("CH", 52),
		.cp_div = (unsigned int)env_f("CPDIV", 8),
		.mod = mod && strcmp(mod, "bpsk") == 0 ? OFDM_BPSK : OFDM_QPSK,
		.amp = (int)env_f("AMP", 120),
		.symbols = (unsigned int)env_f("SYMS", 0),
		.max_samples = (WIN + 1U) / 2U,
		.rx_div = (unsigned int)env_f("RXDIV", 1),
	};
	size_t nbits;

	srand(1);
	if (ofdm_init(&ctx, &cfg) != 0) {
		printf("no layout for these settings\n");
		return 1;
	}
	nbits = ofdm_frame_bits(&ctx);
	printf("rx fft %u at %u MS/s; nfft %u, spacing %.1f kHz, %u subcarriers = %.2f MHz, cp %u, %u data symbols, "
	       "%zu samples, %zu bits = %.1f Mbit/s on air\n",
	       ctx.rnfft, FS / cfg.rx_div / 1000000U, ctx.nfft, ctx.spacing_hz / 1e3, cfg.channels, ctx.bw_hz / 1e6, ctx.cp, ctx.ndata,
	       ctx.len, nbits, (double)nbits / ((double)ctx.len / FS) / 1e6);
	if (check_fft() != 0) {
		return 1;
	}
	ofdm_test_bits(bits, nbits, 1);

	if (argc > 1) {
		FILE *f = fopen(argv[1], "rb");
		size_t n = f ? fread(win, 4, WIN, f) : 0;
		struct ofdm_rx_info info;
		int ret, errs = 0;

		if (f) {
			fclose(f);
		}
		ret = ofdm_rx_decode(&ctx, win, n, got, &info);
		for (size_t i = 0; ret == 0 && i < nbits; i++) {
			errs += bit_at(bits, i) != bit_at(got, i);
		}
		printf("%s: ret %d start %d metric %.3f cfo %.1f kHz MER %.1f dB errors %d/%zu\n",
		       argv[1], ret, info.start, info.metric, info.cfo_hz / 1e3, info.mer_db, errs,
		       nbits);
		return 0;
	}

	(void)ofdm_tx_build(&ctx, bits, frame);
	printf("tx clipped %u of %zu samples\n", ctx.clipped, ctx.len);
	{
		const double snrs[] = {6, 9, 12, 15, 20, 25, 30};
		int trials = (int)env_f("TRIALS", 20), mp = (int)env_f("MP", 0);
		double cfo = env_f("CFO", 20000.0), gain = env_f("GAIN", 0.5);

		for (size_t s = 0; s < sizeof(snrs) / sizeof(snrs[0]); s++) {
			int ok = 0;
			long errs = 0;
			double mer = 0.0, cerr = 0.0, us = 0.0;
			unsigned int clipped = 0;

			for (int t = 0; t < trials; t++) {
				struct ofdm_rx_info info;
				unsigned int off = (unsigned int)rand() % ctx.len;
				double c = cfo * (0.5 + rand() / (double)RAND_MAX);
				clock_t c0;
				int ret;

				channel(ctx.len, off, snrs[s], c, mp, gain, cfg.rx_div, &clipped);
				c0 = clock();
				ret = ofdm_rx_decode(&ctx, win, WIN, got, &info);
				us += (double)(clock() - c0) * 1e6 / CLOCKS_PER_SEC;
				if (ret != 0) {
					errs += (long)nbits;
					continue;
				}
				ok++;
				mer += info.mer_db;
				cerr += fabs(info.cfo_hz - c);
				for (size_t i = 0; i < nbits; i++) {
					errs += bit_at(bits, i) != bit_at(got, i);
				}
			}
			printf("snr %4.0f dB: found %2d/%d, BER %.2e, MER %5.1f dB, |cfo err| %6.0f Hz, "
			       "%5.0f us, rx clipped %u\n",
			       snrs[s], ok, trials, (double)errs / ((double)nbits * trials),
			       ok ? mer / ok : 0.0, ok ? cerr / ok : 0.0, us / trials, clipped);
		}
	}
	return 0;
}
