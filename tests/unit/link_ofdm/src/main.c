/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit test of apps/link/src/ofdm.c and ofdm_frame.c: the layout checks, the receive FFT
 * against a direct DFT, and frames looped like the DAC plays them, cut at a random offset,
 * sent through gain, carrier offset, DC, noise, the receive decimation (at 40 MS/s) and
 * 10-bit quantization, then decoded: raw bits at each modulation, the payload split over two
 * workers, framed data with RS and CRC, header-only (ACK) frames and windows with no frame.
 * The SNRs keep a margin over where tools/ofdm_sim.c starts to see bit errors. Runs on
 * native_sim.
 */
#include <math.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <esp_sdr/esp_sdr_dsp.h>
#include <test_rng.h>

#include "ofdm.h"
#include "ofdm_frame.h"
#include "rs.h"

#define RNG_SEED 0x6a09e667f3bcc908ULL
#define FS       80000000U
#define WIN      16380U
#define TAPS     31

static struct ofdm_ctx ctx __aligned(16);
static struct ofdm_ctx ctx2 __aligned(16);
static struct ofdm_txbuf tb;
static struct ofdm_fwork fwk;
static struct ofdm_worker wk2;
static uint32_t frame[WIN], win[WIN];
static uint8_t bits[WIN], got[WIN];
static uint8_t hdr[OFDM_HDR_BITS_MAX / 8], hgot[OFDM_HDR_BITS_MAX / 8];
static double fr[2 * WIN + TAPS], fq[2 * WIN + TAPS];

/* The bench setting of apps/link (tools/ofdm_sim.c's defaults). */
static const struct ofdm_cfg cfg80 = {
	.sample_rate_hz = FS,
	.bandwidth_hz = 16000000U,
	.channels = 52,
	.cp_div = 8,
	.amp = 90,
	.max_samples = (WIN + 1U) / 2U,
	.rx_div = 1,
	.pilots = 1,
	.hdr_bits = 64,
	.mid_pilot = true,
};

static void *suite_setup(void)
{
	rs_init();
	return NULL;
}

static void before_each(void *f)
{
	ARG_UNUSED(f);
	test_rng_seed(RNG_SEED);
	zassert_equal(ofdm_init(&ctx, &cfg80), 0);
	ofdm_test_bits(hdr, cfg80.hdr_bits, 7);
}

ZTEST_SUITE(link_ofdm, NULL, suite_setup, before_each, NULL, NULL);

/*
 * Loop the frame of len samples from offset off at the transmit rate, rotate by cfo_hz, scale
 * by gain, decimate by rx_div (a Hann-windowed sinc low pass at 0.45 of the receive rate, as
 * the radio's filter), add noise for snr_db and a little DC, quantize to 10 bits.
 */
static void channel(const struct ofdm_ctx *c, size_t len, double snr_db, double cfo_hz,
		    double gain)
{
	unsigned int rx_div = c->cfg.rx_div, nfull = rx_div * WIN + TAPS;
	size_t off = test_rng_u32() % len;
	double h[TAPS], hs = 0.0, p = 0.0, ns, w = ESP_SDR_TAU * cfo_hz / FS;

	for (int k = 0; k < TAPS; k++) {
		double fc = 0.45 / rx_div;

		h[k] = 2.0 * fc * esp_sdr_sinc(2.0 * fc * (k - (TAPS - 1) / 2.0)) *
		       (double)esp_sdr_window(ESP_SDR_WINDOW_HANN, (uint32_t)k, TAPS - 1);
		hs += h[k];
	}
	for (unsigned int i = 0; i < nfull; i++) {
		uint32_t f = frame[(i + off) % len];
		double xi = (int32_t)(f << 22) >> 22, xq = (int32_t)(f << 12) >> 22;

		fr[i] = gain * (xi * cos(w * i) - xq * sin(w * i));
		fq[i] = gain * (xi * sin(w * i) + xq * cos(w * i));
	}
	for (unsigned int i = 0; i < WIN; i++) {
		double ri = 0.0, rq = 0.0;

		if (rx_div == 1U) {
			ri = fr[i];
			rq = fq[i];
		} else {
			for (int k = 0; k < TAPS; k++) {
				ri += h[k] / hs * fr[i * rx_div + k];
				rq += h[k] / hs * fq[i * rx_div + k];
			}
		}
		fr[i] = ri;
		fq[i] = rq;
		p += ri * ri + rq * rq;
	}
	ns = sqrt(p / WIN / pow(10.0, snr_db / 10.0) / 2.0);
	for (unsigned int i = 0; i < WIN; i++) {
		long qi = lround(fr[i] + ns * test_rng_gauss() + 3.0);
		long qq = lround(fq[i] + ns * test_rng_gauss() - 2.0);

		qi = CLAMP(qi, -512, 511);
		qq = CLAMP(qq, -512, 511);
		win[i] = (((uint32_t)qi & 0x3ffU) << 10) | ((uint32_t)qq & 0x3ffU);
	}
}

