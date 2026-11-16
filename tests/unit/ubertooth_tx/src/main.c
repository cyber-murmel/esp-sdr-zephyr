/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit test of the transmit side of apps/ubertooth, on native_sim.
 *
 * gfsk_mod.c is checked against things it does not share code or tables with (txref.h): an
 * FM discriminator with integrate and dump at the known timing (bit errors, deviation), a double
 * precision ideal GFSK model from the Gaussian filter's closed form (error vector), a spectrum
 * from a double FFT (occupancy, mask, carrier placement), and consistency of chunked and random
 * access generation. ubt_tx.c is checked against the vendored upstream whitening, CRC and
 * dewhitening, and a bit-serial implementation of the specification's LFSRs.
 */
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "gfsk_mod.h"
#include "ubt_tx.h"
#include "upstream/bluetooth_le.h"
#include "txref.h"

#define FS    40e6
#define IF_HZ 4000000.0

static struct gfsk_lut lut;
static struct gfsk_tab tab_ble, tab_br, tab_gen;
static struct gfsk_mod mod;

#define MAXW (1U << 17)
static uint32_t wbuf[MAXW], wbuf2[MAXW];
static double fbuf[MAXW];
static double symmean[4096];
#define NFFT MAXW
static double re[NFFT], im[NFFT], psd[NFFT];

static uint32_t rng_state;

static uint32_t rnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static void random_bits(uint8_t *bits, unsigned int nbytes)
{
	for (unsigned int i = 0; i < nbytes; i++) {
		bits[i] = (uint8_t)(rnd() >> 11);
	}
}

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	rng_state = 0x2545f491U;
	gfsk_lut_init(&lut, GFSK_MOD_AMP, false);
	gfsk_tab_init(&tab_ble, GFSK_DEV_BLE_HZ, GFSK_MOD_IF_HZ);
	gfsk_tab_init(&tab_br, GFSK_DEV_BR_HZ, GFSK_MOD_IF_HZ);
	gfsk_tab_init(&tab_gen, GFSK_DEV_GENERIC_HZ, GFSK_MOD_IF_HZ);
}

ZTEST_SUITE(ubertooth_tx, NULL, NULL, before_each, NULL, NULL);

static struct gfsk_burst burst_of(const uint8_t *bits, unsigned int nbits, bool msb_first)
{
	return (struct gfsk_burst){.kind = GFSK_BURST,
				   .bits = bits,
				   .nbits = nbits,
				   .msb_first = msb_first,
				   .end = GFSK_END_CENTRE,
				   .lead = 4,
				   .trail = 4,
				   .ramp = 120};
}

static struct txref_burst ref_of(const struct gfsk_burst *b)
{
	return (struct txref_burst){.bits = b->bits,
				    .nbits = b->nbits,
				    .msb_first = b->msb_first,
				    .lead = b->lead,
				    .trail = b->trail,
				    .hold = b->end == GFSK_END_HOLD,
				    .loop = b->kind == GFSK_LOOP};
}

/* Demodulate the bits of a burst with the reference receiver; returns the errors. */
static unsigned int demod_errors(const uint32_t *w, size_t total, const struct gfsk_burst *b,
				 double if_hz, double *minabs)
{
	unsigned int errors = 0;

	txref_freq(w, total, fbuf);
	txref_dump(fbuf, (size_t)b->lead * 40U, b->nbits, if_hz, symmean);
	if (minabs != NULL) {
		*minabs = 1e12;
	}
	for (unsigned int s = 0; s < b->nbits; s++) {
		int bit = txref_bit(b->bits, s, b->msb_first);

		if ((symmean[s] > 0) != (bit != 0)) {
			errors++;
		}
		if (minabs != NULL && fabs(symmean[s]) < *minabs) {
			*minabs = fabs(symmean[s]);
		}
	}
	return errors;
}

/* ------------------------------------------------------------- tables */

ZTEST(ubertooth_tx, test_sine_lut)
{
	for (int amp = 100; amp <= 511; amp += 137) {
		gfsk_lut_init(&lut, amp, false);
		for (unsigned int i = 0; i < GFSK_LUT_SIZE; i++) {
			double a = ESP_SDR_TAU * i / GFSK_LUT_SIZE;
			int wi = txref_i(lut.w[i]), wq = txref_q(lut.w[i]);

			zassert_true(fabs(wi - amp * cos(a)) <= 0.51 &&
					     fabs(wq - amp * sin(a)) <= 0.51,
				     "lut %u at amp %d: %d %d", i, amp, wi, wq);
			zassert_equal(lut.w[i] >> 20, 0U, "upper bits clear");
		}
	}
	/* conj mirrors the spectrum: Q negated */
	gfsk_lut_init(&lut, 300, false);
	gfsk_lut_init((struct gfsk_lut *)wbuf, 300, true);
	for (unsigned int i = 0; i < GFSK_LUT_SIZE; i++) {
		zassert_equal(txref_i(lut.w[i]), txref_i(((struct gfsk_lut *)wbuf)->w[i]));
		zassert_equal(txref_q(lut.w[i]), -txref_q(((struct gfsk_lut *)wbuf)->w[i]));
	}
}

ZTEST(ubertooth_tx, test_rows_follow_the_closed_form)
{
	/* The integrated pulse, rebuilt from the tab: the phase a history gives over the symbol. */
	for (unsigned int h = 0; h < 8; h++) {
		double b[3] = {(h & 4) ? 1 : -1, (h & 2) ? 1 : -1, (h & 1) ? 1 : -1};
		double ph = tab_ble.phi0[h] / 4294967296.0, acc = 0;
		double s = sqrt(log(2.0)) / (ESP_SDR_TAU * 0.5);

		ARG_UNUSED(s);
		/* phi0 = D (b_prev G(2T) + b_cur G(T)), D = 0.25 turn */
		ph *= 4.0;
		zassert_within(
			ph, b[0] * txref_pulse(2.0 - 1.5, 0.5) + b[1] * txref_pulse(1.0 - 1.5, 0.5),
			3e-4, "phi0 of %u", h);
		for (unsigned int k = 0; k < GFSK_MOD_SPS; k++) {
			acc += tab_ble.inc[h][k] / 65536.0;
		}
		/* A symbol's phase step: IF 0.1 turn x 4 plus D (b_prev (1 - G(2T)) + b_cur (G(2T)
		 * - G(T)) + b_next G(T)) */
		double want =
			4.0 + 0.25 * (b[0] * (1.0 - txref_pulse(0.5, 0.5)) +
				      b[1] * (txref_pulse(0.5, 0.5) - txref_pulse(-0.5, 0.5)) +
				      b[2] * txref_pulse(-0.5, 0.5));

		zassert_within(acc, want, 2e-3, "row %u step %f want %f", h, acc, want);
	}
	/* Opposite histories mirror the deviation about the IF. */
	for (unsigned int h = 0; h < 8; h++) {
		for (unsigned int k = 0; k < GFSK_MOD_SPS; k++) {
			int ifpart = (int)lround(0.1 * 65536.0);

			zassert_within(tab_ble.inc[h][k] - ifpart,
				       -(tab_ble.inc[7 - h][k] - ifpart), 3, "mirror %u %u", h, k);
		}
	}
	zassert_equal(tab_ble.bit_step, 1 << 30, "0.25 turn per bit");
	zassert_within(tab_ble.if_inc, 0.1 * 4294967296.0, 1.0);
}

