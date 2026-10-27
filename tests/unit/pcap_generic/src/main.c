/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit test of the generic burst analysis of apps/pcap (apps/pcap/src/dec_generic.c) and of its
 * refinement (apps/pcap/src/refine.c). The refinement alone: a tone at an offset mixed to DC, the
 * half-band stages' pass band (within 0.5 dB) and the images they push down (40 dB or more), the
 * view of the result, the noise it passes, in place. The analysis: synthetic bursts at 4 and
 * 2 MS/s, AWGN at a given SNR over the rate's band, a carrier offset and noise before and after
 * the signal: 2FSK, GFSK (BLE-like), 4FSK, OOK, DBPSK (rectangular pulses), MSK, CW, noise only
 * and a 100 kHz wide FSK that must refine; the class, the symbol rate within 5 %, the bits
 * against the transmitted ones, a summary table; the record's fields, the arguments, a burst cut
 * by its end and the cost a sample. Runs on native_sim.
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <esp_sdr/esp_sdr_dsp.h>
#include <test_rng.h>

#include "dec_util.h"
#include "decode.h"
#include "refine.h"

#define RNG_SEED 0x2545F4914F6CDD1DULL

/* Samples a burst holds at most; the signal's amplitude (int16 units, -12 dBFS); noise before
 * and after the signal; the detector's loud part reaches this far beyond the signal. */
#define CAP     32768U
#define AMP     4096.0
#define LEAD    1000U
#define TAIL    1000U
#define MARGIN  8U
#define RATE_4M 4000000U
#define RATE_2M 2000000U

uint64_t test_host_ns(void);
uint64_t test_host_cycles(void);

static int16_t iq[2U * CAP] __aligned(4);
static int16_t out_iq[2U * CAP] __aligned(4);
static uint8_t scratch[192U * 1024U] __aligned(8);
static struct dec_generic_out res;

/* The transmitted bits (two a symbol for 4FSK), their count. */
static uint8_t truth[4096];
static uint32_t ntruth;
static double level[2048];

/* ---- signals ---- */

enum kind {
	K_FSK,
	K_GFSK,
	K_OOK,
	K_DBPSK,
	K_CW,
	K_NOISE,
};

struct sig {
	const char *name;
	enum kind kind;
	/* FSK: tones; symbol rate, outer deviation (Hz), Gaussian BT (GFSK), carrier offset. */
	unsigned int tones;
	double baud, dev, bt, offset;
	/* Symbols (CW and noise: samples). */
	uint32_t nsym;
	/* The class expected; bits compared. */
	uint8_t mod;
	bool bits;
};

static const struct sig s_2fsk = {"2FSK 100k", K_FSK, 2, 100e3, 50e3, 0, 300e3, 600,
				  PCAP_MOD_2FSK, true};
static const struct sig s_gfsk = {"GFSK 1M", K_GFSK, 2, 1e6, 250e3, 0.5, 50e3, 600,
				  PCAP_MOD_2FSK, false};
static const struct sig s_4fsk = {"4FSK 50k", K_FSK, 4, 50e3, 75e3, 0, -200e3, 300,
				  PCAP_MOD_4FSK, true};
static const struct sig s_ook = {"OOK 10k", K_OOK, 2, 10e3, 0, 0, 150e3, 72, PCAP_MOD_OOK, true};
static const struct sig s_dbpsk = {"DBPSK 250k", K_DBPSK, 2, 250e3, 0, 0, -100e3, 1000,
				   PCAP_MOD_BPSK, true};
static const struct sig s_msk = {"MSK 500k", K_FSK, 2, 500e3, 125e3, 0, 100e3, 1000, PCAP_MOD_MSK,
				 true};
static const struct sig s_cw = {"CW", K_CW, 0, 0, 0, 0, 400e3, 24000, PCAP_MOD_CW, false};
static const struct sig s_noise = {"noise", K_NOISE, 0, 0, 0, 0, 0, 30000, PCAP_MOD_UNKNOWN,
				   false};
/* About 100 kHz wide (Carson): 40 kbaud, 30 kHz deviation. */
static const struct sig s_wide = {"FSK 100 kHz", K_FSK, 2, 40e3, 30e3, 0, 250e3, 300,
				  PCAP_MOD_2FSK, true};

static int16_t to16(double v)
{
	long r = lrint(v);

	return (int16_t)(r > INT16_MAX ? INT16_MAX : (r < INT16_MIN ? INT16_MIN : r));
}

/* test_rng after a few draws: the first ones of nearby seeds are alike. */
static void rng_start(uint64_t seed)
{
	test_rng_seed(seed);
	for (int i = 0; i < 16; i++) {
		(void)test_rng_u32();
	}
}

/* A Gaussian filtered rectangular frequency pulse of one symbol, at t symbols from its centre. */
static double gauss_pulse(double t, double bt)
{
	double s = sqrt(log(2.0)) / (ESP_SDR_TAU * bt) * sqrt(2.0);

	return 0.5 * (erf((t + 0.5) / s) - erf((t - 0.5) / s));
}

