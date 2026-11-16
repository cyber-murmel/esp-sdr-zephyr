/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit test of the spectrum analyser math of apps/ubertooth (ubt_specan.c), on native_sim.
 * Synthetic 80 MS/s captures of 16380 words (tones at known frequencies and levels, white and band
 * limited noise, a DC offset and an LO spur, quantised to 10 bits as the capture engine writes
 * them) go through the frame decode, the reference 16 bit FFT and the channel binning; levels are
 * checked against the level the generator was told to make, the sweep against the plan, the packets
 * against the byte layout the host's cb_specan() reads.
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <esp_sdr/esp_sdr_dsp.h>

#include "ubt_specan.h"

#define CAP     UBT_SPECAN_CAPTURE
#define FS_FULL 512.0
/* The calibration constant the tests pretend a measurement gave: 0 dBFS is -30 dBm. */
#define FS_DBM  (-30.0)

static uint32_t cap[CAP];
static struct ubt_specan sa;
static uint32_t rng_state;

static uint32_t rnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

/* Uniform in (0, 1). */
static double urand(void)
{
	return ((double)rnd() + 0.5) / 4294967296.0;
}

static double grand(void)
{
	return sqrt(-2.0 * log(urand())) * cos(ESP_SDR_TAU * urand());
}

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	rng_state = 0x1234567U;
}

ZTEST_SUITE(ubertooth_specan, NULL, NULL, before_each, NULL, NULL);

/* ----------------------------------------------------------- generator */

struct tone {
	double mhz;  /* from the LO */
	double dbfs; /* level before the analog response */
};

static uint32_t pack_word(double i, double q)
{
	long li = lround(i), lq = lround(q);

	li = li > 511 ? 511 : (li < -512 ? -512 : li);
	lq = lq > 511 ? 511 : (lq < -512 ? -512 : lq);
	return (((uint32_t)li & 0x3ffU) << 10) | ((uint32_t)lq & 0x3ffU);
}

/* Analog response in dB at an offset (the filter's roll-off inside the span), 0 for flat. */
static double rolloff_db(double mhz)
{
	return -0.004 * mhz * mhz;
}

static int use_rolloff;

/* Tones, white noise of @p sigma LSB per axis, offsets; quantised. */
static void synth(uint32_t *w, const struct tone *t, unsigned int nt, double sigma, int dc_i,
		  int dc_q)
{
	double amp[8], phase[8], step[8];

	for (unsigned int k = 0; k < nt; k++) {
		double db = t[k].dbfs + (use_rolloff ? rolloff_db(t[k].mhz) : 0.0);

		amp[k] = FS_FULL * pow(10.0, db / 20.0);
		phase[k] = ESP_SDR_TAU * urand();
		step[k] = ESP_SDR_TAU * t[k].mhz / 80.0;
	}
	for (unsigned int n = 0; n < CAP; n++) {
		double i = dc_i, q = dc_q;

		for (unsigned int k = 0; k < nt; k++) {
			double a = phase[k] + step[k] * (double)n;

			i += amp[k] * cos(a);
			q += amp[k] * sin(a);
		}
		if (sigma > 0.0) {
			i += sigma * grand();
			q += sigma * grand();
		}
		w[n] = pack_word(i, q);
	}
}