static unsigned int bit_errors(const uint8_t *a, const uint8_t *b, size_t nbits)
{
	unsigned int e = 0;

	for (size_t i = 0; i < nbits; i++) {
		e += ((a[i / 8U] ^ b[i / 8U]) >> (7U - i % 8U)) & 1U;
	}
	return e;
}

ZTEST(link_ofdm, test_init_layout)
{
	struct ofdm_cfg c = cfg80;

	zassert_equal(ctx.nfft, 256U, "nfft %u", ctx.nfft);
	zassert_equal(ctx.cp, ctx.nfft / 8U);
	zassert_equal(ctx.slot, (size_t)(ctx.nfft + ctx.cp));
	zassert_true(ctx.len <= cfg80.max_samples && ctx.ndata > 0U, "len %zu ndata %u",
		     ctx.len, ctx.ndata);
	zassert_true(ctx.mid > 0U && ctx.mid < ctx.ndata, "mid %u of %u", ctx.mid, ctx.ndata);
	zassert_within(ctx.spacing_hz, (float)FS / (float)ctx.nfft, 1.0f);
	zassert_equal(ofdm_frame_bits(&ctx, OFDM_64QAM), (size_t)ctx.ndata * 52U * 6U);
	zassert_equal(ofdm_mod_bits(OFDM_MODS), 0U);
	/* Settings out of range. */
	c.cp_div = 2;
	zassert_equal(ofdm_init(&ctx2, &c), -1);
	c = cfg80;
	c.pilots = 0;
	zassert_equal(ofdm_init(&ctx2, &c), -1);
	c.pilots = OFDM_PILOTS_MAX + 1U;
	zassert_equal(ofdm_init(&ctx2, &c), -1);
	c = cfg80;
	c.hdr_bits = OFDM_HDR_BITS_MAX + 1U;
	zassert_equal(ofdm_init(&ctx2, &c), -1);
	c = cfg80;
	c.channels = 1;
	zassert_equal(ofdm_init(&ctx2, &c), -1);
	c = cfg80;
	c.bandwidth_hz = 0;
	zassert_equal(ofdm_init(&ctx2, &c), -1);
}

ZTEST(link_ofdm, test_fft_matches_dft)
{
	unsigned int n = ctx.nfft;
	struct ofdm_cf x[OFDM_NFFT_MAX];
	double worst = 0.0;

	for (unsigned int i = 0; i < n; i++) {
		x[i] = (struct ofdm_cf){(float)test_rng_gauss(), (float)test_rng_gauss()};
	}
	ctx2 = ctx;
	memcpy(ctx2.w0.buf, x, n * sizeof(x[0]));
	ofdm_test_fft(&ctx2);
	for (unsigned int k = 0; k < n; k++) {
		double re = 0.0, im = 0.0;
		struct ofdm_cf y = ctx2.w0.buf[ctx2.rev[k]];

		for (unsigned int t = 0; t < n; t++) {
			double a = -ESP_SDR_TAU * (double)((k * t) % n) / (double)n;

			re += (double)x[t].re * cos(a) - (double)x[t].im * sin(a);
			im += (double)x[t].re * sin(a) + (double)x[t].im * cos(a);
		}
		worst = MAX(worst, hypot((double)y.re - re, (double)y.im - im));
	}
	zassert_true(worst < 1e-3 * sqrt((double)n), "error %g", worst);
}