/*
 * A burst of @p s at @p rate: LEAD samples of noise, the signal (its first sample a third of a
 * sample into the first symbol), TAIL samples of noise (@p tail: fewer, 0 to cut the signal at
 * the burst's end), complex AWGN at @p snr_db against the amplitude over the whole band. The
 * detector's loud part reaches MARGIN samples beyond the signal; its floor is the noise's power.
 * The bits go to truth[] (DBPSK: from the second symbol on, as differences).
 */
static void make(struct burst *b, const struct sig *s, uint32_t rate, double snr_db, uint64_t seed,
		 uint32_t tail)
{
	const double sigma = AMP * pow(10.0, -snr_db / 20.0), frac = 0.3;
	uint32_t ns, n, i0 = LEAD;
	double phase = 0.0, prev = 0.0;
	int ksym = -1;

	rng_start(seed);
	ntruth = 0U;
	for (uint32_t k = 0; k < s->nsym && k < ARRAY_SIZE(level); k++) {
		uint32_t r = test_rng_u32();
		bool b0 = (r & 1U) != 0U, b1 = (r & 2U) != 0U;

		if (s->kind == K_OOK && (k == 0U || k + 1U == s->nsym)) {
			/* On at both ends: the burst's extent is the signal's. */
			b0 = true;
		}
		if (s->kind == K_FSK && s->tones == 4U) {
			/* Gray: the sign, then 1 for an inner tone. */
			level[k] = (b0 ? 1.0 : -1.0) * (b1 ? 1.0 / 3.0 : 1.0);
			truth[ntruth++] = b0;
			truth[ntruth++] = b1;
		} else {
			level[k] = b0 ? 1.0 : -1.0;
			if (s->kind != K_DBPSK || k > 0U) {
				truth[ntruth++] = b0;
			}
		}
	}
	if (s->kind == K_CW || s->kind == K_NOISE) {
		ns = s->nsym;
	} else {
		ns = (uint32_t)ceil(s->nsym * (double)rate / s->baud);
	}
	n = LEAD + ns + tail;
	zassert_true(n <= CAP, "burst too long");
	for (uint32_t i = 0; i < n; i++) {
		double re = 0.0, im = 0.0;

		if (i >= i0 && i < i0 + ns) {
			double t = (i - i0 + frac) / rate, ts = t * s->baud, a = 1.0, f = 0.0;
			int k = (int)floor(ts);

			k = k >= (int)s->nsym ? (int)s->nsym - 1 : k;
			switch (s->kind) {
			case K_FSK: {
				/* Continuous phase, exact over the sample interval for rectangular
				 * pulses. */
				double u = i == i0 ? ts : prev;

				while (u < ts) {
					int kk = (int)floor(u);
					double next = fmin(ts, kk + 1.0);

					kk = kk >= (int)s->nsym ? (int)s->nsym - 1 : kk;
					f += level[kk] * (next - u);
					u = next;
				}
				phase += ESP_SDR_TAU * s->dev * f / s->baud;
				break;
			}
			case K_GFSK: {
				double u0 = i == i0 ? ts : prev;

				/* The frequency integrated over the interval in eight steps. */
				for (int q = 0; q < 8; q++) {
					double u = u0 + (ts - u0) * (q + 0.5) / 8.0, g = 0.0;
					int kc = (int)floor(u), j0 = MAX(kc - 3, 0);
					int j1 = MIN(kc + 3, (int)s->nsym - 1);

					for (int j = j0; j <= j1; j++) {
						g += level[j] * gauss_pulse(u - (j + 0.5), s->bt);
					}
					g *= (ts - u0) / (8.0 * s->baud);
					phase += ESP_SDR_TAU * s->dev * g;
				}
				break;
			}
			case K_OOK:
				a = level[k] > 0.0 ? 1.0 : 0.0;
				break;
			case K_DBPSK:
				if (k != ksym) {
					/* A phase step of pi for a 1, from the second symbol on. */
					phase += k > 0 && level[k] > 0.0 ? ESP_SDR_TAU / 2.0 : 0.0;
					ksym = k;
				}
				break;
			case K_CW:
				break;
			case K_NOISE:
				a = 0.0;
				break;
			}
			prev = ts;
			re = AMP * a * cos(phase + ESP_SDR_TAU * s->offset * t);
			im = AMP * a * sin(phase + ESP_SDR_TAU * s->offset * t);
		}
		iq[2U * i] = to16(re + sigma / sqrt(2.0) * test_rng_gauss());
		iq[2U * i + 1U] = to16(im + sigma / sqrt(2.0) * test_rng_gauss());
	}
	memset(b, 0, sizeof(*b));
	b->chan = 7U;
	b->channels = 40U;
	b->rate_hz = rate;
	b->freq_hz = 2440000000ULL;
	b->n = n;
	b->loud_from = s->kind == K_NOISE ? 0U : i0 - MARGIN;
	b->loud_to = s->kind == K_NOISE ? n - 1U : MIN(i0 + ns + MARGIN, n - 1U);
	b->floor = (float)(sigma * sigma);
	b->power = (float)(AMP * AMP + sigma * sigma);
	b->iq = iq;
}

