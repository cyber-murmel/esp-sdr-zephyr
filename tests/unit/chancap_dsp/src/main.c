/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit test of lib/esp_sdr/src/esp_sdr_chancap_dsp.h: the polyphase filter bank and the DDC
 * against a direct floating point model (mix, FIR, decimate), tone placement and rejection per
 * channel, and the detector's floor, threshold and hang behaviour.
 */
#include <math.h>
#include <stdlib.h>

#include <zephyr/ztest.h>

#include "esp_sdr_chancap_dsp.h"

#define FS      16000000.0
#define N_IN    4096U
#define TONE_A  12000.0

static int16_t xi[N_IN], xq[N_IN];

static void tone(double hz, double amp, double phase0)
{
	for (unsigned int n = 0; n < N_IN; n++) {
		double ph = phase0 + ESP_SDR_TAU * hz * (double)n / FS;

		xi[n] = (int16_t)lround(amp * cos(ph));
		xq[n] = (int16_t)lround(amp * sin(ph));
	}
}

static void noise(double amp, uint32_t seed)
{
	uint32_t x = seed;

	for (unsigned int n = 0; n < N_IN; n++) {
		x = x * 1664525U + 1013904223U;
		xi[n] = (int16_t)((double)((int32_t)(x >> 8) - 8388608) / 8388608.0 * amp);
		x = x * 1664525U + 1013904223U;
		xq[n] = (int16_t)((double)((int32_t)(x >> 8) - 8388608) / 8388608.0 * amp);
	}
}

/* Channel c's centre, in multiples of the spacing above the LO. */
static double centre(const struct chancap_pfb *p, unsigned int c)
{
	return (double)c - (double)(p->m / 2U) + (p->odd ? 0.5 : 0.0);
}

/*
 * Direct model of channel c of the bank: mix by minus its centre, the prototype FIR (float
 * taps from the same design), output j at input pair n = j d + m - 1.
 */
static void direct_channel(const struct chancap_pfb *p, unsigned int c, unsigned int j,
			   double *yi, double *yq)
{
	int16_t h[CHANCAP_L_MAX];
	double f = centre(p, c) * FS / (double)p->m, si = 0.0, sq = 0.0;
	unsigned int n = j * p->d + p->m - 1U;

	chancap_design_lowpass(h, p->l, 0.05f, chancap_kaiser_beta(50.0f));
	for (unsigned int t = 0; t < p->l; t++) {
		unsigned int at = n - t;
		double ph = -ESP_SDR_TAU * f * (double)at / FS;
		double vi = xi[at] * cos(ph) - xq[at] * sin(ph);
		double vq = xi[at] * sin(ph) + xq[at] * cos(ph);

		si += (double)h[t] / CHANCAP_H_ONE * vi;
		sq += (double)h[t] / CHANCAP_H_ONE * vq;
	}
	*yi = si;
	*yq = sq;
}

static void check_against_direct(unsigned int m, unsigned int d, bool odd, double tol)
{
	static struct chancap_pfb p;
	int16_t oi[CHANCAP_M_MAX], oq[CHANCAP_M_MAX];
	double worst = 0.0;

	zassert_true(chancap_pfb_init(&p, m, d, 8, odd, 0.05f, 50.0f), "init");
	noise(8000.0, 7U + m + (odd ? 1U : 0U));
	/* Frames whose window lies inside the input. */
	unsigned int j0 = (p.l + d - 1U) / d, j1 = (N_IN - m) / d;

	p.frames = j0;
	for (unsigned int j = j0; j < j1; j++) {
		unsigned int n = j * d + m - 1U;

		chancap_pfb_frame(&p, &xi[n - p.l + 1U], &xq[n - p.l + 1U], oi, oq);
		for (unsigned int c = 0; c < m; c++) {
			double yi, yq;

			direct_channel(&p, c, j, &yi, &yq);
			worst = fmax(worst, fmax(fabs(yi - oi[c]), fabs(yq - oq[c])));
		}
	}
	printk("pfb m %u d %u odd %d: worst error %.1f LSB over %u frames\n", m, d, odd, worst,
	       j1 - j0);
	zassert_true(worst <= tol, "worst error %.1f > %.1f", worst, tol);
}