ZTEST(ubertooth_tx, test_mdmctrl_deviation)
{
	zassert_equal(gfsk_dev_mdmctrl(0x0040), 250000);
	zassert_equal(gfsk_dev_mdmctrl(0x0029), 160156);
	zassert_equal(gfsk_dev_mdmctrl(0x0057), 339844);
	zassert_equal(gfsk_dev_mdmctrl(0x7f80), 0);
	zassert_equal(ubt_tx_mdmctrl_offset(0x0080), 15625);
	zassert_equal(ubt_tx_mdmctrl_offset(0x1f80), 63 * 15625);
}

/* -------------------------------------------------------- demodulation */

static void check_modulation(const struct gfsk_tab *tab, double dev_hz, double if_hz)
{
	static uint8_t bits[64];
	struct gfsk_burst b;
	uint32_t total;
	unsigned int errors;
	double minabs;

	random_bits(bits, sizeof(bits));
	b = burst_of(bits, 400, false);
	total = gfsk_mod_start(&mod, tab, &lut, &b);
	zassert_equal(total, (400U + 8U) * 40U);
	gfsk_mod_gen(&mod, wbuf, 0, total);
	errors = demod_errors(wbuf, total, &b, if_hz, &minabs);
	zassert_equal(errors, 0U, "bit errors at %.0f Hz", dev_hz);
	/* The eye is open by a good margin: the weakest symbol keeps a third of the deviation. */
	zassert_true(minabs > dev_hz / 3.0, "eye %.0f Hz", minabs);
}

ZTEST(ubertooth_tx, test_zero_bit_errors)
{
	check_modulation(&tab_ble, GFSK_DEV_BLE_HZ, IF_HZ);
	check_modulation(&tab_br, GFSK_DEV_BR_HZ, IF_HZ);
	check_modulation(&tab_gen, GFSK_DEV_GENERIC_HZ, IF_HZ);
	/* Another IF, and a negative one (the spectrum below the LO). */
	gfsk_tab_init(&tab_ble, GFSK_DEV_BLE_HZ, 3000000);
	check_modulation(&tab_ble, GFSK_DEV_BLE_HZ, 3e6);
	gfsk_tab_init(&tab_ble, GFSK_DEV_BLE_HZ, -4000000);
	check_modulation(&tab_ble, GFSK_DEV_BLE_HZ, -4e6);
	/* MSB first arrays carry the same stream. */
}

ZTEST(ubertooth_tx, test_msb_first_bits)
{
	static uint8_t lsb[16], msb[16];
	struct gfsk_burst a, b;

	random_bits(lsb, sizeof(lsb));
	for (unsigned int i = 0; i < 128U; i++) {
		if (txref_bit(lsb, i, 0)) {
			msb[i / 8U] |= (uint8_t)(0x80U >> (i % 8U));
		}
	}
	a = burst_of(lsb, 120, false);
	b = burst_of(msb, 120, true);
	gfsk_mod_start(&mod, &tab_ble, &lut, &a);
	gfsk_mod_gen(&mod, wbuf, 0, mod.total);
	gfsk_mod_start(&mod, &tab_ble, &lut, &b);
	gfsk_mod_gen(&mod, wbuf2, 0, mod.total);
	zassert_mem_equal(wbuf, wbuf2, mod.total * 4U);
}

ZTEST(ubertooth_tx, test_deviation_within_three_percent)
{
	/* Runs of eight equal bits: from the fourth symbol on the frequency is the full deviation.
	 */
	static const uint8_t bits[3] = {0xff, 0x00, 0xff};
	const struct {
		const struct gfsk_tab *tab;
		double dev;
	} cases[] = {{&tab_ble, 250000.0}, {&tab_br, 160156.0}, {&tab_gen, 339844.0}};

	for (unsigned int c = 0; c < 3; c++) {
		struct gfsk_burst b = burst_of(bits, 24, false);

		gfsk_mod_start(&mod, cases[c].tab, &lut, &b);
		gfsk_mod_gen(&mod, wbuf, 0, mod.total);
		txref_freq(wbuf, mod.total, fbuf);
		for (unsigned int run = 0; run < 3; run++) {
			double acc = 0;

			/* Symbols 3 and 4 of the run, the middle 30 samples of the pair. */
			for (unsigned int n = (4U + 8U * run + 3U) * 40U + 5U;
			     n < (4U + 8U * run + 5U) * 40U - 5U; n++) {
				acc += fbuf[n];
			}
			acc = acc / 70.0 - IF_HZ;
			zassert_within(acc, (run == 1 ? -1.0 : 1.0) * cases[c].dev,
				       0.03 * cases[c].dev, "case %u run %u: %.0f Hz", c, run, acc);
		}
	}
}

ZTEST(ubertooth_tx, test_alternating_deviation_follows_the_model)
{
	/* 0101...: the Gaussian filter and the ISI cut the peak; compare the symbol means to the
	 * ideal. */
	static const uint8_t bits[8] = {0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa};
	struct gfsk_burst b = burst_of(bits, 64, false);
	struct txref_burst rb = ref_of(&b);
	static double ph[MAXW];

	gfsk_mod_start(&mod, &tab_ble, &lut, &b);
	gfsk_mod_gen(&mod, wbuf, 0, mod.total);
	txref_phase(&rb, 250000.0, IF_HZ, 0.5, mod.total, ph);
	txref_freq(wbuf, mod.total, fbuf);
	txref_dump(fbuf, 4U * 40U, 64, IF_HZ, symmean);
	for (unsigned int s = 8; s < 56; s++) {
		/* ideal mean frequency over the symbol from the phase difference across its 40
		 * samples */
		double ideal =
			(ph[(4U + s + 1U) * 40U - 1U] - ph[(4U + s) * 40U]) * 40e6 / 39.0 - IF_HZ;

		zassert_within(symmean[s], ideal, 6000.0, "symbol %u: %.0f vs %.0f", s, symmean[s],
			       ideal);
	}
	/* The peak is below the full deviation and the right sign. */
	zassert_true(fabs(symmean[20]) > 0.5 * 250000.0 && fabs(symmean[20]) < 0.9 * 250000.0,
		     "%.0f", symmean[20]);
}