/*
 * The decoded bits against the transmitted ones at the best alignment (shifts up to 24): the best
 * agreement over a stretch of 64 bits, the agreement over all in *all.
 */
static double bits_match(const struct dec_generic_out *o, double *all)
{
	uint32_t ng = o->hdr.nbits;
	double best = 0.0;
	int shift = 0;

	*all = 0.0;
	for (int sh = -24; sh <= 24; sh++) {
		uint32_t agree = 0U, cnt = 0U;

		for (uint32_t i = 0; i < ng; i++) {
			int j = (int)i + sh;

			if (j >= 0 && j < (int)ntruth) {
				agree += ((o->bits[i / 8U] >> (7U - i % 8U)) & 1U) == truth[j];
				cnt++;
			}
		}
		if (cnt >= 64U && (double)agree / cnt > *all) {
			*all = (double)agree / cnt;
			shift = sh;
		}
	}
	for (uint32_t st = 0; st + 64U <= ng; st++) {
		uint32_t agree = 0U, cnt = 0U;

		for (uint32_t i = st; i < st + 64U; i++) {
			int j = (int)i + shift;

			if (j >= 0 && j < (int)ntruth) {
				agree += ((o->bits[i / 8U] >> (7U - i % 8U)) & 1U) == truth[j];
				cnt++;
			}
		}
		if (cnt == 64U && agree / 64.0 > best) {
			best = agree / 64.0;
		}
	}
	return best;
}

static const char *mod_name(uint8_t m)
{
	static const char *const names[] = {"UNKNOWN", "CW",   "OOK",  "2FSK",    "4FSK",
					    "MSK",     "BPSK", "QPSK", "WIDEBAND"};

	return m < ARRAY_SIZE(names) ? names[m] : "?";
}

static void table_head(void)
{
	printk("%-12s %4s %5s %-8s %8s %7s %7s %9s %8s %8s %5s %6s\n", "case", "MS/s", "SNR",
	       "class", "rate", "err %", "dev", "off err", "bw", "rate out", "bits", "bits64");
}

/*
 * One burst through the analysis, checked: the class, the symbol rate within 5 %, the bits
 * (at least 95 % right over 64 of them, none for a class without), the table's line.
 */
static void check(const struct sig *s, uint32_t rate, double snr_db, uint64_t seed)
{
	struct dec_generic_cfg cfg = {.max_bits = PCAP_BURST_BITS_MAX};
	double err = 0.0, b64 = 0.0, all = 0.0;
	struct burst b;
	int r;

	make(&b, s, rate, snr_db, seed, TAIL);
	r = dec_generic_run(&b, &cfg, scratch, sizeof(scratch), &res);
	zassert_equal(r, 0, "%s: %d", s->name, r);
	if (res.hdr.symbol_rate != 0U && s->baud > 0.0) {
		err = 100.0 * (res.hdr.symbol_rate - s->baud) / s->baud;
	}
	if (res.hdr.nbits > 0U) {
		b64 = bits_match(&res, &all);
	}
	printk("%-12s %4u %5.1f %-8s %8u %+7.2f %7u %+9d %8u %8u %5u %6.3f\n", s->name,
	       rate / 1000000U, snr_db, mod_name(res.hdr.modulation), res.hdr.symbol_rate, err,
	       res.hdr.deviation_hz, res.hdr.offset_hz - (int32_t)s->offset, res.hdr.bandwidth_hz,
	       res.hdr.sample_rate_hz, res.hdr.nbits, b64);
	zassert_equal((res.hdr.flags & PCAP_BF_BITS) != 0U, res.hdr.nbits > 0U, "%s: BITS flag",
		      s->name);
	if (s->kind == K_NOISE) {
		zassert_true(res.hdr.modulation == PCAP_MOD_UNKNOWN ||
				     res.hdr.modulation == PCAP_MOD_WIDEBAND,
			     "noise as %s", mod_name(res.hdr.modulation));
		zassert_equal(res.hdr.nbits, 0U, "bits from noise");
		return;
	}
	zassert_equal(res.hdr.modulation, s->mod, "%s at %u MS/s, %.0f dB: %s", s->name,
		      rate / 1000000U, snr_db, mod_name(res.hdr.modulation));
	if (s->kind == K_CW) {
		zassert_equal(res.hdr.nbits, 0U, "bits from a carrier");
		zassert_true(abs(res.hdr.offset_hz - (int32_t)s->offset) < 100, "CW offset %d",
			     res.hdr.offset_hz);
		return;
	}
	zassert_true(fabs(err) <= 5.0 && res.hdr.symbol_rate != 0U, "%s: rate %u", s->name,
		     res.hdr.symbol_rate);
	if (s->bits) {
		zassert_true(b64 >= 0.95, "%s at %u MS/s, %.0f dB: bits %.3f", s->name,
			     rate / 1000000U, snr_db, b64);
	}
}

/* ---- refinement ---- */