/* Raw bits at mod through the channel; returns the bit errors (all of them if not found). */
static unsigned int raw_trial(struct ofdm_ctx *c, enum ofdm_mod mod, double snr_db, bool split)
{
	size_t nbits = ofdm_frame_bits(c, mod);
	double cfo = 20e3 * (0.5 + test_rng_uniform());
	struct ofdm_rx_info info;

	ofdm_test_bits(bits, nbits, 1);
	zassert_equal(ofdm_tx_build(c, &tb, mod, hdr, bits, frame), c->len);
	zassert_equal(tb.clipped, 0U);
	channel(c, c->len, snr_db, cfo, 0.6);
	memset(got, 0, (nbits + 7U) / 8U);
	if (ofdm_rx_begin(c, win, WIN, hgot, &info) != 0) {
		return (unsigned int)nbits;
	}
	zassert_mem_equal(hgot, hdr, c->cfg.hdr_bits / 8U);
	zassert_within(info.cfo_hz, (float)cfo, 5e3f, "cfo %f, sent %f", (double)info.cfo_hz,
		       cfo);
	if (split) {
		/* Two workers, as on two CPUs: fork before worker 0 moves on. */
		size_t off = (size_t)c->mid * c->cfg.channels * ofdm_mod_bits(mod) / 8U;
		float err = 0.0f;

		ofdm_rx_fork(c, &wk2, win);
		ofdm_rx_part(c, &c->w0, win, mod, 0, c->mid, got, &err);
		ofdm_rx_part(c, &wk2, win, mod, c->mid, c->ndata, got + off, &err);
		info.mer_db = ofdm_rx_mer(c, err);
	} else {
		ofdm_rx_finish(c, win, mod, got, &info);
	}
	zassert_true(info.mer_db > (float)snr_db - 6.0f, "MER %.1f at %.0f dB",
		     (double)info.mer_db, snr_db);
	return bit_errors(bits, got, nbits);
}

ZTEST(link_ofdm, test_raw_each_modulation)
{
	static const double snr[OFDM_MODS] = {15.0, 18.0, 24.0, 30.0};

	for (int m = 0; m < OFDM_MODS; m++) {
		for (int t = 0; t < 2; t++) {
			unsigned int e = raw_trial(&ctx, (enum ofdm_mod)m, snr[m], false);

			zassert_equal(e, 0U, "mod %d trial %d: %u bit errors", m, t, e);
		}
	}
}

ZTEST(link_ofdm, test_raw_split_workers)
{
	unsigned int e = raw_trial(&ctx, OFDM_16QAM, 27.0, true);

	zassert_equal(e, 0U, "%u bit errors", e);
}

ZTEST(link_ofdm, test_raw_receive_at_40msps)
{
	struct ofdm_cfg c = cfg80;
	unsigned int e;

	c.rx_div = 2;
	zassert_equal(ofdm_init(&ctx2, &c), 0);
	zassert_equal(ctx2.rnfft, ctx2.nfft / 2U);
	e = raw_trial(&ctx2, OFDM_QPSK, 24.0, false);
	zassert_equal(e, 0U, "%u bit errors", e);
}

