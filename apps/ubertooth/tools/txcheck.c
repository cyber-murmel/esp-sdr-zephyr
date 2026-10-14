/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host cross-check of the Ubertooth transmit and spectrum analyser math, with the
 * reference receiver and ideal model of the unit test (tests/unit/ubertooth_tx/src/txref.h).
 *
 *   txcheck mod [-d dev_hz] [-i if_hz] [-n bits] [-r ramp] [-e hold] [-s seed] [-o words.txt]
 *       Random burst through the modulator: bit errors of the discriminator receiver, weakest
 *       symbol, error vector against the ideal GFSK, envelope, spectrum occupancy and mask,
 *       carrier placement. -o writes the DAC words (hex, one per line).
 *   txcheck adv <mac aa:bb:cc:dd:ee:ff> <channel mhz> [adv data hex]
 *       The BTLE_SLAVE packet: PDU, CRC and the air bytes, decoded with upstream's receive path.
 *   txcheck tables
 *       The constant tables of gfsk_mod.c and ubt_specan.c as C source, from libm.
 *   txcheck specan [retune_us] [capture_us]
 *       Plans for typical ranges and the sweep rate table.
 *
 * Build, from this directory:
 *   cc -O2 -I../src -o txcheck txcheck.c ../src/gfsk_mod.c ../src/ubt_tx.c ../src/ubt_specan.c \
 *      ../src/upstream/bluetooth_le.c -lm
 */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfsk_mod.h"
#include "ubt_specan.h"
#include "ubt_tx.h"
#include "upstream/bluetooth_le.h"
#include "../../../tests/unit/ubertooth_tx/src/txref.h"

#define NFFT (1U << 17)

static uint32_t rng_state = 0x2545f491U;

static uint32_t rnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static struct gfsk_lut lut;
static struct gfsk_tab tab;
static struct gfsk_mod mod;
static uint32_t words[NFFT];
static double freq[NFFT], symmean[NFFT / 40U], phase[NFFT], re[NFFT], im[NFFT], psd[NFFT];

static const char *arg_of(int argc, char **argv, const char *flag, const char *def)
{
	for (int i = 2; i + 1 < argc; i++) {
		if (strcmp(argv[i], flag) == 0) {
			return argv[i + 1];
		}
	}
	return def;
}