/* The amplitude of the component at @p hz of the middle half of a stream at @p rate. */
static double tone_amp(const int16_t *x, uint32_t n, double rate, double hz)
{
	double re = 0.0, im = 0.0;
	uint32_t from = n / 4U, to = n - n / 4U;

	for (uint32_t k = from; k < to; k++) {
		double ph = -ESP_SDR_TAU * hz * k / rate;

		re += x[2U * k] * cos(ph) - x[2U * k + 1U] * sin(ph);
		im += x[2U * k] * sin(ph) + x[2U * k + 1U] * cos(ph);
	}
	return sqrt(re * re + im * im) / (to - from);
}

/* @p n samples of a tone of amplitude @p amp at @p hz (phase 0 at the first sample). */
static void tone(struct burst *b, uint32_t n, uint32_t rate, double hz, double amp)
{
	for (uint32_t k = 0; k < n; k++) {
		double ph = ESP_SDR_TAU * hz * k / rate;

		iq[2U * k] = to16(amp * cos(ph));
		iq[2U * k + 1U] = to16(amp * sin(ph));
	}
	memset(b, 0, sizeof(*b));
	b->rate_hz = rate;
	b->freq_hz = 2440000000ULL;
	b->n = n;
	b->loud_to = n - 1U;
	b->iq = iq;
}

static void *suite_setup(void)
{
	zassert_true(dec_generic_scratch(CAP) <= sizeof(scratch), "scratch: %u bytes needed",
		     (unsigned int)dec_generic_scratch(CAP));
	return NULL;
}

ZTEST_SUITE(pcap_generic, NULL, suite_setup, NULL, NULL, NULL);

ZTEST(pcap_generic, test_refine_decim)
{
	zassert_equal(refine_decim(RATE_4M, 100000U), 8U);
	zassert_equal(refine_decim(RATE_4M, 500000U), 2U);
	zassert_equal(refine_decim(RATE_4M, 499999U), 2U);
	zassert_equal(refine_decim(RATE_4M, 1000000U), 1U);
	zassert_equal(refine_decim(RATE_4M, 3000000U), 1U);
	zassert_equal(refine_decim(RATE_4M, 10000U), 64U);
	zassert_equal(refine_decim(RATE_4M, 1U), REFINE_DECIM_MAX);
	zassert_equal(refine_decim(RATE_4M, 0U), 1U);
	zassert_equal(refine_decim(RATE_2M, 100000U), 4U);
	zassert_equal(refine_decim(62500U, 10000U), 1U);
}

ZTEST(pcap_generic, test_refine_arguments)
{
	struct burst b, v;
	uint8_t small[16];

	tone(&b, 1024U, RATE_4M, 0.0, 1000.0);
	zassert_equal(refine_run(&b, 0, 3U, out_iq, scratch, sizeof(scratch), &v), -EINVAL);
	zassert_equal(refine_run(&b, 0, 0U, out_iq, scratch, sizeof(scratch), &v), -EINVAL);
	zassert_equal(refine_run(&b, 0, 128U, out_iq, scratch, sizeof(scratch), &v), -EINVAL);
	zassert_equal(refine_run(NULL, 0, 2U, out_iq, scratch, sizeof(scratch), &v), -EINVAL);
	zassert_equal(refine_run(&b, 0, 2U, NULL, scratch, sizeof(scratch), &v), -EINVAL);
	zassert_equal(refine_run(&b, 0, 2U, out_iq, scratch, sizeof(scratch), NULL), -EINVAL);
	zassert_equal(refine_run(&b, 0, 2U, out_iq, small, sizeof(small), &v), -ENOMEM);
	b.rate_hz = 0U;
	zassert_equal(refine_run(&b, 0, 2U, out_iq, scratch, sizeof(scratch), &v), -EINVAL);
}

/* A tone at an offset is at DC after the mix, its amplitude kept, with and without decimation. */
ZTEST(pcap_generic, test_refine_mix)
{
	static const unsigned int decims[] = {1U, 8U, 64U};
	const double hz = 312345.0, amp = 8000.0;

	for (size_t d = 0; d < ARRAY_SIZE(decims); d++) {
		struct burst b, v;
		double re0 = 0.0, im0 = 0.0, re1 = 0.0, im1 = 0.0, f, a;
		uint32_t q;
		int r;

		tone(&b, CAP, RATE_4M, hz, amp);
		r = refine_run(&b, (int32_t)hz, decims[d], out_iq, scratch, sizeof(scratch), &v);
		zassert_equal(r, 0);
		/* The phase from the second quarter to the third: its drift is the residual
		 * frequency. */
		q = v.n / 4U;
		for (uint32_t k = q; k < 2U * q; k++) {
			re0 += out_iq[2U * k];
			im0 += out_iq[2U * k + 1U];
			re1 += out_iq[2U * (k + q)];
			im1 += out_iq[2U * (k + q) + 1U];
		}
		f = atan2(im1 * re0 - re1 * im0, re1 * re0 + im1 * im0) / ESP_SDR_TAU;
		f *= (double)v.rate_hz / q;
		a = tone_amp(out_iq, v.n, v.rate_hz, 0.0);
		printk("refine mix: %+.0f Hz, decim %2u: residual %+.3f Hz, amplitude %.1f dB\n",
		       hz, decims[d], f, 20.0 * log10(a / amp));
		zassert_true(fabs(f) < 1.0, "residual %.3f Hz at decim %u", f, decims[d]);
		zassert_true(fabs(20.0 * log10(a / amp)) < 0.5, "amplitude at decim %u", decims[d]);
	}
}