static void fft_dbl(double *re, double *im, unsigned int n)
{
	for (unsigned int i = 1, j = 0; i < n; i++) {
		unsigned int bit = n >> 1;

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
	for (unsigned int len = 2; len <= n; len <<= 1) {
		for (unsigned int i = 0; i < n; i += len) {
			for (unsigned int k = 0; k < len / 2; k++) {
				double a = -ESP_SDR_TAU * k / len, c = cos(a), s = sin(a);
				unsigned int x = i + k, y = i + k + len / 2;
				double tr = re[y] * c - im[y] * s, ti = re[y] * s + im[y] * c;

				re[y] = re[x] - tr;
				im[y] = im[x] - ti;
				re[x] += tr;
				im[x] += ti;
			}
		}
	}
}

/* Noise confined to [lo, hi] MHz from the LO, total power @p dbfs (relative to a full scale tone),
 * plus white noise. */
static void synth_band_noise(uint32_t *w, double lo, double hi, double dbfs, double sigma)
{
	static double re[16384], im[16384];
	double p = 0, scale;

	memset(im, 0, sizeof(im));
	for (unsigned int k = 0; k < 16384U; k++) {
		double f = (k < 8192U ? (double)k : (double)k - 16384.0) * 80.0 / 16384.0;

		if (f >= lo && f < hi) {
			re[k] = grand();
			im[k] = grand();
		} else {
			re[k] = 0;
			im[k] = 0;
		}
	}
	/* inverse transform by conjugation */
	for (unsigned int k = 0; k < 16384U; k++) {
		im[k] = -im[k];
	}
	fft_dbl(re, im, 16384);
	for (unsigned int k = 0; k < 16384U; k++) {
		im[k] = -im[k];
		p += re[k] * re[k] + im[k] * im[k];
	}
	p /= 16384.0;
	scale = FS_FULL * pow(10.0, dbfs / 20.0) / sqrt(p);
	for (unsigned int n = 0; n < CAP; n++) {
		double i = re[n] * scale + sigma * grand(), q = im[n] * scale + sigma * grand();

		w[n] = pack_word(i, q);
	}
}

/* 1 / the analog response in power, Q12, for the bins in natural order. */
static uint16_t flat_tab[UBT_SPECAN_N];

static void flat_init(void)
{
	for (int k = 0; k < (int)UBT_SPECAN_N; k++) {
		double mhz = (k < 128 ? k : k - 256) * 0.3125;
		double g = pow(10.0, -rolloff_db(mhz) / 10.0);

		flat_tab[k] = (uint16_t)lround(4096.0 * g > 65535.0 ? 65535.0 : 4096.0 * g);
	}
}

static struct ubt_specan_cal cal(bool flat)
{
	return (struct ubt_specan_cal){.fs_dbm_q8 = (int32_t)lround(FS_DBM * 256.0),
				       .flat = flat ? flat_tab : NULL,
				       .mirror = false};
}

static void run_capture(const uint32_t *w, unsigned int frames)
{
	ubt_specan_init(&sa, ubt_specan_fft_ref, UBT_SPECAN_IN_SHIFT);
	zassert_equal(ubt_specan_capture(&sa, w, CAP, 256, frames), frames);
}

/* The level of the channel at @p mhz from the LO in dBFS. */
static double chan_dbfs(const struct ubt_specan *s, int mhz, const struct ubt_specan_cal *c)
{
	int32_t q = ubt_specan_dbfs_q8(ubt_specan_chan_power(s, mhz, c), s->frames, s->in_shift);

	return q == INT32_MIN ? -1000.0 : q / 256.0;
}

static unsigned int rev8t(unsigned int k)
{
	unsigned int r = 0;

	for (unsigned int b = 0; b < 8U; b++) {
		r |= ((k >> b) & 1U) << (7U - b);
	}
	return r;
}

/* --------------------------------------------------------------- tests */

ZTEST(ubertooth_specan, test_reference_fft_contract)
{
	static int16_t buf[2 * UBT_SPECAN_N];
	static double in_re[UBT_SPECAN_N], in_im[UBT_SPECAN_N];
	double worst = 0;

	for (unsigned int n = 0; n < UBT_SPECAN_N; n++) {
		buf[2U * n] = (int16_t)((int)(rnd() % 20001U) - 10000);
		buf[2U * n + 1U] = (int16_t)((int)(rnd() % 20001U) - 10000);
		in_re[n] = buf[2U * n];
		in_im[n] = buf[2U * n + 1U];
	}
	fft_dbl(in_re, in_im, UBT_SPECAN_N);
	ubt_specan_fft_ref(buf);
	for (unsigned int k = 0; k < UBT_SPECAN_N; k++) {
		/* bit reversed order out, scaled by 1/256 */
		unsigned int r = 0;

		for (unsigned int b = 0; b < 8U; b++) {
			r |= ((k >> b) & 1U) << (7U - b);
		}
		worst = fmax(worst, fabs(buf[2U * r] - in_re[k] / 256.0));
		worst = fmax(worst, fabs(buf[2U * r + 1U] - in_im[k] / 256.0));
	}
	TC_PRINT("fft16 reference worst error %.2f LSB\n", worst);
	zassert_true(worst < 4.0, "worst %f", worst);

	/* a tone at bin 37, amplitude 8000: lands at the bit reversed place with amplitude 8000 */
	for (unsigned int n = 0; n < UBT_SPECAN_N; n++) {
		buf[2U * n] = (int16_t)lround(8000.0 * cos(ESP_SDR_TAU * 37.0 * n / 256.0));
		buf[2U * n + 1U] = (int16_t)lround(8000.0 * sin(ESP_SDR_TAU * 37.0 * n / 256.0));
	}
	ubt_specan_fft_ref(buf);
	{
		unsigned int r = 0;

		for (unsigned int b = 0; b < 8U; b++) {
			r |= ((37U >> b) & 1U) << (7U - b);
		}
		zassert_within(hypot(buf[2U * r], buf[2U * r + 1U]), 8000.0, 80.0, "tone bin");
	}
	/* twiddle table: cos, sin of 2 pi rev7(i) / 256 */
	for (unsigned int i = 0; i < 128U; i++) {
		unsigned int r = 0;

		for (unsigned int b = 0; b < 7U; b++) {
			r |= ((i >> b) & 1U) << (6U - b);
		}
		zassert_within(ubt_specan_w256[2U * i], 32767.0 * cos(ESP_SDR_TAU * r / 256.0), 1.0);
		zassert_within(ubt_specan_w256[2U * i + 1U], 32767.0 * sin(ESP_SDR_TAU * r / 256.0),
			       1.0);
	}
	zassert_equal((uintptr_t)ubt_specan_w256 % 16U, 0U, "aligned for the vector kernel");
}

ZTEST(ubertooth_specan, test_decode_words)
{
	static uint32_t w[UBT_SPECAN_N];
	static int16_t iq[2 * UBT_SPECAN_N];

	/* I in bits 19:10, Q in bits 9:0, signed; the DC is removed, the Hann window and 2^shift
	 * applied */
	for (unsigned int n = 0; n < UBT_SPECAN_N; n++) {
		w[n] = pack_word(100.0, -60.0);
	}
	ubt_specan_decode(w, 0, 0, 4, iq);
	zassert_equal(iq[0], 0, "window starts at zero");
	zassert_equal(iq[1], 0);
	zassert_equal(iq[2U * 128U], 1600, "I 100 x 16 at the window peak");
	zassert_equal(iq[2U * 128U + 1U], -960, "Q -60 x 16");
	zassert_equal(iq[2U * 64U], 800, "half way up the Hann window");
	zassert_equal(iq[2U * 192U], 800, "and down: symmetric");
	zassert_equal(iq[2U * 255U], iq[2U * 1U]);
	ubt_specan_decode(w, 100, -60, 4, iq);
	for (unsigned int n = 0; n < 2U * UBT_SPECAN_N; n++) {
		zassert_equal(iq[n], 0, "DC removed");
	}
	/* extremes */
	for (unsigned int n = 0; n < UBT_SPECAN_N; n++) {
		w[n] = pack_word(-512.0, 511.0);
	}
	ubt_specan_decode(w, 0, 0, 5, iq);
	zassert_equal(iq[2U * 128U], -16384);
	zassert_equal(iq[2U * 128U + 1U], 16352);
	/* with a DC of -(+511): worst case 1023 x 32 stays inside int16 */
	ubt_specan_decode(w, 511, -512, 5, iq);
	zassert_equal(iq[2U * 128U], -32736);
	zassert_equal(iq[2U * 128U + 1U], 32736);
	/* word bits above bit 19 are ignored */
	w[0] = 0xfff00000U | pack_word(3.0, -2.0);
	ubt_specan_decode(w, 0, 0, 4, iq);
	zassert_equal(iq[0], 0);
	w[128] = 0xabc00000U | pack_word(7.0, -2.0);
	ubt_specan_decode(w, 0, 0, 4, iq);
	zassert_equal(iq[2U * 128U], 112);
	zassert_equal(iq[2U * 128U + 1U], -32);
}

ZTEST(ubertooth_specan, test_dc_estimate)
{
	int dci, dcq;

	synth(cap, NULL, 0, 2.0, 21, -14);
	ubt_specan_dc(cap, CAP, &dci, &dcq);
	zassert_within(dci, 21, 1, "dc i %d", dci);
	zassert_within(dcq, -14, 1, "dc q %d", dcq);
	synth(cap, NULL, 0, 0.0, -37, 29);
	ubt_specan_dc(cap, CAP, &dci, &dcq);
	zassert_equal(dci, -37);
	zassert_equal(dcq, 29);
	ubt_specan_dc(cap, 0, &dci, &dcq);
	zassert_equal(dci, 0);
}

ZTEST(ubertooth_specan, test_channel_power_weights)
{
	struct ubt_specan s = {0};
	struct ubt_specan_cal flat_half = {.fs_dbm_q8 = 0, .flat = flat_tab, .mirror = false};

	/* a flat spectrum: a channel is 32 / 10 bins wide, whatever its offset */
	for (unsigned int k = 0; k < UBT_SPECAN_N; k++) {
		s.acc[k] = 1000;
		flat_tab[k] = 2048;
	}
	for (int d = -39; d <= 39; d++) {
		zassert_equal(ubt_specan_chan_power(&s, d, NULL), 32000U, "offset %d", d);
		zassert_equal(ubt_specan_chan_power(&s, d, &flat_half), 16000U, "flat table at %d",
			      d);
	}
	/* one hot bin k (natural order) sits at the bit reversed place; its cell is 5 / 16 MHz wide
	 */
	memset(s.acc, 0, sizeof(s.acc));
	s.acc[rev8t(3)] = 1000;
	/* bin 3 is 0.9375 MHz: cell 0.78 to 1.09 MHz, inside the channel at 1 MHz only */
	zassert_equal(ubt_specan_chan_power(&s, 1, NULL), 10000U);
	zassert_equal(ubt_specan_chan_power(&s, 0, NULL), 0U);
	zassert_equal(ubt_specan_chan_power(&s, 2, NULL), 0U);
	/* mirrored: the same bin is read at -1 MHz */
	{
		struct ubt_specan_cal mir = {.fs_dbm_q8 = 0, .flat = NULL, .mirror = true};

		zassert_equal(ubt_specan_chan_power(&s, -1, &mir), 10000U);
		zassert_equal(ubt_specan_chan_power(&s, 1, &mir), 0U);
	}
	/* bin 253 is bin -3 */
	memset(s.acc, 0, sizeof(s.acc));
	s.acc[rev8t(253)] = 1000;
	zassert_equal(ubt_specan_chan_power(&s, -1, NULL), 10000U);
	/* a cell split between two channels: bin 5 = 1.5625 MHz, cell 1.406 to 1.719 MHz: 0.094 of
	 * 0.3125 MHz in channel 1 */
	memset(s.acc, 0, sizeof(s.acc));
	s.acc[rev8t(5)] = 1000;
	zassert_equal(ubt_specan_chan_power(&s, 1, NULL), 3000U, "30 %% of the cell in channel 1");
	zassert_equal(ubt_specan_chan_power(&s, 2, NULL), 7000U);
}

ZTEST(ubertooth_specan, test_log_and_rssi_byte)
{
	struct ubt_specan s = {0};
	struct ubt_specan_cal c = {.fs_dbm_q8 = 0, .flat = NULL, .mirror = false};

	/* the dB conversion against libm over 12 decades */
	for (double pd = 1.0; pd < 1e12; pd *= 1.37) {
		uint64_t p = (uint64_t)pd;

		for (unsigned int g = 0; g <= 5U; g += 5U) {
			int32_t q = ubt_specan_dbfs_q8(p, 63, g);
			double want = 10.0 * log10((double)p) - 10.0 - 10.0 * log10(63.0) -
				      10.0 * log10(0.375 * FS_FULL * FS_FULL * pow(4.0, g));

			zassert_within(q / 256.0, want, 0.03, "power %g shift %u", (double)p, g);
		}
	}
	zassert_equal(ubt_specan_dbfs_q8(0, 63, 4), INT32_MIN);
	/* the byte: dBm + 54, saturating */
	s.frames = 63;
	s.in_shift = 4;
	memset(s.acc, 0, sizeof(s.acc));
	zassert_equal(ubt_specan_channel_raw(&s, 10, &c), -128, "no power");
	for (unsigned int k = 0; k < UBT_SPECAN_N; k++) {
		s.acc[k] = 0xffffffffU;
	}
	c.fs_dbm_q8 = 100 * 256;
	zassert_equal(ubt_specan_channel_raw(&s, 10, &c), 127, "saturates high");
	c.fs_dbm_q8 = -300 * 256;
	zassert_equal(ubt_specan_channel_raw(&s, 10, &c), -128, "saturates low");
	/* a full scale tone over 63 frames: 63 x 0.375 x (512 x 16)^2 in one bin, channel weight 10
	 */
	memset(s.acc, 0, sizeof(s.acc));
	s.acc[rev8t(10)] = (uint32_t)(63.0 * 0.375 * pow(512.0 * 16.0, 2.0));
	c.fs_dbm_q8 = (int32_t)(-30.0 * 256.0);
	/* bin 10 = 3.125 MHz, a cell inside the channel at 3 MHz: reads the tone's dBFS = 0 -> -30
	 * + 54 = 24 */
	zassert_within(ubt_specan_channel_raw(&s, 3, &c), 24, 1);
}

ZTEST(ubertooth_specan, test_tone_levels_in_one_capture)
{
	struct ubt_specan_cal c = cal(false);
	double worst = 0;

	/*
	 * Tones from -40 to -4 dBFS (5 to 320 LSB) at offsets around the LO, 1 LSB of noise, a DC
	 * offset. A tone of 1 or 2 LSB is not a tone any more once the ADC has rounded it: -1 dB at
	 * -52 dBFS.
	 */
	for (double dbfs = -40.0; dbfs <= -4.0; dbfs += 6.0) {
		for (int mhz = -26; mhz <= 26; mhz += 4) {
			struct tone t = {.mhz = mhz, .dbfs = dbfs};
			double got;

			if (abs(mhz) < 2) {
				continue;
			}
			synth(cap, &t, 1, 1.0, 11, -8);
			run_capture(cap, 63);
			got = chan_dbfs(&sa, mhz, &c);
			zassert_within(got, dbfs, 0.5, "tone %d MHz at %.0f dBFS read %.2f", mhz,
				       dbfs, got);
			worst = fmax(worst, fabs(got - dbfs));
		}
	}
	TC_PRINT("worst error of a tone at a channel centre: %.2f dB\n", worst);
}

ZTEST(ubertooth_specan, test_calibration_from_one_source)
{
	/* A source of -63.5 dBm gave the reading at 10 MHz; the board's real 0 dBFS is -63.5 + 25
	 * dBm. */
	const double truth_fs_dbm = -38.5;
	struct ubt_specan_cal c = {.fs_dbm_q8 = 0, .flat = NULL, .mirror = false};
	double worst = 0;
	int32_t k;

	{
		struct tone t = {.mhz = 10.0, .dbfs = -25.0};

		synth(cap, &t, 1, 1.0, 3, 3);
		run_capture(cap, 63);
		k = ubt_specan_cal_fs_dbm(&sa, 10, &c,
					  (int32_t)lround((truth_fs_dbm - 25.0) * 256.0));
		c.fs_dbm_q8 = k;
		zassert_within(k / 256.0, truth_fs_dbm, 0.3, "K %f", k / 256.0);
	}
	/* other levels and places read the right dBm within 1 dB over 36 dB */
	for (double dbfs = -40.0; dbfs <= -4.0; dbfs += 9.0) {
		for (int mhz = -24; mhz <= 24; mhz += 12) {
			struct tone t = {.mhz = mhz, .dbfs = dbfs};
			double dbm;

			if (mhz == 0) {
				continue; /* the LO's own channel is not read from this placement */
			}
			synth(cap, &t, 1, 1.0, -6, 9);
			run_capture(cap, 63);
			dbm = chan_dbfs(&sa, mhz, &c) + c.fs_dbm_q8 / 256.0;
			worst = fmax(worst, fabs(dbm - (truth_fs_dbm + dbfs)));
			zassert_within(dbm, truth_fs_dbm + dbfs, 1.0, "%d MHz %.0f dBFS: %.2f dBm",
				       mhz, dbfs, dbm);
		}
	}
	TC_PRINT("after one point calibration: worst error %.2f dB\n", worst);
	memset(sa.acc, 0, sizeof(sa.acc));
	zassert_equal(ubt_specan_cal_fs_dbm(&sa, 10, &c, 0), INT32_MIN);
}

ZTEST(ubertooth_specan, test_tone_offsets_inside_a_channel)
{
	struct ubt_specan_cal c = cal(false);

	/*
	 * Across a channel the response is that of a 1 MHz filter with -3 dB at the edges (the Hann
	 * lobe is 1.25 MHz wide): within 0.9 dB over the middle 0.4 MHz, as the bin alignment
	 * moves.
	 */
	for (double off = -0.5; off <= 0.501; off += 0.1) {
		struct tone t = {.mhz = 9.0 + off, .dbfs = -30.0};
		double got;

		synth(cap, &t, 1, 1.0, 5, 5);
		run_capture(cap, 63);
		got = chan_dbfs(&sa, 9, &c);
		TC_PRINT("tone %+.2f MHz off the channel centre reads %.2f dB\n", off, got + 30.0);
		if (fabs(off) <= 0.21) {
			zassert_within(got, -30.0, 0.9, "offset %f read %f", off, got);
		} else if (fabs(off) > 0.49) {
			zassert_within(got, -33.0, 0.8, "edge: offset %f read %f", off, got);
		}
	}
}

ZTEST(ubertooth_specan, test_channel_neighbours)
{
	struct ubt_specan_cal c = cal(false);
	struct tone t = {.mhz = 9.0, .dbfs = -20.0};

	synth(cap, &t, 1, 1.0, 0, 0);
	run_capture(cap, 63);
	zassert_within(chan_dbfs(&sa, 9, &c), -20.0, 0.5);
	zassert_true(chan_dbfs(&sa, 8, &c) < -20.0 - 14.0, "neighbour %f", chan_dbfs(&sa, 8, &c));
	zassert_true(chan_dbfs(&sa, 10, &c) < -20.0 - 14.0);
	zassert_true(chan_dbfs(&sa, 7, &c) < -20.0 - 35.0, "two away %f", chan_dbfs(&sa, 7, &c));
	zassert_true(chan_dbfs(&sa, 12, &c) < -20.0 - 45.0);
	zassert_true(chan_dbfs(&sa, -9, &c) < -20.0 - 45.0, "image side %f",
		     chan_dbfs(&sa, -9, &c));
}

ZTEST(ubertooth_specan, test_band_limited_noise)
{
	struct ubt_specan_cal c = cal(false);
	double sum = 0;

	/* noise in 12 to 16 MHz (four channels), total -30 dBFS: -36 dBFS per channel */
	synth_band_noise(cap, 12.0, 16.0, -30.0, 0.5);
	run_capture(cap, 63);
	for (int d = 13; d <= 15; d++) {
		double got = chan_dbfs(&sa, d, &c);

		zassert_within(got, -30.0 - 10.0 * log10(4.0), 0.8, "noise channel %d: %.2f", d,
			       got);
		sum += pow(10.0, got / 10.0);
	}
	/* channels beyond the band edge: down by the filter edge or at the white floor */
	zassert_true(chan_dbfs(&sa, 18, &c) < -30.0 - 25.0, "outside %f", chan_dbfs(&sa, 18, &c));
	zassert_true(chan_dbfs(&sa, 10, &c) < -30.0 - 25.0);
	zassert_true(chan_dbfs(&sa, -14, &c) < -30.0 - 30.0);
}

ZTEST(ubertooth_specan, test_rolloff_needs_the_flatness_table)
{
	struct ubt_specan_cal flat = cal(true), raw = cal(false);
	struct tone t = {.mhz = 27.0, .dbfs = -30.0};

	use_rolloff = 1;
	flat_init();
	synth(cap, &t, 1, 1.0, 0, 0);
	run_capture(cap, 63);
	TC_PRINT("27 MHz with the roll-off: %.2f dBFS, with the flatness table: %.2f\n",
		 chan_dbfs(&sa, 27, &raw), chan_dbfs(&sa, 27, &flat));
	zassert_true(chan_dbfs(&sa, 27, &raw) < -30.0 - 2.4, "uncorrected %f",
		     chan_dbfs(&sa, 27, &raw));
	zassert_within(chan_dbfs(&sa, 27, &flat), -30.0, 0.5, "corrected");
	use_rolloff = 0;
}

ZTEST(ubertooth_specan, test_dc_and_spur_do_not_leak)
{
	struct ubt_specan_cal c = cal(false);
	struct tone t[2] = {{.mhz = 0.2, .dbfs = -16.0}, {.mhz = 12.0, .dbfs = -40.0}};

	/* DC offset of (30, -25) and a spur at +200 kHz, 24 dB above the wanted tone at 12 MHz */
	synth(cap, t, 2, 1.0, 30, -25);
	run_capture(cap, 63);
	zassert_within(chan_dbfs(&sa, 12, &c), -40.0, 0.7, "wanted %f", chan_dbfs(&sa, 12, &c));
	zassert_true(chan_dbfs(&sa, -12, &c) < -66.0, "empty %f", chan_dbfs(&sa, -12, &c));
	zassert_true(chan_dbfs(&sa, 3, &c) < -62.0, "3 MHz from the spur %f",
		     chan_dbfs(&sa, 3, &c));
	zassert_true(chan_dbfs(&sa, -3, &c) < -62.0, "-3 MHz %f", chan_dbfs(&sa, -3, &c));
	zassert_true(chan_dbfs(&sa, 2, &c) < -50.0, "2 MHz %f", chan_dbfs(&sa, 2, &c));
	zassert_true(chan_dbfs(&sa, 0, &c) > -20.0, "the spur itself is there: %f",
		     chan_dbfs(&sa, 0, &c));
}

/* ---------------------------------------------------------------- plans */

static void check_plan(const struct ubt_specan_plan *p, const struct ubt_specan_cfg *cfg)
{
	for (unsigned int f = p->low; f <= p->high; f++) {
		int idx = ubt_specan_lo_for(p, (uint16_t)f);

		zassert_true(idx >= 0 && idx < p->n_lo, "channel %u has no placement", f);
		{
			int d = abs((int)f - (int)p->lo_mhz[idx]);

			zassert_true(d >= cfg->excl_mhz && d <= cfg->span_mhz,
				     "channel %u at %d from %u", f, d, p->lo_mhz[idx]);
		}
	}
	for (unsigned int i = 0; i < p->n_lo; i++) {
		zassert_true(p->lo_mhz[i] >= cfg->lo_min_mhz && p->lo_mhz[i] <= cfg->lo_max_mhz);
	}
}

ZTEST(ubertooth_specan, test_plan_for_the_bluetooth_band)
{
	struct ubt_specan_plan p;
	struct ubt_specan_cfg cfg;

	ubt_specan_cfg_default(&cfg);
	zassert_equal(ubt_specan_plan(&p, 2402, 2480, NULL), 0);
	TC_PRINT("2402..2480: %u placements %u %u\n", p.n_lo, p.lo_mhz[0], p.lo_mhz[1]);
	zassert_equal(p.n_lo, 2U, "two placements");
	check_plan(&p, &cfg);
	zassert_equal(ubt_specan_plan(&p, 2400, 2483, NULL), 0);
	zassert_equal(p.n_lo, 2U);
	check_plan(&p, &cfg);
	/* the whole band the host may ask for */
	zassert_equal(ubt_specan_plan(&p, UBT_SPECAN_MIN, UBT_SPECAN_MAX, NULL), 0);
	TC_PRINT("2049..3072: %u placements\n", p.n_lo);
	zassert_true(p.n_lo <= 39U && p.n_lo >= 37U);
	check_plan(&p, &cfg);
	/* the LO of the plan for the band is 2430 +- a few, so the spur channel comes from the
	 * other one */
	zassert_equal(ubt_specan_plan(&p, 2402, 2480, NULL), 0);
	for (unsigned int i = 0; i < 2U; i++) {
		int idx = ubt_specan_lo_for(&p, p.lo_mhz[i]);

		zassert_true(idx >= 0 && idx != (int)i,
			     "the channel at LO %u is read from the other placement", i);
	}
}

ZTEST(ubertooth_specan, test_plans_for_any_range)
{
	struct ubt_specan_cfg cfg;
	struct ubt_specan_plan p;
	unsigned int single = 0, multi = 0;

	ubt_specan_cfg_default(&cfg);
	/* every length from one channel up at a few places */
	for (unsigned int low = 2049; low <= 3072; low += 61) {
		for (unsigned int len = 1; len <= 300; len++) {
			unsigned int high = low + len - 1U;
			unsigned int want;

			if (high > UBT_SPECAN_MAX) {
				continue;
			}
			zassert_equal(ubt_specan_plan(&p, (uint16_t)low, (uint16_t)high, &cfg), 0,
				      "[%u, %u]", low, high);
			check_plan(&p, &cfg);
			/* minimal: one placement for 27 channels (one side of the LO), else 2 + the
			 * tiling of the rest */
			want = len <= 27U ? 1U : (len <= 84U ? 2U : 1U + (len - 57U + 26U) / 27U);
			zassert_equal(p.n_lo, want, "[%u, %u] -> %u placements, want %u", low, high,
				      p.n_lo, want);
			if (p.n_lo == 1U) {
				single++;
			} else {
				multi++;
			}
		}
	}
	for (unsigned int i = 0; i < 400U; i++) {
		unsigned int low = 2049U + rnd() % 1024U,
			     high = low + rnd() % (UBT_SPECAN_MAX - low + 1U);

		zassert_equal(ubt_specan_plan(&p, (uint16_t)low, (uint16_t)high, NULL), 0,
			      "[%u, %u]", low, high);
		check_plan(&p, &cfg);
	}
	TC_PRINT("%u single and %u multi placement plans\n", single, multi);
	/* a one channel span, and two channels */
	zassert_equal(ubt_specan_plan(&p, 2441, 2441, NULL), 0);
	zassert_equal(p.n_lo, 1U);
	zassert_true(abs(2441 - (int)p.lo_mhz[0]) >= 2 && abs(2441 - (int)p.lo_mhz[0]) <= 28);
	zassert_equal(ubt_specan_plan(&p, 2441, 2442, NULL), 0);
	zassert_equal(p.n_lo, 1U);
}

ZTEST(ubertooth_specan, test_plan_limits_and_errors)
{
	struct ubt_specan_cfg cfg;
	struct ubt_specan_plan p;

	ubt_specan_cfg_default(&cfg);
	zassert_equal(ubt_specan_plan(&p, 2480, 2402, NULL), -EINVAL);
	cfg.excl_mhz = 0;
	zassert_equal(ubt_specan_plan(&p, 2402, 2480, &cfg), -EINVAL);
	cfg.excl_mhz = 30;
	zassert_equal(ubt_specan_plan(&p, 2402, 2480, &cfg), -EINVAL);

	/* a narrower usable span needs more placements */
	ubt_specan_cfg_default(&cfg);
	cfg.span_mhz = 20;
	zassert_equal(ubt_specan_plan(&p, 2402, 2480, &cfg), 0);
	zassert_true(p.n_lo >= 3U, "%u placements for a span of 20", p.n_lo);
	check_plan(&p, &cfg);
	/* a skip zone of 4 MHz */
	ubt_specan_cfg_default(&cfg);
	cfg.excl_mhz = 4;
	zassert_equal(ubt_specan_plan(&p, 2402, 2480, &cfg), 0);
	check_plan(&p, &cfg);
	/* the LO limited to a window: placements stay in it */
	ubt_specan_cfg_default(&cfg);
	cfg.lo_min_mhz = 2405;
	cfg.lo_max_mhz = 2440;
	zassert_equal(ubt_specan_plan(&p, 2402, 2480, &cfg), -ERANGE,
		      "2480 is out of reach of an LO below 2440 + 28");
	zassert_equal(ubt_specan_plan(&p, 2402, 2460, &cfg), 0);
	check_plan(&p, &cfg);
	/* a small span near the lower LO limit: the LO goes above the range */
	cfg.lo_min_mhz = 2410;
	cfg.lo_max_mhz = 2500;
	zassert_equal(ubt_specan_plan(&p, 2402, 2410, &cfg), 0);
	check_plan(&p, &cfg);
	zassert_equal(p.n_lo, 1U);
	zassert_true(p.lo_mhz[0] > 2410U, "LO above the range: %u", p.lo_mhz[0]);
	/* too many placements */
	ubt_specan_cfg_default(&cfg);
	cfg.span_mhz = 3;
	cfg.excl_mhz = 2;
	zassert_equal(ubt_specan_plan(&p, 2049, 3072, &cfg), -ERANGE);
}

/* -------------------------------------------------------- whole sweeps */

static int8_t sweep_raw[1024];
static struct ubt_specan_plan plan;

/* Capture the tones (given as absolute MHz and dBFS) at every placement and read all channels. */
static void sweep(const double *tone_mhz, const double *tone_dbfs, unsigned int nt, double sigma,
		  int dc_i, int dc_q, bool flat)
{
	struct ubt_specan_cal c = cal(flat);

	for (unsigned int i = 0; i < plan.n_lo; i++) {
		struct tone t[4];
		unsigned int n = 0;

		for (unsigned int k = 0; k < nt; k++) {
			/* what the analog filter and the ADC pass: within the 80 MS/s capture */
			if (fabs(tone_mhz[k] - plan.lo_mhz[i]) < 39.0) {
				t[n++] = (struct tone){.mhz = tone_mhz[k] - plan.lo_mhz[i],
						       .dbfs = tone_dbfs[k]};
			}
		}
		synth(cap, t, n, sigma, dc_i, dc_q);
		run_capture(cap, 63);
		ubt_specan_collect(&sa, &plan, i, &c, sweep_raw);
	}
}

ZTEST(ubertooth_specan, test_sweep_finds_a_tone_in_every_channel)
{
	double worst = 0;

	zassert_equal(ubt_specan_plan(&plan, 2402, 2480, NULL), 0);
	/* the generator's roll-off and the table that undoes it, as the board would be calibrated
	 */
	use_rolloff = 1;
	flat_init();
	for (unsigned int f = 2402; f <= 2480U; f++) {
		double mhz = f, dbfs = -34.0;
		int best = -1000, at = 0;

		sweep(&mhz, &dbfs, 1, 1.0, 7, -5, true);
		for (unsigned int g = 2402; g <= 2480U; g++) {
			if (sweep_raw[g - 2402U] > best) {
				best = sweep_raw[g - 2402U];
				at = (int)g;
			}
		}
		zassert_equal(at, (int)f, "tone at %u found at %d", f, at);
		/* -34 dBFS = -64 dBm = -10 on the byte; rounding to 1 dB */
		worst = fmax(worst, fabs(sweep_raw[f - 2402U] - (-34.0 + FS_DBM + 54.0)));
		zassert_within(sweep_raw[f - 2402U], -34.0 + FS_DBM + 54.0, 1.0, "channel %u: %d",
			       f, sweep_raw[f - 2402U]);
		/* the floor away from the tone */
		for (unsigned int g = 2402; g <= 2480U; g++) {
			if (abs((int)g - (int)f) >= 4) {
				zassert_true(sweep_raw[g - 2402U] < sweep_raw[f - 2402U] - 25,
					     "channel %u with the tone at %u: %d", g, f,
					     sweep_raw[g - 2402U]);
			}
		}
	}
	TC_PRINT("worst level error over the band %.2f dB (byte rounding included)\n", worst);
	use_rolloff = 0;
}

ZTEST(ubertooth_specan, test_sweep_order_and_spur_fill)
{
	struct ubt_specan_cal c = cal(false);
	double mhz[2], dbfs[2];

	zassert_equal(ubt_specan_plan(&plan, 2402, 2480, NULL), 0);
	/* the wanted signal sits on the first placement's LO, where that capture has its spur */
	mhz[0] = plan.lo_mhz[0];
	dbfs[0] = -36.0;
	mhz[1] = plan.lo_mhz[1] + 0.2; /* a second signal, 200 kHz above the other placement's LO */
	dbfs[1] = -50.0;
	/* LO leakage: a strong tone at each capture's DC. The sweep adds it to every capture. */
	for (unsigned int i = 0; i < plan.n_lo; i++) {
		struct tone t[3];
		unsigned int n = 0;

		t[n++] = (struct tone){.mhz = 0.15, .dbfs = -14.0}; /* this placement's LO leak */
		for (unsigned int k = 0; k < 2U; k++) {
			if (fabs(mhz[k] - plan.lo_mhz[i]) < 39.0) {
				t[n++] = (struct tone){.mhz = mhz[k] - plan.lo_mhz[i],
						       .dbfs = dbfs[k]};
			}
		}
		synth(cap, t, n, 1.0, 20, -18);
		run_capture(cap, 63);
		ubt_specan_collect(&sa, &plan, i, &c, sweep_raw);
		if (i == 0U) {
			/* reading the channel at LO 0 from capture 0 would give the spur */
			zassert_true(chan_dbfs(&sa, 0, &c) > -20.0,
				     "the spur is in the capture: %f", chan_dbfs(&sa, 0, &c));
		}
	}
	/* the channel at the first LO comes from the second placement, with the wanted level */
	zassert_equal(ubt_specan_lo_for(&plan, plan.lo_mhz[0]), 1);
	zassert_within(sweep_raw[plan.lo_mhz[0] - 2402U], -36.0 + FS_DBM + 54.0, 1.0,
		       "filled channel %d", sweep_raw[plan.lo_mhz[0] - 2402U]);
	/* ...and the spur of the second one is not shown at its LO (it is outside the wanted tone's
	 * channel, +0.2 MHz: the real second signal is at -50 dBFS there; the other placement has
	 * no leak)
	 */
	zassert_within(sweep_raw[plan.lo_mhz[1] - 2402U], -50.0 + FS_DBM + 54.0, 1.5,
		       "second filled channel %d", sweep_raw[plan.lo_mhz[1] - 2402U]);
}

/* --------------------------------------------------------------- packets */

static usb_pkt_rx got[32];
static unsigned int ngot;
static uint8_t clk_hi;
static uint32_t clk_ns;

static void clock_cb(void *user, uint8_t *hi, uint32_t *ns)
{
	ARG_UNUSED(user);
	*hi = clk_hi;
	*ns = clk_ns;
	clk_ns += 5000U;
}

static void emit_cb(void *user, const usb_pkt_rx *p)
{
	ARG_UNUSED(user);
	if (ngot < ARRAY_SIZE(got)) {
		got[ngot++] = *p;
	}
}

ZTEST(ubertooth_specan, test_packets_as_the_host_reads_them)
{
	struct ubt_specan_packer pk;
	int8_t raw[79];
	unsigned int seen = 0, expect_f = 2402;

	for (unsigned int i = 0; i < 79U; i++) {
		raw[i] = (int8_t)((int)i * 3 - 128);
	}
	ngot = 0;
	clk_hi = 0x12;
	clk_ns = 1000;
	ubt_specan_packer_init(&pk, clock_cb, emit_cb, NULL);
	/* three sweeps back to back: the 16 triple packets cut across the sweeps like upstream's
	 * counter */
	for (unsigned int s = 0; s < 3U; s++) {
		ubt_specan_pack_sweep(&pk, 2402, 2480, raw);
	}
	zassert_equal(ngot, 3U * 79U / 16U, "237 triples make 14 packets");
	zassert_equal(pk.n, (3U * 79U) % 16U);
	for (unsigned int i = 0; i < ngot; i++) {
		const usb_pkt_rx *p = &got[i];

		zassert_equal(sizeof(*p), 64U);
		zassert_equal(p->pkt_type, SPECAN);
		zassert_equal(p->status, 0);
		zassert_equal(p->channel, 0);
		zassert_equal(p->clkn_high, 0x12);
		zassert_equal(p->clk100ns, 1000U + 5000U * i, "clock at completion");
		zassert_equal(p->rssi_max, 0);
		zassert_equal(p->rssi_count, 0);
		zassert_equal(p->data[48], 0, "unused bytes are zero");
		zassert_equal(p->data[49], 0);
		/* cb_specan() of ubertooth-specan.c: for (j = 0; j < DMA_SIZE - 2; j += 3) */
		for (unsigned int j = 0; j < DMA_SIZE - 2U; j += 3U) {
			uint16_t frequency = (uint16_t)((p->data[j] << 8) | p->data[j + 1U]);
			int8_t rssi = (int8_t)p->data[j + 2U];

			zassert_equal(frequency, expect_f, "triple %u of packet %u", j / 3U, i);
			zassert_equal(rssi, raw[expect_f - 2402U]);
			expect_f = expect_f == 2480U ? 2402U : expect_f + 1U;
			seen++;
		}
	}
	zassert_equal(seen, 14U * 16U);
	/* byte layout of one triple: 2441 = 0x0989, rssi -54 = 0xca */
	{
		struct ubt_specan_packer p2;

		ngot = 0;
		ubt_specan_packer_init(&p2, NULL, emit_cb, NULL);
		for (unsigned int i = 0; i < 16U; i++) {
			ubt_specan_pack(&p2, 2441, -54);
		}
		zassert_equal(ngot, 1U);
		zassert_equal(got[0].data[0], 0x09);
		zassert_equal(got[0].data[1], 0x89);
		zassert_equal(got[0].data[2], 0xca);
		zassert_equal(got[0].clk100ns, 0U, "no clock callback: zero");
	}
}

ZTEST(ubertooth_specan, test_sweep_through_the_packer_is_ascending_and_complete)
{
	struct ubt_specan_packer pk;
	struct ubt_specan_cal c = cal(false);
	unsigned int assigned = 0, expect_f = 2402, seen = 0;
	struct tone t = {.mhz = 5.0, .dbfs = -30.0};

	zassert_equal(ubt_specan_plan(&plan, 2402, 2480, NULL), 0);
	memset(sweep_raw, 0x55, sizeof(sweep_raw));
	for (unsigned int i = 0; i < plan.n_lo; i++) {
		synth(cap, &t, 1, 1.0, 3, 3);
		run_capture(cap, 32);
		assigned += ubt_specan_collect(&sa, &plan, i, &c, sweep_raw);
	}
	zassert_equal(assigned, 79U, "every channel read exactly once");
	for (unsigned int f = 2402; f <= 2480U; f++) {
		zassert_true(sweep_raw[f - 2402U] != 0x55, "channel %u was read", f);
	}
	ngot = 0;
	ubt_specan_packer_init(&pk, NULL, emit_cb, NULL);
	ubt_specan_pack_sweep(&pk, 2402, 2480, sweep_raw);
	ubt_specan_pack_sweep(&pk, 2402, 2480, sweep_raw);
	zassert_equal(ngot, 158U / 16U);
	for (unsigned int i = 0; i < ngot; i++) {
		for (unsigned int j = 0; j < 48U; j += 3U) {
			unsigned int f =
				(unsigned int)((got[i].data[j] << 8) | got[i].data[j + 1U]);

			zassert_equal(f, expect_f, "packet %u triple %u", i, j / 3U);
			expect_f = expect_f == 2480U ? 2402U : expect_f + 1U;
			seen++;
		}
	}
	zassert_equal(seen, 144U);
}

ZTEST(ubertooth_specan, test_mirrored_spectrum_flag)
{
	struct ubt_specan_cal c = cal(false), m = cal(false);
	struct tone t = {.mhz = 13.0, .dbfs = -25.0};

	m.mirror = true;
	synth(cap, &t, 1, 1.0, 0, 0);
	run_capture(cap, 63);
	zassert_within(chan_dbfs(&sa, 13, &c), -25.0, 0.5);
	zassert_true(chan_dbfs(&sa, -13, &c) < -65.0);
	zassert_within(chan_dbfs(&sa, -13, &m), -25.0, 0.5, "mirrored reads the other side");
	zassert_true(chan_dbfs(&sa, 13, &m) < -65.0);
}

/* -------------------------------------------------------- cost estimates */

ZTEST(ubertooth_specan, test_sweep_rate_model)
{
	/*
	 * Per frame, from the instruction counts of the C loops at 240 MHz (one instruction per
	 * cycle, so these are estimates): decode 20 instructions per sample = 22 us, accumulate 10
	 * per bin = 11 us; the FFT 16 us (7 us per 128 points measured on the vector unit, scaled
	 * by N log N).
	 */
	struct ubt_specan_model m = {.retune_us = 3500,
				     .capture_us = 205,
				     .decode_us = 22,
				     .fft_us = 16,
				     .accum_us = 11,
				     .sweep_us = 150,
				     .overlap = true};
	const unsigned int frames[] = {32, 63};

	TC_PRINT("placements frames  overlapped          serial   (sweeps per second)\n");
	for (unsigned int n = 2; n <= 3; n++) {
		for (unsigned int f = 0; f < 2U; f++) {
			uint32_t ov, ser;

			m.overlap = true;
			ov = ubt_specan_sweep_us(n, frames[f], &m);
			m.overlap = false;
			ser = ubt_specan_sweep_us(n, frames[f], &m);
			TC_PRINT("%u %u  %u us %.0f/s   %u us %.0f/s\n", n, frames[f], ov, 1e6 / ov,
				 ser, 1e6 / ser);
		}
	}
	m.overlap = true;
	zassert_equal(ubt_specan_sweep_us(2, 63, &m), 2U * 3705U, "retune bound: 7.4 ms");
	zassert_equal(ubt_specan_sweep_us(3, 32, &m), 3U * 3705U);
	m.overlap = false;
	zassert_equal(ubt_specan_sweep_us(2, 63, &m), 2U * (3705U + 63U * 49U) + 150U);
	/* DSP bound when the retune is short */
	m.overlap = true;
	m.retune_us = 500;
	zassert_equal(ubt_specan_sweep_us(2, 63, &m), 2U * 63U * 49U);
}

ZTEST(ubertooth_specan, test_input_gain_headroom)
{
	struct ubt_specan_cal c = cal(false);
	struct tone t = {.mhz = 9.0, .dbfs = -0.1};

	/* a tone at full scale for 63 frames does not overflow the accumulator at shift 4 or 5 */
	synth(cap, &t, 1, 0.0, 0, 0);
	for (unsigned int shift = 4; shift <= 5U; shift++) {
		uint64_t peak = 0;

		ubt_specan_init(&sa, ubt_specan_fft_ref, shift);
		zassert_equal(ubt_specan_capture(&sa, cap, CAP, 256, 63), 63U);
		for (unsigned int k = 0; k < UBT_SPECAN_N; k++) {
			peak = MAX(peak, sa.acc[k]);
		}
		TC_PRINT("shift %u: largest accumulator %llu of 4294967295\n", shift,
			 (unsigned long long)peak);
		zassert_true(peak < 4294967295ULL);
		zassert_within(chan_dbfs(&sa, 9, &c), -0.1, 0.6, "full scale tone at shift %u",
			       shift);
		/* more than 63 frames are refused: no overflow */
		zassert_equal(ubt_specan_capture(&sa, cap, CAP, 128, 63), 0U);
	}
	/* 128 stride, 50 % overlap: 127 frames of a capture, capped at 63 */
	ubt_specan_init(&sa, ubt_specan_fft_ref, 4);
	zassert_equal(ubt_specan_capture(&sa, cap, CAP, 128, 32), 32U);
	zassert_equal(sa.frames, 32U);
	zassert_within(chan_dbfs(&sa, 9, &c), -0.1, 0.6);
}