static int cmd_mod(int argc, char **argv)
{
	long dev = atol(arg_of(argc, argv, "-d", "250000"));
	long if_hz = atol(arg_of(argc, argv, "-i", "4000000"));
	unsigned int nbits = (unsigned int)atoi(arg_of(argc, argv, "-n", "1500"));
	unsigned int ramp = (unsigned int)atoi(arg_of(argc, argv, "-r", "120"));
	int hold = atoi(arg_of(argc, argv, "-e", "0"));
	const char *out = arg_of(argc, argv, "-o", NULL);
	static uint8_t bits[4096];
	struct gfsk_burst b = {.kind = GFSK_BURST,
			       .bits = bits,
			       .nbits = nbits,
			       .end = hold ? GFSK_END_HOLD : GFSK_END_CENTRE,
			       .lead = 4,
			       .trail = 4,
			       .ramp = (uint16_t)ramp};
	struct txref_burst rb;
	uint32_t total;
	unsigned int errors = 0;
	double minabs = 1e12, e2 = 0, sig = 0, rmin = 1e9, rmax = 0, all;

	rng_state ^= (uint32_t)atoi(arg_of(argc, argv, "-s", "0")) * 2654435761U;
	if (nbits == 0U || nbits > 4096U * 8U - 8U || (nbits + 8U) * 40U > NFFT) {
		fprintf(stderr, "bits: 1 to %u\n", NFFT / 40U - 8U);
		return 2;
	}
	for (unsigned int i = 0; i < sizeof(bits); i++) {
		bits[i] = (uint8_t)(rnd() >> 11);
	}
	gfsk_lut_init(&lut, GFSK_MOD_AMP, false);
	gfsk_tab_init(&tab, (int32_t)dev, (int32_t)if_hz);
	total = gfsk_mod_start(&mod, &tab, &lut, &b);
	gfsk_mod_gen(&mod, words, 0, total);
	rb = (struct txref_burst){
		.bits = bits, .nbits = nbits, .lead = 4, .trail = 4, .hold = hold};

	printf("burst: %u bits, %u samples (%.1f us), dev %ld Hz, IF %ld Hz, LUT %u bits (%zu "
	       "bytes), tab %zu bytes\n",
	       nbits, total, total / 40.0, dev, if_hz, GFSK_LUT_BITS, sizeof(lut), sizeof(tab));

	txref_freq(words, total, freq);
	txref_dump(freq, 4U * 40U, nbits, (double)if_hz, symmean);
	for (unsigned int s = 0; s < nbits; s++) {
		errors += (symmean[s] > 0) != (txref_bit(bits, s, 0) != 0);
		minabs = fabs(symmean[s]) < minabs ? fabs(symmean[s]) : minabs;
	}
	printf("receiver: %u bit errors of %u, weakest symbol %.0f Hz (%.0f %% of the deviation)\n",
	       errors, nbits, minabs, 100.0 * minabs / (double)dev);

	txref_phase(&rb, (double)dev, (double)if_hz, 0.5, total, phase);
	for (unsigned int n = ramp; n < total - ramp; n++) {
		double a = 2.0 * TXREF_PI * phase[n];
		double di = txref_i(words[n]) - GFSK_MOD_AMP * cos(a),
		       dq = txref_q(words[n]) - GFSK_MOD_AMP * sin(a);
		double r = hypot(txref_i(words[n]), txref_q(words[n]));

		e2 += di * di + dq * dq;
		sig += (double)GFSK_MOD_AMP * GFSK_MOD_AMP;
		rmin = r < rmin ? r : rmin;
		rmax = r > rmax ? r : rmax;
	}
	printf("error vector against the ideal GFSK: %.1f dB (%.2f %% rms); envelope %.1f to %.1f "
	       "of %d (%.2f %% p-p)\n",
	       10.0 * log10(e2 / sig), 100.0 * sqrt(e2 / sig), rmin, rmax, GFSK_MOD_AMP,
	       100.0 * (rmax - rmin) / GFSK_MOD_AMP);

	txref_psd(words, total, NFFT, re, im, psd);
	all = txref_band(psd, NFFT, -20e6, 20e6);
	printf("power within +-0.5 MHz of the carrier %.2f %%, +-0.6 MHz %.2f %%, +-1 MHz %.3f "
	       "%%\n",
	       100.0 * txref_band(psd, NFFT, (double)if_hz - 0.5e6, (double)if_hz + 0.5e6) / all,
	       100.0 * txref_band(psd, NFFT, (double)if_hz - 0.6e6, (double)if_hz + 0.6e6) / all,
	       100.0 * txref_band(psd, NFFT, (double)if_hz - 1e6, (double)if_hz + 1e6) / all);
	printf("1 MHz bands, dBc to the total power:\n  offset MHz    below     above\n");
	for (double off = 1.0; off <= 12.0; off += off < 4.0 ? 0.5 : 2.0) {
		double lo = txref_band(psd, NFFT, (double)if_hz - off * 1e6 - 0.5e6,
				       (double)if_hz - off * 1e6 + 0.5e6);
		double hi = txref_band(psd, NFFT, (double)if_hz + off * 1e6 - 0.5e6,
				       (double)if_hz + off * 1e6 + 0.5e6);

		printf("  %8.1f  %8.1f  %8.1f\n", off, 10.0 * log10(lo / all),
		       10.0 * log10(hi / all));
	}
	{
		double image = txref_band(psd, NFFT, -(double)if_hz - 1e6, -(double)if_hz + 1e6);

		printf("mirror image at -IF: %.1f dBc\n", 10.0 * log10(image / all));
	}
	if (out != NULL) {
		FILE *f = fopen(out, "w");

		if (f == NULL) {
			perror(out);
			return 1;
		}
		for (uint32_t n = 0; n < total; n++) {
			fprintf(f, "%05x\n", words[n]);
		}
		fclose(f);
	}
	return errors != 0U;
}

static int parse_hex(const char *s, uint8_t *out, unsigned int max)
{
	unsigned int n = 0;

	for (; s[0] != '\0' && s[1] != '\0' && n < max; s += 2) {
		unsigned int v;

		if (sscanf(s, "%2x", &v) != 1) {
			return -1;
		}
		out[n++] = (uint8_t)v;
	}
	return (int)n;
}