/*
 * The half-band stages: a tone in the pass band (up to a quarter of the output rate) kept within
 * 0.5 dB; an image, a tone beyond the new Nyquist frequency that folds onto the pass band, pushed
 * down by 40 dB or more. Every decimation from 2 to REFINE_DECIM_MAX at 4 MS/s.
 */
ZTEST(pcap_generic, test_refine_bands)
{
	const double amp = 8000.0;

	for (unsigned int decim = 2U; decim <= REFINE_DECIM_MAX; decim *= 2U) {
		const double ro = (double)RATE_4M / decim;
		double worst_pass = 0.0, worst_image = -200.0;
		int r;

		for (int p = -2; p <= 2; p++) {
			/* Pass band: 0, +-1/8, +-1/4 of the output rate. */
			double fp = p * ro / 8.0, g;
			struct burst b, v;

			tone(&b, CAP, RATE_4M, fp, amp);
			r = refine_run(&b, 0, decim, out_iq, scratch, sizeof(scratch), &v);
			zassert_equal(r, 0);
			g = 20.0 * log10(tone_amp(out_iq, v.n, v.rate_hz, fp) / amp);
			worst_pass = fabs(g) > fabs(worst_pass) ? g : worst_pass;
			zassert_true(fabs(g) <= 0.5, "decim %u, %.0f Hz: %.2f dB", decim, fp, g);
			for (int k = -2; k <= 2; k++) {
				double fi = fp + k * ro;

				if (k == 0 || fabs(fi) >= 0.5 * RATE_4M) {
					continue;
				}
				tone(&b, CAP, RATE_4M, fi, amp);
				r = refine_run(&b, 0, decim, out_iq, scratch, sizeof(scratch), &v);
				zassert_equal(r, 0);
				g = tone_amp(out_iq, v.n, v.rate_hz, fp) / amp + 1e-12;
				g = 20.0 * log10(g);
				worst_image = g > worst_image ? g : worst_image;
				zassert_true(g <= -40.0, "decim %u: %.0f Hz onto %.0f Hz: %.1f dB",
					     decim, fi, fp, g);
			}
		}
		printk("refine decim %2u: pass band within %+.3f dB, images at most %.1f dB\n",
		       decim, worst_pass, worst_image);
	}
}

/* The view of the result, and the noise the stages pass: the floor of the narrower stream. */
ZTEST(pcap_generic, test_refine_view)
{
	struct burst b, v;
	double var = 0.0;
	const double sigma = 1000.0;

	rng_start(RNG_SEED);
	for (uint32_t k = 0; k < 2U * CAP; k++) {
		iq[k] = to16(sigma / sqrt(2.0) * test_rng_gauss());
	}
	memset(&b, 0, sizeof(b));
	b.chan = 5U;
	b.channels = 40U;
	b.rate_hz = RATE_4M;
	b.freq_hz = 2440000000ULL;
	b.m0 = 1000U;
	b.n = CAP - 3U;
	b.loud_from = 100U;
	b.loud_to = CAP - 200U;
	b.floor = (float)(sigma * sigma);
	b.power = (float)(4.0 * sigma * sigma);
	b.flags = BURST_F_NEIGHBOUR;
	b.gain = 3U;
	b.iq = iq;
	zassert_equal(refine_run(&b, -25000, 4U, out_iq, scratch, sizeof(scratch), &v), 0);
	zassert_equal(v.rate_hz, RATE_4M / 4U);
	zassert_equal(v.freq_hz, 2440000000ULL - 25000U);
	zassert_equal(v.n, (CAP - 3U) / 4U);
	zassert_equal(v.m0, 250U);
	zassert_equal(v.loud_from, 25U);
	zassert_equal(v.loud_to, (CAP - 200U) / 4U);
	zassert_equal(v.iq, out_iq);
	zassert_equal(v.chan, 5U);
	zassert_equal(v.channels, 40U);
	zassert_equal(v.flags, BURST_F_NEIGHBOUR);
	zassert_equal(v.gain, 3U);
	for (uint32_t k = v.n / 8U; k < v.n - v.n / 8U; k++) {
		var += (double)out_iq[2U * k] * out_iq[2U * k] +
		       (double)out_iq[2U * k + 1U] * out_iq[2U * k + 1U];
	}
	var /= v.n - 2U * (v.n / 8U);
	printk("refine view: floor %.0f -> %.0f, noise measured %.0f\n", (double)b.floor,
	       (double)v.floor, var);
	zassert_true(fabs(var / (double)v.floor - 1.0) < 0.1, "floor %.0f, noise %.0f",
		     (double)v.floor, var);
	/* The power: the signal above the floor kept, the noise scaled. */
	zassert_true(fabs((double)v.power - (3.0 * sigma * sigma + (double)v.floor)) < 1.0,
		     "power %.0f", (double)v.power);
}