ZTEST(ubertooth_tx, test_error_vector_against_the_ideal_model)
{
	static uint8_t bits[64];
	static double ph[MAXW];
	struct gfsk_burst b;
	struct txref_burst rb;
	double e2 = 0, sig = 0;

	random_bits(bits, sizeof(bits));
	b = burst_of(bits, 400, false);
	rb = ref_of(&b);
	gfsk_mod_start(&mod, &tab_ble, &lut, &b);
	gfsk_mod_gen(&mod, wbuf, 0, mod.total);
	txref_phase(&rb, 250000.0, IF_HZ, 0.5, mod.total, ph);
	for (unsigned int n = b.ramp; n < mod.total - b.ramp; n++) {
		double a = ESP_SDR_TAU * ph[n];
		double di = txref_i(wbuf[n]) - GFSK_MOD_AMP * cos(a),
		       dq = txref_q(wbuf[n]) - GFSK_MOD_AMP * sin(a);

		e2 += di * di + dq * dq;
		sig += (double)GFSK_MOD_AMP * GFSK_MOD_AMP;
	}
	TC_PRINT("EVM %.1f dB (LUT %u bits)\n", 10.0 * log10(e2 / sig), GFSK_LUT_BITS);
	zassert_true(10.0 * log10(e2 / sig) <
			     -43.0 + 6.0 * (GFSK_LUT_BITS - 8U) - 2.0 * (GFSK_LUT_BITS < 8U),
		     "EVM");
}

ZTEST(ubertooth_tx, test_constant_envelope)
{
	static uint8_t bits[64];
	struct gfsk_burst b;
	double rmin = 1e9, rmax = 0;

	random_bits(bits, sizeof(bits));
	b = burst_of(bits, 400, false);
	gfsk_mod_start(&mod, &tab_br, &lut, &b);
	gfsk_mod_gen(&mod, wbuf, 0, mod.total);
	for (unsigned int n = b.ramp; n < mod.total - b.ramp; n++) {
		double r = hypot(txref_i(wbuf[n]), txref_q(wbuf[n]));

		rmin = r < rmin ? r : rmin;
		rmax = r > rmax ? r : rmax;
	}
	zassert_true(rmin > 0.99 * GFSK_MOD_AMP && rmax < 1.01 * GFSK_MOD_AMP,
		     "envelope %.2f to %.2f", rmin, rmax);
	/* Headroom: every word inside the amplitude and 20 bits wide. */
	for (unsigned int n = 0; n < mod.total; n++) {
		zassert_true(abs(txref_i(wbuf[n])) <= GFSK_MOD_AMP &&
			     abs(txref_q(wbuf[n])) <= GFSK_MOD_AMP);
		zassert_equal(wbuf[n] >> 20, 0U);
	}
}

/* ------------------------------------------------------------ spectrum */

static void spectrum_of(const uint32_t *w, size_t n)
{
	txref_psd(w, n, NFFT, re, im, psd);
}

ZTEST(ubertooth_tx, test_spectrum_occupancy_and_mask)
{
	static uint8_t bits[256];
	struct gfsk_burst b;
	double all, in06, in05;

	random_bits(bits, sizeof(bits));
	b = burst_of(bits, 1800, false);
	gfsk_mod_start(&mod, &tab_ble, &lut, &b);
	gfsk_mod_gen(&mod, wbuf, 0, mod.total);
	spectrum_of(wbuf, mod.total);
	all = txref_band(psd, NFFT, -20e6, 20e6);
	in06 = txref_band(psd, NFFT, IF_HZ - 0.6e6, IF_HZ + 0.6e6) / all;
	in05 = txref_band(psd, NFFT, IF_HZ - 0.5e6, IF_HZ + 0.5e6) / all;
	TC_PRINT("power within +-0.5 MHz %.4f, +-0.6 MHz %.4f\n", in05, in06);
	/* GMSK BT 0.5 holds 98.8 % in +-0.5 MHz and 99.8 % in +-0.6 MHz */
	zassert_true(in05 > 0.985, "+-0.5 MHz: %f", in05);
	zassert_true(in06 > 0.995, "+-0.6 MHz: %f", in06);
	/* 1 MHz wide bands centred at the offsets, dBc to the total: a mask with margin */
	const struct {
		double off, limit_db;
	} mask[] = {{1.0, -18.0}, {1.5, -36.0}, {2.0, -55.0},
		    {3.0, -55.0}, {4.0, -55.0}, {8.0, -50.0}};

	for (unsigned int i = 0; i < ARRAY_SIZE(mask); i++) {
		for (int sgn = -1; sgn <= 1; sgn += 2) {
			double c = IF_HZ + sgn * mask[i].off * 1e6;
			double dbc =
				10.0 * log10(txref_band(psd, NFFT, c - 0.5e6, c + 0.5e6) / all);

			TC_PRINT("  %+.1f MHz: %.1f dBc\n", sgn * mask[i].off, dbc);
			zassert_true(dbc < mask[i].limit_db, "%+.1f MHz: %.1f dBc",
				     sgn * mask[i].off, dbc);
		}
	}
	/* Compare with the ideal model's spectrum inside +-1.5 MHz. */
	{
		static double ph[MAXW], ours[7], ideal[7];
		struct txref_burst rb = ref_of(&b);

		for (unsigned int i = 0; i < 7U; i++) {
			double c = IF_HZ + (-1.5 + 0.5 * i) * 1e6;

			ours[i] = txref_band(psd, NFFT, c - 0.25e6, c + 0.25e6);
		}
		txref_phase(&rb, 250000.0, IF_HZ, 0.5, mod.total, ph);
		for (unsigned int n = 0; n < mod.total; n++) {
			double a = ESP_SDR_TAU * ph[n];

			/* the ideal as DAC words at the same amplitude (the ramps are not compared)
			 */
			wbuf2[n] = (n < b.ramp || n >= mod.total - b.ramp)
					   ? wbuf[n]
					   : gfsk_word((int)lround(GFSK_MOD_AMP * cos(a)),
						       (int)lround(GFSK_MOD_AMP * sin(a)));
		}
		spectrum_of(wbuf2, mod.total);
		for (unsigned int i = 0; i < 7U; i++) {
			double c = IF_HZ + (-1.5 + 0.5 * i) * 1e6;

			ideal[i] = txref_band(psd, NFFT, c - 0.25e6, c + 0.25e6);
			TC_PRINT("  ours - ideal at %+.1f MHz: %.2f dB\n", -1.5 + 0.5 * i,
				 10.0 * log10(ours[i] / ideal[i]));
			/* the skirts at +-1.5 MHz are within 10 dB of the table's phase noise */
			zassert_within(10.0 * log10(ours[i] / ideal[i]), 0.0,
				       i == 0U || i == 6U ? 1.5 : 0.3, "offset %.1f MHz",
				       -1.5 + 0.5 * i);
		}
	}
}