ZTEST(chancap_dsp, test_pfb_matches_direct_model)
{
	check_against_direct(8, 8, false, 16.0);
	check_against_direct(8, 8, true, 16.0);
	check_against_direct(8, 4, false, 16.0);
	check_against_direct(8, 4, true, 16.0);
	check_against_direct(16, 16, true, 24.0);
}

ZTEST(chancap_dsp, test_design_dc_gain)
{
	int16_t h[CHANCAP_L_MAX];
	int32_t sum = 0;

	chancap_design_lowpass(h, 64, 0.05f, chancap_kaiser_beta(50.0f));
	for (unsigned int n = 0; n < 64; n++) {
		sum += h[n];
		/* The rounding fix-up lands on one of the two centre taps. */
		zassert_within(h[n], h[63 - n], 1, "symmetric");
	}
	zassert_equal(sum, CHANCAP_H_ONE, "dc gain %d", sum);
}

/* Mean power per channel over the frames [j0, j1), and the mean phase step of channel c. */
static void run_tone(struct chancap_pfb *p, double pw[CHANCAP_M_MAX], unsigned int watch,
		     double *step)
{
	int16_t oi[CHANCAP_M_MAX], oq[CHANCAP_M_MAX];
	unsigned int j0 = (p->l + p->d - 1U) / p->d, j1 = (N_IN - p->m) / p->d;
	double cs = 0.0, sn = 0.0;
	int32_t li = 0, lq = 0;

	memset(pw, 0, sizeof(double) * CHANCAP_M_MAX);
	p->frames = j0;
	for (unsigned int j = j0; j < j1; j++) {
		unsigned int n = j * p->d + p->m - 1U;

		chancap_pfb_frame(p, &xi[n - p->l + 1U], &xq[n - p->l + 1U], oi, oq);
		for (unsigned int c = 0; c < p->m; c++) {
			pw[c] += (double)oi[c] * oi[c] + (double)oq[c] * oq[c];
		}
		if (j > j0) {
			/* y[j] conj(y[j - 1]) */
			cs += (double)oi[watch] * li + (double)oq[watch] * lq;
			sn += (double)oq[watch] * li - (double)oi[watch] * lq;
		}
		li = oi[watch];
		lq = oq[watch];
	}
	for (unsigned int c = 0; c < p->m; c++) {
		pw[c] /= (double)(j1 - j0);
	}
	*step = atan2(sn, cs);
}

static void check_tones(unsigned int m, unsigned int d, bool odd)
{
	static struct chancap_pfb p;
	double pw[CHANCAP_M_MAX], step, ref = TONE_A * TONE_A;
	double spacing = FS / (double)m, fout = FS / (double)d;

	zassert_true(chancap_pfb_init(&p, m, d, 8, odd, 0.05f, 50.0f), "init");
	for (unsigned int c = 0; c < m; c++) {
		double off = 300000.0 * ((c & 1U) ? 1.0 : -1.0);

		tone(centre(&p, c) * spacing + off, TONE_A, 0.3 * c);
		run_tone(&p, pw, c, &step);
		zassert_within(10.0 * log10(pw[c] / ref), 0.0, 1.0, "channel %u: %.1f dB", c,
			       10.0 * log10(pw[c] / ref));
		for (unsigned int o = 0; o < m; o++) {
			if (o != c) {
				double rel = 10.0 * log10(pw[o] / pw[c]);

				zassert_true(rel < -40.0, "channel %u leaks %.1f dB into %u", c,
					     rel, o);
			}
		}
		/* The offset tone sits at off in the channel's output. */
		zassert_within(step * fout / ESP_SDR_TAU, off, 2000.0,
			       "channel %u: %.0f Hz", c, step * fout / ESP_SDR_TAU);
	}
	printk("pfb m %u d %u odd %d: every channel within 1 dB, others below -40 dB\n", m, d, odd);
}

ZTEST(chancap_dsp, test_pfb_tones)
{
	check_tones(8, 8, false);
	check_tones(8, 8, true);
	check_tones(8, 4, true);
}