/* In place (the output over the input) gives what a separate buffer does. */
ZTEST(pcap_generic, test_refine_in_place)
{
	static int16_t copy[2U * 4096U];
	struct burst b, v1, v2;

	make(&b, &s_2fsk, RATE_4M, 20.0, RNG_SEED + 1U, TAIL);
	b.n = 4096U;
	zassert_equal(refine_run(&b, 300000, 8U, copy, scratch, sizeof(scratch), &v1), 0);
	zassert_equal(refine_run(&b, 300000, 8U, iq, scratch, sizeof(scratch), &v2), 0);
	zassert_equal(v1.n, v2.n);
	zassert_mem_equal(copy, iq, 4U * v1.n, "in place differs");
}

/* ---- the analysis ---- */

/* Every class at 20 dB, at 4 and 2 MS/s. */
ZTEST(pcap_generic, test_classes_20db)
{
	static const struct sig *const sigs[] = {&s_2fsk, &s_gfsk, &s_4fsk, &s_ook, &s_dbpsk,
						 &s_msk,  &s_cw,   &s_noise};
	static const uint32_t rates[] = {RATE_4M, RATE_2M};

	table_head();
	for (size_t r = 0; r < ARRAY_SIZE(rates); r++) {
		for (size_t i = 0; i < ARRAY_SIZE(sigs); i++) {
			check(sigs[i], rates[r], 20.0, RNG_SEED + 100U * r + i);
		}
	}
}

/* FSK (BLE-like GFSK too), OOK, MSK (and DBPSK) at 10 dB, at 4 and 2 MS/s. */
ZTEST(pcap_generic, test_classes_10db)
{
	static const struct sig *const sigs[] = {&s_2fsk, &s_gfsk, &s_4fsk, &s_ook,
						 &s_msk,  &s_wide, &s_dbpsk};
	static const uint32_t rates[] = {RATE_4M, RATE_2M};

	table_head();
	for (size_t r = 0; r < ARRAY_SIZE(rates); r++) {
		for (size_t i = 0; i < ARRAY_SIZE(sigs); i++) {
			check(sigs[i], rates[r], 10.0, RNG_SEED + 1000U + 100U * r + i);
		}
	}
}

/* Noise alone, of several lengths and levels: UNKNOWN or WIDEBAND, never bits. */
ZTEST(pcap_generic, test_noise_only)
{
	static const uint32_t lens[] = {64U, 400U, 3000U, 30000U};
	static const uint32_t rates[] = {RATE_4M, RATE_2M};

	for (size_t r = 0; r < ARRAY_SIZE(rates); r++) {
		for (size_t l = 0; l < ARRAY_SIZE(lens); l++) {
			for (int lvl = 0; lvl < 3; lvl++) {
				struct sig s = s_noise;
				struct burst b;
				int ret;

				s.nsym = lens[l] > LEAD + TAIL ? lens[l] - LEAD - TAIL : 0U;
				make(&b, &s, rates[r], 20.0 - 10.0 * lvl,
				     RNG_SEED + 2000U + 10U * l + r + 100U * lvl, TAIL);
				b.n = lens[l];
				b.loud_to = b.n - 1U;
				/* A floor a little off, as the detector's may be. */
				b.floor *= lvl == 1 ? 0.8f : (lvl == 2 ? 1.25f : 1.0f);
				ret = dec_generic_run(&b, NULL, scratch, sizeof(scratch), &res);
				zassert_equal(ret, 0);
				zassert_true(res.hdr.modulation == PCAP_MOD_UNKNOWN ||
						     res.hdr.modulation == PCAP_MOD_WIDEBAND,
					     "noise (%u samples) as %s", lens[l],
					     mod_name(res.hdr.modulation));
				zassert_equal(res.hdr.nbits, 0U, "bits from noise");
				zassert_equal(res.hdr.symbol_rate, 0U, "a rate from noise");
			}
		}
	}
}

/* A 100 kHz wide FSK in a 4 MS/s bin: refined to 500 or 250 kS/s, centred within 10 kHz. */
ZTEST(pcap_generic, test_refine_narrow_fsk)
{
	static const double snrs[] = {20.0, 10.0};

	table_head();
	for (size_t i = 0; i < ARRAY_SIZE(snrs); i++) {
		check(&s_wide, RATE_4M, snrs[i], RNG_SEED + 3000U + i);
		zassert_true(res.hdr.sample_rate_hz == 500000U || res.hdr.sample_rate_hz == 250000U,
			     "refined to %u S/s", res.hdr.sample_rate_hz);
		zassert_true(abs(res.hdr.offset_hz - (int32_t)s_wide.offset) <= 10000,
			     "centre %d Hz", res.hdr.offset_hz);
	}
}