ZTEST(ubertooth_tx, test_carrier_placement)
{
	static const uint8_t none[1] = {0};
	struct gfsk_burst b = {.kind = GFSK_CARRIER, .bits = none, .nbits = 400};
	const int ifs[] = {4000000, 3000000, -4000000, 7500000};

	for (unsigned int c = 0; c < ARRAY_SIZE(ifs); c++) {
		double peak = 0, centroid = 0, sum = 0;
		size_t pk = 0;

		gfsk_tab_init(&tab_ble, GFSK_DEV_BLE_HZ, ifs[c]);
		gfsk_mod_start(&mod, &tab_ble, &lut, &b);
		gfsk_mod_gen(&mod, wbuf, 0, 16000);
		spectrum_of(wbuf, 16000);
		for (size_t k = 0; k < NFFT; k++) {
			double f = (k < NFFT / 2 ? (double)k : (double)k - NFFT) * FS / NFFT;

			if (psd[k] > peak) {
				peak = psd[k];
				pk = k;
			}
			if (fabs(f - ifs[c]) < 1e6) {
				centroid += f * psd[k];
				sum += psd[k];
			}
		}
		double fpk = (pk < NFFT / 2 ? (double)pk : (double)pk - NFFT) * FS / NFFT;

		zassert_within(fpk, (double)ifs[c], 2500.0, "peak %.0f Hz for IF %d", fpk, ifs[c]);
		zassert_within(centroid / sum, (double)ifs[c], 100.0, "centroid %.1f",
			       centroid / sum);
		/* The spur and image floor of the table: 10 dB below nothing worth worrying about,
		 * but bounded. */
		{
			double tot = txref_band(psd, NFFT, -20e6, 20e6);
			double carrier = txref_band(psd, NFFT, ifs[c] - 50e3, ifs[c] + 50e3);

			zassert_true(carrier / tot > 0.9995, "carrier fraction %f", carrier / tot);
		}
	}
}

ZTEST(ubertooth_tx, test_conj_lut_mirrors_the_spectrum)
{
	static uint8_t bits[64];
	static struct gfsk_lut lut_c;
	struct gfsk_burst b;
	double up, down;

	random_bits(bits, sizeof(bits));
	b = burst_of(bits, 400, false);
	gfsk_lut_init(&lut_c, GFSK_MOD_AMP, true);
	gfsk_mod_start(&mod, &tab_ble, &lut_c, &b);
	gfsk_mod_gen(&mod, wbuf, 0, mod.total);
	spectrum_of(wbuf, mod.total);
	up = txref_band(psd, NFFT, IF_HZ - 1e6, IF_HZ + 1e6);
	down = txref_band(psd, NFFT, -IF_HZ - 1e6, -IF_HZ + 1e6);
	zassert_true(down / up > 1000.0, "mirrored: %f", down / up);
}

/* ------------------------------------------------------ chunks and loops */

ZTEST(ubertooth_tx, test_chunked_fill_equals_one_shot)
{
	static uint8_t bits[64];
	struct gfsk_burst b;
	size_t at = 0;

	random_bits(bits, sizeof(bits));
	b = burst_of(bits, 400, false);
	b.end = GFSK_END_HOLD;
	gfsk_mod_start(&mod, &tab_ble, &lut, &b);
	gfsk_mod_gen(&mod, wbuf, 0, mod.total);
	while (at < mod.total) {
		size_t n = 1U + rnd() % 317U;
		size_t got = gfsk_mod_fill(&mod, wbuf2 + at, n);

		zassert_equal(got, MIN(n, mod.total - at), "short fill");
		at += got;
	}
	zassert_true(gfsk_mod_done(&mod));
	zassert_equal(gfsk_mod_fill(&mod, wbuf2, 10), 0U);
	zassert_mem_equal(wbuf, wbuf2, mod.total * 4U, "chunked");
	/* random access, overlapping, out of order, like the DAC filler */
	for (unsigned int i = 0; i < 300; i++) {
		uint32_t index = rnd() % mod.total, n = 1U + rnd() % 2000U;
		uint32_t take = MIN(n, mod.total - index);

		memset(wbuf2, 0xa5, take * 4U);
		gfsk_mod_gen(&mod, wbuf2, index, take);
		zassert_mem_equal(wbuf + index, wbuf2, take * 4U, "random access at %u", index);
	}
	/* past the end: zero words */
	gfsk_mod_gen(&mod, wbuf2, mod.total - 3U, 10);
	zassert_mem_equal(wbuf2, wbuf + mod.total - 3U, 12U);
	for (unsigned int k = 3; k < 10; k++) {
		zassert_equal(wbuf2[k], 0U);
	}
	gfsk_mod_rewind(&mod);
	zassert_false(gfsk_mod_done(&mod));
}