static int cmd_adv(int argc, char **argv)
{
	uint8_t mac[6], adv[UBT_TX_ADV_DATA_MAX + 1U] = {0x02, 0x01, 0x05};
	unsigned int advlen = 3, m[6];
	struct ubt_tx_pkt p;
	uint8_t rx[64], pdu[40];
	int n;

	if (argc < 4 || sscanf(argv[2], "%2x:%2x:%2x:%2x:%2x:%2x", &m[0], &m[1], &m[2], &m[3],
			       &m[4], &m[5]) != 6) {
		fprintf(stderr,
			"usage: txcheck adv aa:bb:cc:dd:ee:ff channel_mhz [adv data hex]\n");
		return 2;
	}
	for (int i = 0; i < 6; i++) {
		mac[i] = (uint8_t)m[i];
	}
	if (argc > 4) {
		n = parse_hex(argv[4], adv, sizeof(adv));
		if (n < 0) {
			fprintf(stderr, "bad adv data\n");
			return 2;
		}
		advlen = (unsigned int)n;
	}
	n = ubt_tx_build_adv(&p, mac, adv, advlen, (uint16_t)atoi(argv[3]),
			     UBT_TX_ADV_CRC_INIT_REV);
	if (n < 0) {
		fprintf(stderr, "ubt_tx_build_adv: %d\n", n);
		return 1;
	}
	n = ubt_tx_adv_pdu(pdu, mac, adv, advlen, false);
	printf("channel %u MHz = index %u, %u bits (%u us), deviation %d Hz\n", p.channel_mhz,
	       ubt_tx_adv_index(p.channel_mhz), p.nbits, p.nbits, p.dev_hz);
	printf("PDU    :");
	for (int i = 0; i < n; i++) {
		printf(" %02x", pdu[i]);
	}
	printf("\nair    :");
	for (unsigned int i = 0; i < p.nbits / 8U; i++) {
		printf(" %02x", p.bits[i]);
	}
	printf("\n(preamble, access address d6 be 89 8e, whitened PDU, whitened CRC; every byte "
	       "sent low bit first)\n");
	/* the receive path of a sniffer built from upstream code */
	memset(rx, 0, sizeof(rx));
	for (unsigned int i = 0; i < 8U * (unsigned int)(n + 3); i++) {
		if (txref_bit(p.bits, 40U + i, 0)) {
			rx[i / 8U] |= (uint8_t)(0x80U >> (i % 8U));
		}
	}
	le_dewhiten(rx, (unsigned int)n + 3U, p.channel_mhz);
	{
		uint32_t crc = btle_crcgen_lut(UBT_TX_ADV_CRC_INIT_REV, rx, n);

		printf("le_dewhiten() + btle_crcgen_lut(): PDU %s, CRC %02x %02x %02x %s\n",
		       memcmp(rx, pdu, (size_t)n) == 0 ? "matches" : "DIFFERS", rx[n], rx[n + 1],
		       rx[n + 2],
		       (rx[n] == (uint8_t)crc && rx[n + 1] == (uint8_t)(crc >> 8) &&
			rx[n + 2] == (uint8_t)(crc >> 16))
			       ? "ok"
			       : "BAD");
	}
	return 0;
}

static void print_table(const char *name, const long *v, unsigned int n, unsigned int per_line)
{
	printf("/* %s */\n", name);
	for (unsigned int i = 0; i < n; i++) {
		printf("%ld,%s", v[i], (i % per_line == per_line - 1U || i == n - 1U) ? "\n" : " ");
	}
}