/* The record's fields: copied, mapped, measured; the bits capped. */
ZTEST(pcap_generic, test_record)
{
	static const uint16_t fmap[][2] = {
		{BURST_F_TRUNCATED, PCAP_BF_TRUNCATED}, {BURST_F_WIDEBAND, PCAP_BF_WIDEBAND},
		{BURST_F_TRANSIENT, PCAP_BF_TRANSIENT}, {BURST_F_NEIGHBOUR, PCAP_BF_NEIGHBOUR},
		{BURST_F_DROPPED, PCAP_BF_DROPPED},     {BURST_F_CLIPPED, PCAP_BF_CLIPPED},
		{BURST_F_CUT, PCAP_BF_CUT},             {BURST_F_HOLE, PCAP_BF_HOLE},
	};
	struct dec_generic_cfg cfg = {.max_bits = 100U};
	const double snr = 20.0, sigma2 = AMP * AMP * pow(10.0, -snr / 10.0);
	struct burst b;
	double dur;
	uint16_t want = 0U;

	make(&b, &s_2fsk, RATE_4M, snr, RNG_SEED + 4000U, TAIL);
	b.gain = 7U;
	b.chan = 33U;
	b.channels = 40U;
	for (size_t i = 0; i < ARRAY_SIZE(fmap); i++) {
		b.flags |= fmap[i][0];
		want |= fmap[i][1];
	}
	/* Unknown bits and the decoder thread's own map to nothing. */
	b.flags |= 0x8000U | BURST_F_CLAIMED;
	zassert_equal(dec_generic_run(&b, &cfg, scratch, sizeof(scratch), &res), 0);
	zassert_equal(res.hdr.version, 0U);
	zassert_equal(res.hdr.hdr_len, 0U);
	zassert_equal(res.hdr.freq_hz, b.freq_hz);
	zassert_equal(res.hdr.rx_gain, 7U);
	zassert_equal(res.hdr.channel, 33U);
	zassert_equal(res.hdr.channels, 40U);
	zassert_equal(res.hdr.flags, want | PCAP_BF_BITS, "flags %04x", res.hdr.flags);
	zassert_equal(res.hdr.modulation, PCAP_MOD_2FSK);
	zassert_equal(res.hdr.nbits, 100U, "nbits %u", res.hdr.nbits);
	for (uint32_t i = 100U / 8U + 1U; i < sizeof(res.bits); i++) {
		zassert_equal(res.bits[i], 0U, "bits beyond nbits");
	}
	zassert_equal(res.bits[100U / 8U] & (0xFFU >> (100U % 8U)), 0U, "padding");
	zassert_equal(res.hdr.floor_dbfs_x10, dec_dbfs_x10(b.floor));
	zassert_true(abs(res.hdr.power_dbfs_x10 -
			 dec_dbfs_x10((float)(AMP * AMP + sigma2))) <= 5,
		     "power %d", res.hdr.power_dbfs_x10);
	zassert_true(res.hdr.peak_dbfs_x10 > res.hdr.power_dbfs_x10);
	/* The loud part trimmed to the signal: its start and length within a few samples. */
	dur = s_2fsk.nsym / s_2fsk.baud * 1e6;
	zassert_true(fabs(res.hdr.duration_us - dur) <= 5.0, "duration %u us, %.0f sent",
		     res.hdr.duration_us, dur);
	zassert_true(abs((int)res.start - (int)LEAD) <= 4, "start %u", res.start);
	zassert_true(abs((int)res.hdr.deviation_hz - (int)s_2fsk.dev) <= 2500, "deviation %u",
		     res.hdr.deviation_hz);
	zassert_true(res.hdr.bandwidth_hz > 2U * s_2fsk.dev && res.hdr.bandwidth_hz < 300000U,
		     "bandwidth %u", res.hdr.bandwidth_hz);
	/* Some 120 kHz wide: 1 MS/s or 500 kS/s (refine_decim()'s limit lies between). */
	zassert_true(res.hdr.sample_rate_hz == RATE_4M / 4U ||
			     res.hdr.sample_rate_hz == RATE_4M / 8U,
		     "rate %u", res.hdr.sample_rate_hz);

	/* No configuration: up to the record's cap. */
	zassert_equal(dec_generic_run(&b, NULL, scratch, sizeof(scratch), &res), 0);
	zassert_true(abs((int)res.hdr.nbits - (int)ntruth) <= 2, "nbits %u of %u", res.hdr.nbits,
		     ntruth);
}