ZTEST(chancap_dsp, test_pfb_init_rejects)
{
	static struct chancap_pfb p;

	zassert_false(chancap_pfb_init(&p, 6, 6, 8, false, 0.05f, 50.0f), "m not a power of 2");
	zassert_false(chancap_pfb_init(&p, 8, 2, 8, false, 0.05f, 50.0f), "d");
	zassert_false(chancap_pfb_init(&p, 8, 8, 0, false, 0.05f, 50.0f), "k");
	zassert_false(chancap_pfb_init(&p, 32, 32, 8, false, 0.05f, 50.0f), "m too big");
	zassert_false(chancap_pfb_init(&p, 8, 8, 8, false, 0.5f, 50.0f), "fc");
}

/* ---- DDC ---- */

static void ddc_direct(const struct chancap_ddc *c, int num, unsigned int den, unsigned int j,
		       double *yi, double *yq)
{
	int16_t h[CHANCAP_DDC_L];
	double f = (double)num / (double)den * FS, si = 0.0, sq = 0.0;
	unsigned int n = j * c->d + c->d - 1U;

	chancap_design_lowpass(h, CHANCAP_DDC_L, 0.075f, chancap_kaiser_beta(40.0f));
	for (unsigned int t = 0; t < CHANCAP_DDC_L; t++) {
		unsigned int at = n - t;
		double ph = -ESP_SDR_TAU * f * (double)at / FS;
		double vi = xi[at] * cos(ph) - xq[at] * sin(ph);
		double vq = xi[at] * sin(ph) + xq[at] * cos(ph);

		si += (double)h[t] / CHANCAP_H_ONE * vi;
		sq += (double)h[t] / CHANCAP_H_ONE * vq;
	}
	*yi = si;
	*yq = sq;
}

ZTEST(chancap_dsp, test_ddc_matches_direct_model)
{
	static struct chancap_ddc c;
	double worst = 0.0;

	zassert_true(chancap_ddc_init(&c, 5, 16, 4, 0.075f, 40.0f), "init");
	noise(8000.0, 99);
	unsigned int j0 = CHANCAP_DDC_L / 4U + 1U, j1 = (N_IN - 4U) / 4U;

	c.outputs = j0;
	for (unsigned int j = j0; j < j1; j++) {
		unsigned int n = j * 4U + 3U, n0 = n - CHANCAP_DDC_L + 1U;
		int16_t oi, oq;
		double yi, yq;

		chancap_ddc_output(&c, &xi[n0], &xq[n0], n0, &oi, &oq);
		ddc_direct(&c, 5, 16, j, &yi, &yq);
		worst = fmax(worst, fmax(fabs(yi - oi), fabs(yq - oq)));
	}
	printk("ddc 5/16: worst error %.1f LSB\n", worst);
	zassert_true(worst <= 16.0, "worst %.1f", worst);
}

/* Mixing the stream once and filtering the mixed history equals the per-window path. */
ZTEST(chancap_dsp, test_ddc_mix_once)
{
	static struct chancap_ddc c;
	static int16_t mi[N_IN], mq[N_IN];

	zassert_true(chancap_ddc_init(&c, 3, 16, 4, 0.075f, 40.0f), "init");
	noise(9000.0, 123);
	chancap_ddc_mix(&c, xi, xq, 0, N_IN, mi, mq);
	for (unsigned int j = CHANCAP_DDC_L / 4U; j < (N_IN - 4U) / 4U; j++) {
		unsigned int n0 = j * 4U + 4U - CHANCAP_DDC_L;
		int16_t ai, aq, bi, bq;

		chancap_ddc_output(&c, &xi[n0], &xq[n0], n0, &ai, &aq);
		chancap_ddc_fir(&c, &mi[n0], &mq[n0], &bi, &bq);
		zassert_true(ai == bi && aq == bq, "output %u: %d %d vs %d %d", j, ai, aq, bi, bq);
	}
	/* Non-power-of-two periods are refused (the phase is a mask). */
	zassert_false(chancap_ddc_init(&c, 3, 12, 4, 0.075f, 40.0f), "period 12");
}