static int cmd_tables(void)
{
	static long v[257];
	double g0, g1;

	g0 = txref_pulse(-1.5, 0.5);
	g1 = txref_pulse(1.5, 0.5);
	for (unsigned int j = 0; j <= 120U; j++) {
		v[j] = lround(32768.0 * (txref_pulse(j / 40.0 - 1.5, 0.5) - g0) / (g1 - g0));
	}
	print_table(
		"gfsk_mod.c gauss: integrated Gaussian pulse, BT 0.5, Q15, 3 symbols x 40 points",
		v, 121, 13);
	for (unsigned int i = 0; i <= 256U; i++) {
		v[i] = lround(32768.0 * sin(TXREF_PI / 2 * i / 256.0));
	}
	print_table("gfsk_mod.c qsin: 32768 sin(pi/2 i/256)", v, 257, 13);
	for (unsigned int n = 0; n <= 128U; n++) {
		v[n] = lround(32768.0 * 0.5 * (1.0 - cos(2.0 * TXREF_PI * n / 256.0)));
	}
	print_table("ubt_specan.c hann: periodic Hann, Q15, half", v, 129, 15);
	for (unsigned int i = 0; i <= 32U; i++) {
		v[i] = lround(65536.0 * log2(1.0 + i / 32.0));
	}
	print_table("ubt_specan.c log2_tab", v, 33, 11);
	for (unsigned int i = 0; i < 128U; i++) {
		unsigned int r = 0;

		for (unsigned int b = 0; b < 7U; b++) {
			r |= ((i >> b) & 1U) << (6U - b);
		}
		v[2U * i] = lround(32767.0 * cos(2.0 * TXREF_PI * r / 256.0));
		v[2U * i + 1U] = lround(32767.0 * sin(2.0 * TXREF_PI * r / 256.0));
	}
	print_table("ubt_specan.c ubt_specan_w256 (esp-dsp layout: cos, sin, bit reversed)", v, 256,
		    13);
	return 0;
}

static int cmd_specan(int argc, char **argv)
{
	struct ubt_specan_model m = {.retune_us = (uint32_t)(argc > 2 ? atoi(argv[2]) : 3500),
				     .capture_us = (uint32_t)(argc > 3 ? atoi(argv[3]) : 205),
				     .decode_us = 22,
				     .fft_us = 16,
				     .accum_us = 11,
				     .sweep_us = 150};
	const unsigned int ranges[][2] = {
		{2402, 2480}, {2400, 2483}, {2412, 2432}, {2440, 2440}, {2049, 3072}};
	struct ubt_specan_plan p;

	printf("placements (usable +-%u MHz, skip +-%u):\n", UBT_SPECAN_SPAN_MHZ,
	       UBT_SPECAN_EXCL_MHZ - 1U);
	for (unsigned int r = 0; r < sizeof(ranges) / sizeof(ranges[0]); r++) {
		int ret = ubt_specan_plan(&p, (uint16_t)ranges[r][0], (uint16_t)ranges[r][1], NULL);

		printf("  %u..%u: ", ranges[r][0], ranges[r][1]);
		if (ret != 0) {
			printf("error %d\n", ret);
			continue;
		}
		printf("%u LO:", p.n_lo);
		for (unsigned int i = 0; i < p.n_lo && i < 6U; i++) {
			printf(" %u", p.lo_mhz[i]);
		}
		printf("%s\n", p.n_lo > 6U ? " ..." : "");
	}
	printf("\nretune %u us, capture %u us; per frame decode %u + FFT %u + accumulate %u us\n",
	       m.retune_us, m.capture_us, m.decode_us, m.fft_us, m.accum_us);
	printf("LOs frames  DSP/placement  overlapped sweep (rate)   serial sweep (rate)\n");
	for (unsigned int n = 1; n <= 3; n++) {
		for (unsigned int f = 32; f <= 63; f += 31) {
			uint32_t ov, ser;

			m.overlap = true;
			ov = ubt_specan_sweep_us(n, f, &m);
			m.overlap = false;
			ser = ubt_specan_sweep_us(n, f, &m);
			printf("%3u %5u  %9u us   %9u us %5.0f/s   %9u us %5.0f/s\n", n, f,
			       f * (m.decode_us + m.fft_us + m.accum_us), ov, 1e6 / ov, ser,
			       1e6 / ser);
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 2 && strcmp(argv[1], "mod") == 0) {
		return cmd_mod(argc, argv);
	}
	if (argc >= 2 && strcmp(argv[1], "adv") == 0) {
		return cmd_adv(argc, argv);
	}
	if (argc >= 2 && strcmp(argv[1], "tables") == 0) {
		return cmd_tables();
	}
	if (argc >= 2 && strcmp(argv[1], "specan") == 0) {
		return cmd_specan(argc, argv);
	}
	fprintf(stderr, "usage: txcheck mod|adv|tables|specan ... (see the header of txcheck.c)\n");
	return 2;
}