ZTEST(ubertooth_tx, test_gen_matches_the_dac_callback_type)
{
	void (*cb)(void *, uint32_t *, uint64_t, uint32_t) = gfsk_mod_gen_cb;
	static uint8_t bits[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	struct gfsk_burst b = burst_of(bits, 64, false);

	gfsk_mod_start(&mod, &tab_ble, &lut, &b);
	cb(&mod, wbuf, 100, 500);
	gfsk_mod_gen(&mod, wbuf2, 100, 500);
	zassert_mem_equal(wbuf, wbuf2, 2000U);
}

ZTEST(ubertooth_tx, test_burst_timeline_and_ends)
{
	static const uint8_t ones[2] = {0xff, 0xff};
	static const uint8_t zeros[2] = {0, 0};
	const double dev = 250000.0;

	/* Centre: the carrier is back at the IF a few symbols after the last bit, and before the
	 * first. */
	for (int hold = 0; hold < 2; hold++) {
		struct gfsk_burst b = burst_of(ones, 16, false);
		double lead, mid, tail;

		b.end = hold ? GFSK_END_HOLD : GFSK_END_CENTRE;
		b.ramp = 0;
		gfsk_mod_start(&mod, &tab_ble, &lut, &b);
		zassert_equal(mod.total, (16U + 8U) * 40U);
		zassert_equal(gfsk_mod_data_start(&mod), 160U);
		gfsk_mod_gen(&mod, wbuf, 0, mod.total);
		txref_freq(wbuf, mod.total, fbuf);
		lead = mid = tail = 0;
		for (unsigned int k = 5; k < 100; k++) {
			lead += fbuf[k];
		}
		for (unsigned int k = (4U + 7U) * 40U; k < (4U + 9U) * 40U; k++) {
			mid += fbuf[k];
		}
		for (unsigned int k = (4U + 16U + 3U) * 40U; k < (4U + 16U + 4U) * 40U - 2U; k++) {
			tail += fbuf[k];
		}
		zassert_within(lead / 95.0, IF_HZ, 1500.0, "lead %.0f", lead / 95.0);
		zassert_within(mid / 80.0, IF_HZ + dev, 0.02 * dev, "plateau %.0f", mid / 80.0);
		zassert_within(tail / (40.0 - 2.0), hold ? IF_HZ + dev : IF_HZ, 0.02 * dev,
			       "tail %.0f (hold %d)", tail / 38.0, hold);
	}
	/* A burst of zeros goes the other way and the sum is tracked: same checks on the sign. */
	{
		struct gfsk_burst b = burst_of(zeros, 16, false);
		double mid = 0;

		b.ramp = 0;
		gfsk_mod_start(&mod, &tab_ble, &lut, &b);
		gfsk_mod_gen(&mod, wbuf, 0, mod.total);
		txref_freq(wbuf, mod.total, fbuf);
		for (unsigned int k = (4U + 7U) * 40U; k < (4U + 9U) * 40U; k++) {
			mid += fbuf[k];
		}
		zassert_within(mid / 80.0, IF_HZ - dev, 0.02 * dev);
	}
}

ZTEST(ubertooth_tx, test_ramps)
{
	static const uint8_t bits[8] = {0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa};
	const unsigned int ramps[] = {1, 16, 120, 255};

	for (unsigned int r = 0; r < ARRAY_SIZE(ramps); r++) {
		struct gfsk_burst b = burst_of(bits, 64, false);
		double prev = -1;

		b.ramp = (uint16_t)ramps[r];
		gfsk_mod_start(&mod, &tab_ble, &lut, &b);
		gfsk_mod_gen(&mod, wbuf, 0, mod.total);
		for (unsigned int n = 0; n < ramps[r]; n++) {
			double rad = hypot(txref_i(wbuf[n]), txref_q(wbuf[n]));

			zassert_true(rad >= prev - 1.5, "rises: %u %.1f after %.1f", n, rad, prev);
			prev = rad;
		}
		if (ramps[r] >= 16U) {
			zassert_true(hypot(txref_i(wbuf[0]), txref_q(wbuf[0])) <
				     0.05 * GFSK_MOD_AMP + 1.0);
		}
		zassert_true(prev < GFSK_MOD_AMP + 1.0);
		if (ramps[r] > 16U) {
			zassert_true(prev > 0.95 * GFSK_MOD_AMP, "rose to %.1f", prev);
		}
		prev = 1e9;
		for (unsigned int n = mod.total - ramps[r]; n < mod.total; n++) {
			double rad = hypot(txref_i(wbuf[n]), txref_q(wbuf[n]));

			zassert_true(rad <= prev + 1.5, "falls");
			prev = rad;
		}
		if (ramps[r] >= 16U) {
			zassert_true(prev < 0.05 * GFSK_MOD_AMP + 1.0, "last sample %.1f", prev);
		}
		/* the raised cosine: 0.5 (1 - cos(pi (n + 1/2) / R)) of the amplitude */
		if (ramps[r] >= 16U) {
			for (unsigned int n = 0; n < ramps[r]; n += 7U) {
				double want = GFSK_MOD_AMP * 0.5 *
					      (1.0 - cos(ESP_SDR_TAU / 2.0 * (n + 0.5) / ramps[r]));
				double rad = hypot(txref_i(wbuf[n]), txref_q(wbuf[n]));

				zassert_within(rad, want, 0.012 * GFSK_MOD_AMP,
					       "ramp %u at %u: %.1f vs %.1f", ramps[r], n, rad,
					       want);
			}
		}
	}
}

ZTEST(ubertooth_tx, test_loop_bank_is_seamless)
{
	struct ubt_tx_pkt p;
	struct gfsk_burst b;
	struct txref_burst rb;
	static double ph[MAXW];
	unsigned int ones = 0;
	double e2 = 0, sig = 0;

	zassert_equal(ubt_tx_build_test(&p, MOD_BT_LOW_ENERGY, 2441, UBT_TX_TEST_PRBS, 0x1a5), 400);
	for (unsigned int i = 0; i < 400U; i++) {
		ones += (p.bits[i / 8U] >> (i % 8U)) & 1U;
	}
	zassert_equal(ones, 200U, "balanced");
	ubt_tx_burst(&p, &b);
	zassert_equal(b.kind, GFSK_LOOP);
	zassert_equal(gfsk_mod_start(&mod, &tab_ble, &lut, &b), 16000U);
	/* two periods: the second is the first and the join has no step against the ideal periodic
	 * signal */
	gfsk_mod_gen(&mod, wbuf, 0, 32000);
	zassert_mem_equal(wbuf, wbuf + 16000, 16000U * 4U);
	rb = ref_of(&b);
	txref_phase(&rb, 250000.0, IF_HZ, 0.5, 32000, ph);
	for (unsigned int n = 0; n < 32000U; n++) {
		double a = ESP_SDR_TAU * ph[n];
		double di = txref_i(wbuf[n]) - GFSK_MOD_AMP * cos(a),
		       dq = txref_q(wbuf[n]) - GFSK_MOD_AMP * sin(a);

		e2 += di * di + dq * dq;
		sig += (double)GFSK_MOD_AMP * GFSK_MOD_AMP;
		if (n >= 15990U && n < 16010U) {
			zassert_true(di * di + dq * dq < 36.0 * 36.0,
				     "error at the join %u: %.1f %.1f", n, di, dq);
		}
	}
	zassert_true(10.0 * log10(e2 / sig) < -41.0, "periodic EVM");
	/* the frequency at the join equals the ideal's */
	txref_freq(wbuf, 32000, fbuf);
	{
		double fj = 0, fi = 0;

		for (unsigned int n = 15980; n < 16020; n++) {
			fj += fbuf[n];
			fi += (ph[n] - ph[n - 1]) * FS;
		}
		zassert_within(fj / 40.0, fi / 40.0, 3000.0, "join %.0f vs %.0f", fj / 40.0,
			       fi / 40.0);
	}
	/* bits recovered from both periods */
	{
		unsigned int errors = 0;

		txref_dump(fbuf, 0, 800, IF_HZ, symmean);
		for (unsigned int s = 0; s < 800U; s++) {
			errors += (symmean[s] > 0) != (txref_bit(p.bits, s % 400U, 0) != 0);
		}
		zassert_equal(errors, 0U, "loop bank bit errors");
	}
}

ZTEST(ubertooth_tx, test_unmodulated_carrier_loop)
{
	struct ubt_tx_pkt p;
	struct gfsk_burst b;
	double acc = 0, worst = 0;

	zassert_equal(ubt_tx_build_test(&p, MOD_BT_BASIC_RATE, 2441, UBT_TX_TEST_CARRIER, 0), 400);
	zassert_equal(p.dev_hz, 160156);
	ubt_tx_burst(&p, &b);
	zassert_equal(b.kind, GFSK_CARRIER);
	zassert_equal(gfsk_mod_start(&mod, &tab_br, &lut, &b), 16000U);
	gfsk_mod_gen(&mod, wbuf, 0, 32000);
	zassert_mem_equal(wbuf, wbuf + 16000, 16000U * 4U);
	txref_freq(wbuf, 32000, fbuf);
	for (unsigned int n = 1; n < 32000; n++) {
		acc += fbuf[n];
	}
	zassert_within(acc / 31999.0, IF_HZ, 20.0, "mean %f", acc / 31999.0);
	for (unsigned int n = 100; n < 31900; n += 10) {
		double m = 0;

		for (unsigned int k = 0; k < 10; k++) {
			m += fbuf[n + k];
		}
		worst = fmax(worst, fabs(m / 10.0 - IF_HZ));
	}
	zassert_true(worst < 5000.0, "short term wander %.0f Hz", worst);
	/* the join between periods does not hide a phase step either */
	zassert_within(fbuf[16000], IF_HZ, 90000.0);
}

/* ------------------------------------------------------- packet builders */

/* Data whitening of the specification, bit serial: LFSR x^7 + x^4 + 1, position 0 and the channel
 * index. */
static void spec_whiten(uint8_t *data, unsigned int len, unsigned int chan_index)
{
	unsigned int lfsr = (chan_index & 0x3fU) | 0x40U;

	for (unsigned int i = 0; i < len; i++) {
		for (unsigned int bit = 0; bit < 8; bit++) {
			if (lfsr & 1U) {
				data[i] ^= (uint8_t)(1U << bit);
				lfsr ^= 0x88U;
			}
			lfsr >>= 1;
		}
	}
}

/* CRC of the specification, bit serial: positions 0 to 23, polynomial taps 1, 3, 4, 6, 9, 10; init
 * in position order. */
static void spec_crc(uint32_t init, const uint8_t *data, unsigned int len, uint8_t crc[3])
{
	int s[24];

	for (int i = 0; i < 24; i++) {
		s[i] = (init >> i) & 1;
	}
	for (unsigned int i = 0; i < len; i++) {
		for (unsigned int bit = 0; bit < 8; bit++) {
			int fb = s[23] ^ ((data[i] >> bit) & 1);

			for (int k = 23; k > 0; k--) {
				s[k] = s[k - 1];
			}
			s[0] = fb;
			for (int t = 0; t < 6; t++) {
				static const int taps[6] = {1, 3, 4, 6, 9, 10};

				s[taps[t]] ^= fb;
			}
		}
	}
	/* Sent from position 23 down, every octet from its low bit. */
	memset(crc, 0, 3);
	for (int i = 0; i < 24; i++) {
		crc[i / 8] |= (uint8_t)(s[23 - i] << (i % 8));
	}
}

ZTEST(ubertooth_tx, test_whitening_table_matches_the_lfsr)
{
	/* The vendored upstream sequence per channel index against the specification's LFSR. */
	for (unsigned int idx = 0; idx < 40; idx++) {
		uint8_t seq[16] = {0};
		unsigned int pos = whitening_index[idx];

		spec_whiten(seq, sizeof(seq), idx);
		for (unsigned int i = 0; i < 128U; i++) {
			unsigned int bit = (seq[i / 8U] >> (i % 8U)) & 1U;

			zassert_equal(bit, whitening[(pos + i) % sizeof(whitening)],
				      "channel %u bit %u", idx, i);
		}
	}
}

/* The air bytes of an advertising packet by the specification, independent of ubt_tx.c and of
 * upstream's tables. */
static unsigned int spec_adv(uint8_t *air, const uint8_t mac[6], const uint8_t *adv,
			     unsigned int adv_len, unsigned int chan_index)
{
	uint8_t pdu[39 + 3];
	unsigned int n = 0, len = 8U + adv_len;

	pdu[0] = 0x00;
	pdu[1] = (uint8_t)(6U + adv_len);
	for (unsigned int i = 0; i < 6; i++) {
		pdu[2 + i] = mac[5 - i];
	}
	memcpy(pdu + 8, adv, adv_len);
	spec_crc(0x555555U, pdu, len, pdu + len);
	spec_whiten(pdu, len + 3U, chan_index);
	air[n++] = 0xaa;
	air[n++] = 0xd6;
	air[n++] = 0xbe;
	air[n++] = 0x89;
	air[n++] = 0x8e;
	memcpy(air + n, pdu, len + 3U);
	return n + len + 3U;
}

ZTEST(ubertooth_tx, test_adv_matches_the_specification)
{
	static const uint8_t mac[6] = {0x22, 0x44, 0x66, 0x88, 0xaa, 0xcc};
	static const uint8_t adv[3] = {0x02, 0x01, 0x05};
	const struct {
		uint16_t mhz;
		unsigned int idx;
	} chans[] = {{2402, 37}, {2426, 38}, {2480, 39}};
	struct ubt_tx_pkt p;
	uint8_t air[48];

	for (unsigned int c = 0; c < 3; c++) {
		unsigned int n = spec_adv(air, mac, adv, 3, chans[c].idx);

		zassert_equal(ubt_tx_adv_index(chans[c].mhz), chans[c].idx);
		zassert_equal(
			ubt_tx_build_adv(&p, mac, adv, 3, chans[c].mhz, UBT_TX_ADV_CRC_INIT_REV),
			(int)(8U * n));
		zassert_equal(p.nbits, 8U * n);
		zassert_false(p.msb_first);
		zassert_equal(p.channel_mhz, chans[c].mhz);
		zassert_equal(p.dev_hz, 250000);
		zassert_mem_equal(p.bits, air, n, "air bytes on %u", chans[c].mhz);
	}
	/* lengths 0 to 31, all against the specification */
	for (unsigned int len = 0; len <= UBT_TX_ADV_DATA_MAX; len++) {
		uint8_t data[32];
		unsigned int n;

		random_bits(data, sizeof(data));
		n = spec_adv(air, mac, data, len, 38);
		zassert_equal(ubt_tx_build_adv(&p, mac, data, len, 2426, UBT_TX_ADV_CRC_INIT_REV),
			      (int)(8U * n));
		zassert_mem_equal(p.bits, air, n, "adv length %u", len);
	}
	zassert_equal(ubt_tx_build_adv(&p, mac, adv, 32, 2402, UBT_TX_ADV_CRC_INIT_REV), -EMSGSIZE);
}

ZTEST(ubertooth_tx, test_adv_pdu_bytes_as_upstream_builds_them)
{
	/* bt_slave_le(): adv_ind = {0, 6 + len, mac[5] .. mac[0], adv data} for
	 * "-s22:44:66:88:aa:cc". */
	static const uint8_t mac[6] = {0x22, 0x44, 0x66, 0x88, 0xaa, 0xcc};
	static const uint8_t adv[3] = {0x02, 0x01, 0x05};
	static const uint8_t want[] = {0x00, 0x09, 0xcc, 0xaa, 0x88, 0x66,
				       0x44, 0x22, 0x02, 0x01, 0x05};
	uint8_t pdu[40];

	zassert_equal(ubt_tx_adv_pdu(pdu, mac, adv, 3, false), 11);
	zassert_mem_equal(pdu, want, sizeof(want));
	zassert_equal(ubt_tx_adv_pdu(pdu, mac, adv, 3, true), 11);
	zassert_equal(pdu[0], 0x40, "TxAdd");
	zassert_equal(ubt_tx_adv_pdu(pdu, mac, NULL, 0, false), 8);
	zassert_equal(pdu[1], 6);
}

ZTEST(ubertooth_tx, test_adv_decodes_with_the_upstream_receive_path)
{
	/*
	 * The chain a sniffer built from upstream code runs: the CC2400 hands bytes with the first
	 * received bit in the most significant bit; le_dewhiten() whitens back and restores the LSB
	 * order; the CRC from btle_crcgen_lut().
	 */
	static const uint8_t mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
	const unsigned int chans[] = {2402, 2426, 2480, 2404, 2440};
	struct ubt_tx_pkt p;

	for (unsigned int c = 0; c < ARRAY_SIZE(chans); c++) {
		uint8_t adv[31], rx[64], pdu[40];
		unsigned int len = 1U + rnd() % 31U, pdu_len = 8U + len;

		random_bits(adv, sizeof(adv));
		zassert_true(ubt_tx_build_adv(&p, mac, adv, len, (uint16_t)chans[c],
					      UBT_TX_ADV_CRC_INIT_REV) > 0);
		/* After preamble and access address: MSB first bytes of the whitened PDU and CRC */
		memset(rx, 0, sizeof(rx));
		for (unsigned int i = 0; i < 8U * (pdu_len + 3U); i++) {
			unsigned int src = 40U + i;

			if (txref_bit(p.bits, src, 0)) {
				rx[i / 8U] |= (uint8_t)(0x80U >> (i % 8U));
			}
		}
		le_dewhiten(rx, pdu_len + 3U, chans[c]);
		zassert_equal(ubt_tx_adv_pdu(pdu, mac, adv, len, false), (int)pdu_len);
		zassert_mem_equal(rx, pdu, pdu_len, "dewhitened PDU on %u", chans[c]);
		{
			uint32_t crc = btle_crcgen_lut(UBT_TX_ADV_CRC_INIT_REV, rx, (int)pdu_len);

			zassert_equal(rx[pdu_len], (uint8_t)crc);
			zassert_equal(rx[pdu_len + 1U], (uint8_t)(crc >> 8));
			zassert_equal(rx[pdu_len + 2U], (uint8_t)(crc >> 16));
			zassert_equal(btle_calc_crc(UBT_TX_ADV_CRC_INIT_REV, rx, (int)pdu_len),
				      crc);
		}
		/* Access address and preamble */
		zassert_equal(p.bits[0], 0xaa);
		zassert_equal(p.bits[1], 0xd6);
		zassert_equal(p.bits[2], 0xbe);
		zassert_equal(p.bits[3], 0x89);
		zassert_equal(p.bits[4], 0x8e);
	}
}

ZTEST(ubertooth_tx, test_preamble_follows_the_access_address)
{
	uint8_t pdu[8] = {0x00, 0x06, 1, 2, 3, 4, 5, 6};
	struct ubt_tx_pkt p;

	zassert_true(ubt_tx_build_le(&p, 0x8e89bed6U, UBT_TX_ADV_CRC_INIT_REV, pdu, 8, 2402) > 0);
	zassert_equal(p.bits[0], 0xaa, "AA bit 0 is 0: first preamble bit 0");
	zassert_true(ubt_tx_build_le(&p, 0x8e89bed7U, UBT_TX_ADV_CRC_INIT_REV, pdu, 8, 2402) > 0);
	zassert_equal(p.bits[0], 0x55, "AA bit 0 is 1: first preamble bit 1");
	zassert_equal(ubt_tx_build_le(&p, 1, 0, pdu, 1, 2402), -EMSGSIZE);
	zassert_equal(ubt_tx_build_le(&p, 1, 0, pdu, 40, 2402), -EMSGSIZE);
}

ZTEST(ubertooth_tx, test_generic_packet)
{
	/* A request: synch 0x6b7d syncl 0x9171, channel 2441, length 5, pa_level 3, data 10 20 30
	 * 40 50 */
	static const uint8_t req[] = {0x7d, 0x6b, 0x71, 0x91, 0x89, 0x09, 5,
				      3,    0x10, 0x20, 0x30, 0x40, 0x50};
	struct ubt_tx_generic g;
	struct ubt_tx_pkt p;

	memset(&g, 0, sizeof(g));
	/* upstream copies 7 + length bytes: the last data byte (index 4) is not part of the copy */
	zassert_equal(ubt_tx_generic_load(&g, req, sizeof(req)), 12U);
	zassert_equal(g.synch, 0x6b7d);
	zassert_equal(g.syncl, 0x9171);
	zassert_equal(g.channel, 2441);
	zassert_equal(g.length, 5);
	zassert_equal(g.pa_level, 3, "pa_level at byte 7");
	zassert_equal(g.data[0], 0x10);
	zassert_equal(g.data[3], 0x40);
	zassert_equal(g.data[4], 0x00, "the byte beyond the copy keeps the old contents");
	g.data[4] = 0x50;
	/* a second request leaves the stale tail */
	zassert_equal(ubt_tx_generic_load(&g, req, 8), 8U, "short request");
	zassert_equal(g.data[0], 0x10, "data untouched by a 8 byte request");
	zassert_equal(ubt_tx_generic_load(&g, req, 3), 0U);

	zassert_equal(ubt_tx_build_generic(&p, &g), (int)(8U * (32U + 4U + 5U)));
	zassert_true(p.msb_first);
	zassert_equal(p.channel_mhz, 2441);
	zassert_equal(p.dev_hz, 339844);
	zassert_equal(p.pa_level, 3);
	for (unsigned int i = 0; i < 32U; i++) {
		zassert_equal(p.bits[i], 0x55);
	}
	zassert_equal(p.bits[32], 0x6b);
	zassert_equal(p.bits[33], 0x7d);
	zassert_equal(p.bits[34], 0x91);
	zassert_equal(p.bits[35], 0x71);
	zassert_equal(p.bits[36], 0x10);
	zassert_equal(p.bits[40], 0x50);
	/* the FIFO holds 32 bytes */
	g.length = 50;
	zassert_equal(ubt_tx_build_generic(&p, &g), (int)(8U * (32U + 4U + 32U)));
	g.length = 0;
	zassert_equal(ubt_tx_build_generic(&p, &g), (int)(8U * 36U));
}

ZTEST(ubertooth_tx, test_generic_packet_over_the_air)
{
	static const uint8_t req[] = {0x7d, 0x6b, 0x71, 0x91, 0x89, 0x09, 20, 0,  1,  2,
				      3,    4,    5,    6,    7,    8,    9,  10, 11, 12,
				      13,   14,   15,   16,   17,   18,   19, 20};
	struct ubt_tx_generic g;
	struct ubt_tx_pkt p;
	struct gfsk_burst b;
	unsigned int errors;

	memset(&g, 0, sizeof(g));
	ubt_tx_generic_load(&g, req, sizeof(req));
	zassert_true(ubt_tx_build_generic(&p, &g) > 0);
	gfsk_tab_init(&tab_gen, p.dev_hz, GFSK_MOD_IF_HZ);
	ubt_tx_burst(&p, &b);
	zassert_equal(b.kind, GFSK_BURST);
	zassert_true(b.msb_first);
	gfsk_mod_start(&mod, &tab_gen, &lut, &b);
	gfsk_mod_gen(&mod, wbuf, 0, mod.total);
	errors = demod_errors(wbuf, mod.total, &b, IF_HZ, NULL);
	zassert_equal(errors, 0U);
	/* 544 us of data plus lead and trail: longer than one DAC bank */
	zassert_true(mod.total > 16380U);
}

ZTEST(ubertooth_tx, test_adv_over_the_air)
{
	static const uint8_t mac[6] = {0x22, 0x44, 0x66, 0x88, 0xaa, 0xcc};
	static const uint8_t adv[3] = {0x02, 0x01, 0x05};
	struct ubt_tx_pkt p;
	struct gfsk_burst b;
	uint32_t aa = 0;
	unsigned int found = 0;

	zassert_true(ubt_tx_build_adv(&p, mac, adv, 3, 2426, UBT_TX_ADV_CRC_INIT_REV) > 0);
	ubt_tx_burst(&p, &b);
	gfsk_mod_start(&mod, &tab_ble, &lut, &b);
	gfsk_mod_gen(&mod, wbuf, 0, mod.total);
	zassert_equal(demod_errors(wbuf, mod.total, &b, IF_HZ, NULL), 0U);
	/* The receiver finds the access address in the demodulated stream like a sniffer does. */
	txref_dump(fbuf, 4U * 40U, p.nbits, IF_HZ, symmean);
	for (unsigned int s = 0; s < p.nbits; s++) {
		aa = (aa >> 1) | ((symmean[s] > 0 ? 1U : 0U) << 31);
		if (s >= 39U && aa == 0x8e89bed6U) {
			found = s - 31U;
		}
	}
	zassert_equal(found, 8U, "access address found at bit %u", found);
	/* whole packet 376 bits at most: fits one DAC bank with the ramps */
	zassert_true(mod.total < 16380U);
}

ZTEST(ubertooth_tx, test_pattern_generator)
{
	struct ubt_tx_pkt a, b, c;

	zassert_equal(ubt_tx_build_test(&a, MOD_BT_LOW_ENERGY, 2441, UBT_TX_TEST_PRBS, 1), 400);
	zassert_equal(a.dev_hz, 250000);
	zassert_true(a.loop && !a.carrier && !a.msb_first);
	zassert_equal(ubt_tx_build_test(&b, MOD_BT_BASIC_RATE, 2441, UBT_TX_TEST_PRBS, 1), 400);
	zassert_equal(b.dev_hz, 160156);
	zassert_mem_equal(a.bits, b.bits, 50U, "same seed, same bits");
	zassert_equal(ubt_tx_build_test(&c, MOD_BT_LOW_ENERGY, 2441, UBT_TX_TEST_PRBS, 77), 400);
	zassert_true(memcmp(a.bits, c.bits, 50U) != 0, "different seed");
	zassert_equal(ubt_tx_build_test(&c, MOD_NONE, 2441, UBT_TX_TEST_PRBS, 1), -EINVAL);
	zassert_equal(ubt_tx_build_test(&c, MOD_80211_FHSS, 2441, UBT_TX_TEST_PRBS, 1), -EINVAL);
	for (uint16_t seed = 0; seed < 600; seed += 7) {
		unsigned int ones = 0, runs = 1, longest = 1, run = 1;

		zassert_equal(
			ubt_tx_build_test(&c, MOD_BT_LOW_ENERGY, 2402, UBT_TX_TEST_PRBS, seed),
			400);
		for (unsigned int i = 0; i < 400U; i++) {
			unsigned int bit = (c.bits[i / 8U] >> (i % 8U)) & 1U;

			ones += bit;
			if (i > 0U) {
				unsigned int prev = (c.bits[(i - 1U) / 8U] >> ((i - 1U) % 8U)) & 1U;

				if (prev == bit) {
					run++;
				} else {
					run = 1;
					runs++;
				}
				longest = MAX(longest, run);
			}
		}
		zassert_equal(ones, 200U, "balanced, seed %u", seed);
		/* random-like: about 200 runs, none long */
		zassert_true(runs > 150U && runs < 250U && longest <= 16U, "runs %u longest %u",
			     runs, longest);
	}
}