ZTEST(link_ofdm, test_frames)
{
	static const enum ofdm_mod mods[] = {OFDM_QPSK, OFDM_64QAM};

	for (size_t m = 0; m < ARRAY_SIZE(mods); m++) {
		struct ofdm_fhdr h = {.type = 0, .mod = mods[m], .dst = 2, .src = 1, .seq = 77};
		size_t user = ofdm_frame_user_bytes(&ctx, h.mod, h.fec);
		struct ofdm_rx_info info;
		struct ofdm_fhdr g;
		int corr;

		zassert_true(user > 0U && user <= OFDM_FRAME_CAP_MAX, "user %zu", user);
		zassert_true(ofdm_frame_units(&ctx, h.mod) >= 1U);
		ofdm_test_bits(bits, user * 8U, 3U);
		zassert_equal(ofdm_frame_build(&ctx, &tb, &fwk, &h, bits, frame), ctx.len);
		channel(&ctx, ctx.len, 30.0, 15e3, 0.6);
		zassert_equal(ofdm_frame_begin(&ctx, win, WIN, &g, &info), 0);
		zassert_true(g.seq == h.seq && g.mod == h.mod && g.dst == 2 && g.src == 1,
			     "header %u %d %u %u", g.seq, g.mod, g.dst, g.src);
		zassert_equal(ofdm_frame_finish(&ctx, win, &g, &fwk, got, &corr, &info), 0);
		zassert_mem_equal(got, bits, user);
		zassert_true(corr >= 0, "corrected %d", corr);
	}
}

ZTEST(link_ofdm, test_frame_fec_in_parts)
{
	struct ofdm_fhdr h = {.mod = OFDM_16QAM, .dst = 5, .src = 6, .seq = 1};
	size_t user = ofdm_frame_user_bytes(&ctx, h.mod, h.fec);
	unsigned int units = ofdm_frame_units(&ctx, h.mod);
	struct ofdm_rx_info info;
	struct ofdm_fhdr g;
	int corr = 0;

	ofdm_test_bits(bits, user * 8U, 9U);
	zassert_equal(ofdm_frame_build(&ctx, &tb, &fwk, &h, bits, frame), ctx.len);
	channel(&ctx, ctx.len, 27.0, 10e3, 0.6);
	zassert_equal(ofdm_frame_begin(&ctx, win, WIN, &g, &info), 0);
	/* Demodulate into the work buffer, then the RS units in two halves, then the CRC. */
	ofdm_rx_finish(&ctx, win, g.mod, fwk.bytes, &info);
	zassert_equal(ofdm_frame_fec_units(&ctx, &g, &fwk, 0, units / 2U, &corr), 0);
	zassert_equal(ofdm_frame_fec_units(&ctx, &g, &fwk, units / 2U, units, &corr), 0);
	zassert_equal(ofdm_frame_check(&ctx, &g, &fwk, got), 0);
	zassert_mem_equal(got, bits, user);
}

ZTEST(link_ofdm, test_fec_names)
{
	zassert_equal(ofdm_fec_parse(ofdm_fec_name(OFDM_FEC_RS)), (int)OFDM_FEC_RS);
	zassert_equal(ofdm_fec_parse(ofdm_fec_name(OFDM_FEC_NONE)), (int)OFDM_FEC_NONE);
	for (unsigned int f = 0; f < OFDM_FECS; f++) {
		zassert_not_null(ofdm_fec_name(f));
		zassert_equal(ofdm_fec_parse(ofdm_fec_name(f)), (int)f, "%s", ofdm_fec_name(f));
	}
	zassert_true(ofdm_fec_name(OFDM_FECS) == NULL);
	zassert_equal(ofdm_fec_parse("rs255"), -1);
}