ZTEST(chancap_dsp, test_ddc_tone)
{
	static struct chancap_ddc c;
	double pw = 0.0, cs = 0.0, sn = 0.0, ref = TONE_A * TONE_A;
	int16_t li = 0, lq = 0;
	unsigned int j0 = CHANCAP_DDC_L / 4U + 1U, j1 = (N_IN - 4U) / 4U;

	zassert_true(chancap_ddc_init(&c, -5, 16, 4, 0.075f, 40.0f), "init");
	/* The channel at -5 MHz, a tone 200 kHz above its centre. */
	tone(-5000000.0 + 200000.0, TONE_A, 0.0);
	c.outputs = j0;
	for (unsigned int j = j0; j < j1; j++) {
		unsigned int n = j * 4U + 3U, n0 = n - CHANCAP_DDC_L + 1U;
		int16_t oi, oq;

		chancap_ddc_output(&c, &xi[n0], &xq[n0], n0, &oi, &oq);
		pw += (double)oi * oi + (double)oq * oq;
		if (j > j0) {
			cs += (double)oi * li + (double)oq * lq;
			sn += (double)oq * li - (double)oi * lq;
		}
		li = oi;
		lq = oq;
	}
	pw /= (double)(j1 - j0);
	zassert_within(10.0 * log10(pw / ref), 0.0, 1.0, "%.1f dB", 10.0 * log10(pw / ref));
	zassert_within(atan2(sn, cs) * 4e6 / ESP_SDR_TAU, 200000.0, 2000.0, "offset");
	/* A tone on the neighbouring 802.15.4 channel (5 MHz away) is rejected. */
	tone(200000.0, TONE_A, 0.0);
	pw = 0.0;
	for (unsigned int j = j0; j < j1; j++) {
		unsigned int n = j * 4U + 3U, n0 = n - CHANCAP_DDC_L + 1U;
		int16_t oi, oq;

		chancap_ddc_output(&c, &xi[n0], &xq[n0], n0, &oi, &oq);
		pw += (double)oi * oi + (double)oq * oq;
	}
	pw /= (double)(j1 - j0);
	zassert_true(10.0 * log10(pw / ref) < -35.0, "neighbour %.1f dB", 10.0 * log10(pw / ref));
	printk("ddc: tone within 1 dB at +200 kHz, neighbour at 5 MHz %.1f dB\n",
	       10.0 * log10(pw / ref));
}

/* ---- detector ---- */

ZTEST(chancap_dsp, test_detector)
{
	struct chancap_det d;
	uint32_t floor_p = 1000U;
	unsigned int opened = 0, closed_at = 0;

	chancap_det_init(&d, 10, 4);
	/* The floor settles on a noisy constant. */
	for (unsigned int b = 0; b < 400; b++) {
		zassert_false(chancap_det_update(&d, floor_p + (b % 7U) * 30U), "noise opened it");
	}
	zassert_within((int)d.floor, (int)floor_p + 90, 120, "floor %u", d.floor);
	/* 20 dB above: opens at once, peak kept, closes hang blocks after the drop. */
	for (unsigned int b = 0; b < 10; b++) {
		bool open = chancap_det_update(&d, 100U * floor_p);

		zassert_true(open, "not open at block %u", b);
		opened++;
	}
	zassert_equal(d.peak, 100U * floor_p, "peak");
	for (unsigned int b = 0; b < 20; b++) {
		if (!chancap_det_update(&d, floor_p)) {
			closed_at = b;
			break;
		}
	}
	zassert_equal(closed_at, 4, "closed after %u blocks", closed_at);
	/* The burst did not drag the floor up by more than a few percent. */
	zassert_true(d.floor < floor_p + 200U, "floor crept to %u", d.floor);
	/* 6 dB above opens too (threshold 10 dB? no: below it stays shut). */
	zassert_false(chancap_det_update(&d, 4U * floor_p), "6 dB opened a 10 dB detector");
	chancap_det_init(&d, 3, 1);
	for (unsigned int b = 0; b < 200; b++) {
		chancap_det_update(&d, floor_p);
	}
	zassert_true(chancap_det_update(&d, 4U * floor_p), "6 dB did not open a 3 dB detector");
	printk("detector: floor %u, opened %u blocks, closed after hang\n", d.floor, opened);
}