ZTEST(pcap_generic, test_arguments)
{
	struct burst b;

	make(&b, &s_2fsk, RATE_4M, 20.0, RNG_SEED, TAIL);
	zassert_equal(dec_generic_run(NULL, NULL, scratch, sizeof(scratch), &res), -EINVAL);
	zassert_equal(dec_generic_run(&b, NULL, scratch, sizeof(scratch), NULL), -EINVAL);
	zassert_equal(dec_generic_run(&b, NULL, scratch, dec_generic_scratch(b.n) - 1U, &res),
		      -ENOMEM);
	zassert_equal(dec_generic_run(&b, NULL, NULL, sizeof(scratch), &res), -ENOMEM);
	zassert_true(dec_generic_scratch(1000U) < dec_generic_scratch(2000U));

	/* No samples (a wideband record): the metadata alone. */
	memset(&b, 0, sizeof(b));
	b.rate_hz = RATE_4M;
	b.freq_hz = 2450000000ULL;
	b.flags = BURST_F_WIDEBAND;
	b.loud_from = 0U;
	b.loud_to = 3999U;
	b.power = 1e6f;
	b.floor = 1e4f;
	zassert_equal(dec_generic_run(&b, NULL, NULL, 0U, &res), 0);
	zassert_equal(res.hdr.modulation, PCAP_MOD_WIDEBAND);
	zassert_equal(res.hdr.flags, PCAP_BF_WIDEBAND);
	zassert_equal(res.hdr.nbits, 0U);
	zassert_equal(res.hdr.duration_us, 1000U);
	zassert_equal(res.hdr.power_dbfs_x10, dec_dbfs_x10(1e6f));
}

/* A burst cut by its end (the signal on to its last sample): analysed, its bits right. */
ZTEST(pcap_generic, test_cut)
{
	struct burst b;
	double all;

	make(&b, &s_2fsk, RATE_4M, 20.0, RNG_SEED + 5000U, 0U);
	b.flags = BURST_F_TRUNCATED;
	zassert_equal(dec_generic_run(&b, NULL, scratch, sizeof(scratch), &res), 0);
	zassert_equal(res.hdr.modulation, PCAP_MOD_2FSK, "%s", mod_name(res.hdr.modulation));
	zassert_true(res.hdr.flags & PCAP_BF_TRUNCATED);
	zassert_true(bits_match(&res, &all) >= 0.95, "bits");
}

/* The loud part's start alone analysed: the class from it, duration and power of all of it. */
ZTEST(pcap_generic, test_max_samples)
{
	static struct dec_generic_out whole;
	const struct dec_generic_cfg cfg = {.max_samples = 8000U};
	struct burst b;
	double all;

	make(&b, &s_2fsk, RATE_4M, 20.0, RNG_SEED + 6000U, TAIL);
	zassert_equal(dec_generic_run(&b, NULL, scratch, sizeof(scratch), &whole), 0);
	zassert_equal(dec_generic_run(&b, &cfg, scratch, sizeof(scratch), &res), 0);
	zassert_equal(res.hdr.modulation, PCAP_MOD_2FSK, "%s", mod_name(res.hdr.modulation));
	zassert_equal(res.hdr.duration_us, whole.hdr.duration_us);
	zassert_equal(res.hdr.power_dbfs_x10, whole.hdr.power_dbfs_x10);
	zassert_equal(res.hdr.peak_dbfs_x10, whole.hdr.peak_dbfs_x10);
	zassert_equal(res.start, whole.start);
	/* 8000 samples of 40 a symbol: some 200 bits, right. */
	zassert_true(res.hdr.nbits >= 190U && res.hdr.nbits <= 200U, "nbits %u", res.hdr.nbits);
	zassert_true(bits_match(&res, &all) >= 0.95, "bits");
}

/*
 * The host's time a loud sample, printed: a refined FSK, BLE-like GFSK and DBPSK at 4 MS/s,
 * noise, and two bursts of 200 us (the survey's: the C6 keeps some 200 us of a burst).
 */
ZTEST(pcap_generic, test_cost)
{
#if defined(CONFIG_ARCH_POSIX)
	static const struct {
		const struct sig *s;
		uint32_t nsym;
	} cases[] = {
		{&s_2fsk, 0U}, {&s_gfsk, 0U}, {&s_dbpsk, 0U}, {&s_noise, 0U}, {&s_2fsk, 20U},
		{&s_dbpsk, 50U},
	};
	const unsigned int reps = 10U;

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		struct sig s = *cases[i].s;
		struct burst b;
		uint64_t t0, c0, ns, cyc;
		uint32_t loud;

		s.nsym = cases[i].nsym > 0U ? cases[i].nsym : s.nsym;
		make(&b, &s, RATE_4M, 20.0, RNG_SEED + 6000U + i, TAIL);
		loud = b.loud_to - b.loud_from + 1U;
		/* Once untimed: caches and clock warm. */
		(void)dec_generic_run(&b, NULL, scratch, sizeof(scratch), &res);
		t0 = test_host_ns();
		c0 = test_host_cycles();
		for (unsigned int r = 0; r < reps; r++) {
			(void)dec_generic_run(&b, NULL, scratch, sizeof(scratch), &res);
		}
		ns = test_host_ns() - t0;
		cyc = test_host_cycles() - c0;
		printk("cost, %s at 4 MS/s (%u loud samples, analysed at %u S/s, %s): %.1f ns, "
		       "%.1f host cycles a loud sample\n",
		       s.name, loud, res.hdr.sample_rate_hz, mod_name(res.hdr.modulation),
		       (double)ns / reps / loud, (double)cyc / reps / loud);
	}
#else
	ztest_test_skip();
#endif
}