ZTEST(link_ofdm, test_frames_bch_and_none)
{
	/* A BCH code and the CRC alone: the CRC is checked first, BCH decodes only on a failure. */
	static const char *const codes[] = {"bch2-128", "bch1-8"};

	for (size_t i = 0; i <= ARRAY_SIZE(codes); i++) {
		int fec = i < ARRAY_SIZE(codes) ? ofdm_fec_parse(codes[i]) : (int)OFDM_FEC_NONE;
		struct ofdm_fhdr h = {.mod = OFDM_QPSK, .fec = (uint8_t)fec, .dst = 4, .src = 3,
				      .seq = (uint16_t)(200 + i)};
		size_t user;
		struct ofdm_rx_info info;
		struct ofdm_fhdr g;
		int corr;

		zassert_true(fec >= 0, "%s", i < ARRAY_SIZE(codes) ? codes[i] : "none");
		user = ofdm_frame_user_bytes(&ctx, h.mod, h.fec);
		zassert_true(user > 0U, "code %d", fec);
		ofdm_test_bits(bits, user * 8U, 11U + (uint32_t)i);
		zassert_equal(ofdm_frame_build(&ctx, &tb, &fwk, &h, bits, frame), ctx.len);
		channel(&ctx, ctx.len, 24.0, 12e3, 0.6);
		zassert_equal(ofdm_frame_begin(&ctx, win, WIN, &g, &info), 0, "code %d", fec);
		zassert_equal(g.fec, h.fec);
		zassert_equal(ofdm_frame_finish(&ctx, win, &g, &fwk, got, &corr, &info), 0,
			      "code %d", fec);
		zassert_mem_equal(got, bits, user);
	}
}

ZTEST(link_ofdm, test_ack_frames)
{
	struct ofdm_fhdr h = {.type = 1, .mod = OFDM_BPSK, .dst = 0x80, .src = 0x00};
	size_t al = ofdm_hdr_samples(&ctx), n = 2U * ofdm_hdr_rsamples(&ctx) + ctx.rnfft;

	for (int t = 0; t < 3; t++) {
		struct ofdm_rx_info info;
		struct ofdm_fhdr g;

		h.seq = (uint16_t)(t * 7);
		zassert_equal(ofdm_frame_build_hdr(&ctx, &tb, &h, frame), al);
		channel(&ctx, al, 18.0, 15e3, 0.6);
		zassert_equal(ofdm_frame_begin_hdr(&ctx, win, n, &g, &info), 0, "trial %d", t);
		zassert_true(g.seq == h.seq && g.type == 1 && g.dst == 0x80, "trial %d", t);
	}
}

ZTEST(link_ofdm, test_no_frame)
{
	struct ofdm_rx_info info;
	struct ofdm_fhdr g;

	for (unsigned int i = 0; i < WIN; i++) {
		long qi = lround(30.0 * test_rng_gauss()), qq = lround(30.0 * test_rng_gauss());

		win[i] = (((uint32_t)qi & 0x3ffU) << 10) | ((uint32_t)qq & 0x3ffU);
	}
	zassert_equal(ofdm_rx_begin(&ctx, win, WIN, hgot, &info), -1);
	zassert_equal(ofdm_frame_begin(&ctx, win, WIN, &g, &info), -1);
}

ZTEST(link_ofdm, test_frame_too_noisy)
{
	/* 64-QAM far below its SNR: the header (BPSK) still decodes, the payload fails RS or
	 * CRC and is never passed on as good.
	 */
	struct ofdm_fhdr h = {.mod = OFDM_64QAM, .dst = 2, .src = 1, .seq = 3};
	size_t user = ofdm_frame_user_bytes(&ctx, h.mod, h.fec);
	struct ofdm_rx_info info;
	struct ofdm_fhdr g;
	int corr;

	ofdm_test_bits(bits, user * 8U, 5U);
	zassert_equal(ofdm_frame_build(&ctx, &tb, &fwk, &h, bits, frame), ctx.len);
	channel(&ctx, ctx.len, 12.0, 15e3, 0.6);
	zassert_equal(ofdm_frame_begin(&ctx, win, WIN, &g, &info), 0);
	zassert_equal(ofdm_frame_finish(&ctx, win, &g, &fwk, got, &corr, &info), -3);
}