ZTEST(chancap_dsp, test_run8)
{
	static int16_t fi[8][8], fq[8][8], out[8][16];
	int16_t *dst[8];
	uint32_t p[8];

	for (unsigned int f = 0; f < 8; f++) {
		for (unsigned int c = 0; c < 8; c++) {
			fi[f][c] = (int16_t)(100 * f + c);
			fq[f][c] = (int16_t)-(100 * f + c);
		}
	}
	for (unsigned int c = 0; c < 8; c++) {
		dst[c] = out[c];
	}
	chancap_run8(fi, fq, dst, p);
	for (unsigned int c = 0; c < 8; c++) {
		uint64_t s = 0;

		for (unsigned int f = 0; f < 8; f++) {
			zassert_equal(out[c][2 * f], 100 * f + c, "lane %u frame %u I", c, f);
			zassert_equal(out[c][2 * f + 1], -(100 * f + c), "lane %u frame %u Q", c, f);
			s += 2ULL * (100 * f + c) * (100 * f + c);
		}
		zassert_equal(p[c], (uint32_t)(s >> 4), "lane %u power", c);
	}
	/*
	 * The packed power: steps of 2^5 below 2^20 (sum >> 9 unsaturated), of 2^15 above, and
	 * full scale (8 samples of +-32767 on both axes: p just under 2^30) stays below saturation.
	 */
	zassert_equal(chancap_run8_pack(12345U), 12320U, "%u", chancap_run8_pack(12345U));
	zassert_equal(chancap_run8_pack((32766U << 5) + 31U), 32766U << 5);
	/* At the switch point the coarse value takes over: 32767 << 5 is 31 steps of 2^15. */
	zassert_equal(chancap_run8_pack(32767U << 5), 31U << 15, "%u", chancap_run8_pack(32767U << 5));
	zassert_equal(chancap_run8_pack(100000000U), (100000000U >> 15) << 15);
	zassert_equal(chancap_run8_pack(0xffffffffU), 32767U << 15);
	{
		uint32_t fs = (uint32_t)((8ULL * 2U * 32767U * 32767U) >> 4);

		zassert_equal(chancap_run8_pack(fs), (fs >> 15) << 15, "full scale");
		zassert_true((fs >> 15) < 32767U, "full scale within the coarse range");
	}
	printk("run8: transpose and power as expected\n");
}

/* The split frame (branch sums, then post) and the frame-major power. */
ZTEST(chancap_dsp, test_pfb_split_and_power8)
{
	static struct chancap_pfb a, b;
	static int16_t fr[8][16];
	uint32_t p[8];

	zassert_true(chancap_pfb_init(&a, 8, 8, 8, true, 0.05f, 50.0f), "init");
	b = a;
	noise(9000.0, 77);
	for (unsigned int f = 0; f < 8; f++) {
		unsigned int n0 = 64U + 8U * f;
		int16_t oi[8], oq[8], ui[8], uq[8];

		chancap_pfb_frame(&a, &xi[n0], &xq[n0], oi, oq);
		chancap_pfb_sums(&b, &xi[n0], &xq[n0], ui, uq);
		chancap_pfb_post(&b, ui, uq, &fr[f][0], &fr[f][8]);
		zassert_mem_equal(oi, &fr[f][0], sizeof(oi), "frame %u I", f);
		zassert_mem_equal(oq, &fr[f][8], sizeof(oq), "frame %u Q", f);
	}
	chancap_power8(fr, p);
	for (unsigned int c = 0; c < 8; c++) {
		uint64_t s = 0;

		for (unsigned int f = 0; f < 8; f++) {
			s += (uint64_t)((int32_t)fr[f][c] * fr[f][c]) +
			     (uint64_t)((int32_t)fr[f][8 + c] * fr[f][8 + c]);
		}
		zassert_equal(p[c], (uint32_t)(s >> 4), "lane %u", c);
	}
}

ZTEST(chancap_dsp, test_power)
{
	static int16_t i[64], q[64];

	for (unsigned int n = 0; n < 64; n++) {
		i[n] = 32767;
		q[n] = 32767;
	}
	zassert_equal(chancap_power(i, q, 64), 2U * 32767U * 32767U, "full scale");
	zassert_within(chancap_power_db(2U * 32767U * 32767U), 0.0f, 0.01f, "0 dBFS");
}

ZTEST_SUITE(chancap_dsp, NULL, NULL, NULL, NULL, NULL);
