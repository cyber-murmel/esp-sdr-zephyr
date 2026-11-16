/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Raw OFDM test on the radio, outside the MAC (paused meanwhile): one board
 * loops a frame of known test bits on the DAC, the other decodes captures of
 * it and counts bit errors. Both must be set alike ("ofdm set").
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#include <esp_sdr/esp_sdr.h>

#include <esp_attr.h>

#include "mac.h"
#include "ofdm.h"
#include "rs.h"

/* Transmit at the layout's sample rate, receive at that divided by rx_div. */
#define TX_RATE   (cfg.sample_rate_hz == 40000000U ? ESP_SDR_RATE_40MSPS : ESP_SDR_RATE_80MSPS)
#define RX_RATE   (cfg.sample_rate_hz / cfg.rx_div == 40000000U ? ESP_SDR_RATE_40MSPS \
							       : ESP_SDR_RATE_80MSPS)
#define WINDOW    ESP_SDR_SAMPLES_MAX
#define TX_BANK   1
#define BITS_SEED 1U
#define BITS_MAX  (WINDOW / 2U)
#define HDR_BITS  64U

/* FFT tables and buffers above the capture bank: internal SRAM, outside the libc heap. */
static ESP_SDR_HIGH_RAM struct ofdm_ctx ctx __aligned(16);
/* Transmit builds are not timed: PSRAM will do. */
static EXT_RAM_BSS_ATTR struct ofdm_txbuf txb __aligned(16);
static EXT_RAM_BSS_ATTR uint8_t ref_bits[BITS_MAX], rx_bits[BITS_MAX];
static uint8_t ref_hdr[HDR_BITS / 8U], rx_hdr[HDR_BITS / 8U];
static enum ofdm_mod mod = OFDM_QPSK;

static struct ofdm_cfg cfg = {
	.sample_rate_hz = CONFIG_APP_OFDM_FS_MHZ * 1000000U,
	.bandwidth_hz = CONFIG_APP_OFDM_BW_KHZ * 1000U,
	.channels = CONFIG_APP_OFDM_CHANNELS,
	.cp_div = 8,
	.amp = 90,
	.symbols = 0,
	/* Set by layout_limit(): a whole copy must fit the receive window. */
	.max_samples = 0,
	.rx_div = CONFIG_APP_OFDM_RX_DIV,
	.pilots = 2,
	.smooth = true,
	.hdr_bits = HDR_BITS,
	.mid_pilot = true,
};
static bool ready;

static const char *const mod_names[OFDM_MODS] = {"bpsk", "qpsk", "16qam", "64qam"};

static uint32_t cyc(void)
{
	return k_cycle_get_32();
}

/*
 * Longest frame (transmit samples): the DAC bank, and one copy plus the wrap
 * margin in the receive window (ofdm_rx_window()).
 */
static void layout_limit(struct ofdm_cfg *c)
{
	size_t bank = WINDOW - 2U * OFDM_NFFT_MAX / 4U;
	size_t rx = (WINDOW - OFDM_RX_WRAP_MARGIN(OFDM_NFFT_MAX, OFDM_NFFT_MAX / 4U)) * c->rx_div;

	c->max_samples = MIN(bank, rx);
}

/* 80 or 40 MS/s; the receiver below 40 MS/s has no rate. */
static bool rates_ok(const struct ofdm_cfg *c)
{
	return (c->sample_rate_hz == 80000000U || c->sample_rate_hz == 40000000U) &&
	       c->sample_rate_hz / c->rx_div >= 40000000U;
}

enum esp_sdr_rate link_ofdm_tx_rate(void)
{
	return TX_RATE;
}

enum esp_sdr_rate link_ofdm_rx_rate(void)
{
	return RX_RATE;
}

static int prepare(const struct shell *sh)
{
	if (!ready) {
		layout_limit(&cfg);
		if (ofdm_init(&ctx, &cfg) != 0) {
			if (sh != NULL) {
				shell_error(sh, "no frame layout for these settings");
			}
			return -EINVAL;
		}
		if (ofdm_frame_bits(&ctx, OFDM_64QAM) > BITS_MAX * 8U) {
			if (sh != NULL) {
				shell_error(sh, "frame too long");
			}
			return -EINVAL;
		}
		/* A prefix of the same sequence for every modulation. */
		ofdm_test_bits(ref_bits, BITS_MAX * 8U, BITS_SEED);
		ofdm_test_bits(ref_hdr, HDR_BITS, BITS_SEED + 1U);
		ofdm_clock = cyc;
		ready = true;
	}
	return 0;
}

static void show(const struct shell *sh)
{
	size_t bits = ofdm_frame_bits(&ctx, mod);
	float us = (float)ctx.len * 1e6f / (float)cfg.sample_rate_hz;

	shell_print(sh, "%s, %u subcarriers, %.3f MHz (asked %.3f), spacing %.1f kHz, fft %u, "
		    "cp %u, %u pilots%s, %u header + %u data symbols", mod_names[mod],
		    cfg.channels, (double)(ctx.bw_hz / 1e6f), (double)(cfg.bandwidth_hz / 1e6f),
		    (double)(ctx.spacing_hz / 1e3f), ctx.nfft, ctx.cp, cfg.pilots,
		    cfg.smooth ? " smoothed" : "", ctx.nhdr, ctx.ndata);
	shell_print(sh, "frame %u samples at %u MS/s (%.1f us), %u bits: %.2f Mbit/s on air, "
		    "tx rms %d, rx at %u MS/s (fft %u)%s",
		    (unsigned int)ctx.len, cfg.sample_rate_hz / 1000000U, (double)us,
		    (unsigned int)bits, (double)(bits / us),
		    cfg.amp, cfg.sample_rate_hz / cfg.rx_div / 1000000U, ctx.rnfft,
		    IS_ENABLED(CONFIG_APP_OFDM_ESP_DSP) ? ", esp-dsp fft" : ", C fft");
}

struct ofdm_ctx *link_ofdm(void)
{
	(void)prepare(NULL);
	return &ctx;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (prepare(sh) != 0) {
		return -EINVAL;
	}
	show(sh);
	return 0;
}

/* One key into @p c (or @p m); false if unknown. */
static bool set_key(struct ofdm_cfg *c, enum ofdm_mod *m, const char *k, const char *v)
{
	if (strcmp(k, "bw") == 0) {
		c->bandwidth_hz = (uint32_t)(strtof(v, NULL) * 1e6f);
	} else if (strcmp(k, "ch") == 0) {
		c->channels = (unsigned int)strtoul(v, NULL, 0);
	} else if (strcmp(k, "cp") == 0) {
		c->cp_div = (unsigned int)strtoul(v, NULL, 0);
	} else if (strcmp(k, "amp") == 0) {
		c->amp = (int)strtol(v, NULL, 0);
	} else if (strcmp(k, "fs") == 0) {
		c->sample_rate_hz = (uint32_t)strtoul(v, NULL, 0) * 1000000U;
	} else if (strcmp(k, "rxdiv") == 0) {
		c->rx_div = (unsigned int)strtoul(v, NULL, 0);
	} else if (strcmp(k, "syms") == 0) {
		c->symbols = (unsigned int)strtoul(v, NULL, 0);
	} else if (strcmp(k, "pilots") == 0) {
		c->pilots = (unsigned int)strtoul(v, NULL, 0);
	} else if (strcmp(k, "smooth") == 0) {
		c->smooth = strtoul(v, NULL, 0) != 0U;
	} else if (strcmp(k, "mod") == 0) {
		*m = OFDM_MODS;
		for (int i = 0; i < OFDM_MODS; i++) {
			if (strcmp(v, mod_names[i]) == 0) {
				*m = (enum ofdm_mod)i;
			}
		}
	} else {
		return false;
	}
	return true;
}

/* Several pairs at once: the layout is checked on the whole combination. */
static int cmd_set(const struct shell *sh, size_t argc, char **argv)
{
	struct ofdm_cfg c = cfg;
	enum ofdm_mod m = mod;

	for (size_t i = 1; i + 1 < argc; i += 2) {
		if (!set_key(&c, &m, argv[i], argv[i + 1])) {
			argc = 0;
			break;
		}
	}
	if (argc % 2U == 0U) {
		shell_error(sh, "<key> <value> ...; keys: bw <MHz> ch <n> cp <fft/cp> "
			    "mod bpsk|qpsk|16qam|64qam amp <rms> syms <n, 0: fill> "
			    "rxdiv 1|2 fs 80|40 pilots 1..4 smooth 0|1");
		return -EINVAL;
	}
	layout_limit(&c);
	/* The MAC builds and decodes with this context: hold it off meanwhile. */
	link_mac_pause(true);
	if (m >= OFDM_MODS || !rates_ok(&c) || ofdm_init(&ctx, &c) != 0 || c.amp < 1 ||
	    c.amp > 400) {
		ready = false;
		(void)prepare(sh);
		link_mac_pause(false);
		shell_error(sh, "no frame layout for that, unchanged");
		return -EINVAL;
	}
	cfg = c;
	mod = m;
	ready = false;
	(void)prepare(sh);
	link_mac_pause(false);
	show(sh);
	return 0;
}

static int cmd_tx(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t ms = 10000, t0;
	size_t words;
	uint32_t *bank;
	int ret;

	for (size_t i = 1; i + 1 < argc; i += 2) {
		if (strcmp(argv[i], "-t") == 0) {
			ms = (uint32_t)(strtof(argv[i + 1], NULL) * 1000.0f);
		}
	}
	if (prepare(sh) != 0) {
		return -EINVAL;
	}
	link_mac_pause(true);
	bank = esp_sdr_tx_loop_buf(TX_BANK, &words);
	t0 = cyc();
	(void)ofdm_tx_build(&ctx, &txb, mod, ref_hdr, ref_bits, bank);
	t0 = k_cyc_to_us_floor32(cyc() - t0);
	ret = esp_sdr_tx_loop_begin(TX_RATE);
	if (ret == 0) {
		ret = esp_sdr_tx_loop_start(TX_BANK, ctx.len);
		if (ret == 0) {
			shell_print(sh, "looping %u samples for %u ms (built in %u us, %u clipped)",
				    (unsigned int)ctx.len, ms, t0, txb.clipped);
			k_msleep(ms);
		}
		esp_sdr_tx_loop_end();
	}
	link_mac_pause(false);
	if (ret != 0) {
		shell_error(sh, "transmit failed (%d)", ret);
	}
	return ret;
}

/* Channel magnitude per subcarrier in dB against the mean, from the last pilot. */
static void print_channel(const struct shell *sh)
{
	float mean = 0.0f;
	char line[100];
	int len = 0;

	for (unsigned int a = 0; a < cfg.channels; a++) {
		struct ofdm_cf e = ctx.eq[a];

		mean += e.re * e.re + e.im * e.im;
	}
	mean /= (float)cfg.channels;
	shell_print(sh, "channel |H| dB vs mean, subcarriers from lowest frequency:");
	for (unsigned int a = 0; a < cfg.channels; a++) {
		struct ofdm_cf e = ctx.eq[a];
		float g = e.re * e.re + e.im * e.im;
		int db = g > 0.0f ? (int)lrintf(10.0f * log10f(mean / g)) : -99;

		len += snprintf(&line[len], sizeof(line) - len, "%4d", db);
		if (len > (int)sizeof(line) - 6 || a + 1 == cfg.channels) {
			shell_print(sh, "%s", line);
			len = 0;
		}
	}
}

static int cmd_rx(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t n = 100, found = 0, bad = 0, hbad = 0;
	uint64_t errs = 0, prof[3] = {0}, cap_us = 0;
	float mer = 0.0f, cfo = 0.0f, mer_min = 99.0f;
	bool verbose = false, full = false, dump = false, cmp = false;
	uint64_t errs_f = 0, errs_d = 0;
	struct ofdm_cf sv_trk = {0}, sv_rate = {0}, sv_u = {0};
	bool sv_wrapped = false;
	size_t bits;

	for (size_t i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
			n = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (strcmp(argv[i], "-v") == 0) {
			verbose = true;
		} else if (strcmp(argv[i], "-w") == 0) {
			full = true;
		} else if (strcmp(argv[i], "-d") == 0) {
			dump = true;
		} else if (strcmp(argv[i], "-c") == 0) {
			cmp = true;
		}
	}
	if (prepare(sh) != 0) {
		return -EINVAL;
	}
	bits = ofdm_frame_bits(&ctx, mod);
	link_mac_pause(true);
	for (uint32_t k = 0; k < n; k++) {
		struct esp_sdr_rx_burst b;
		struct ofdm_rx_info info;
		uint32_t t0 = cyc(), e = 0;

		/* Two frames hold a whole copy wherever the capture starts. */
		if (esp_sdr_rx_capture(RX_RATE, full ? WINDOW : MIN(WINDOW, ofdm_rx_window(&ctx, ctx.rlen)),
				       &b) != 0) {
			continue;
		}
		cap_us += k_cyc_to_us_floor32(cyc() - t0);
		if (ofdm_rx_begin(&ctx, b.words, b.count, rx_hdr, &info) != 0) {
			continue;
		}
		found++;
		hbad += memcmp(rx_hdr, ref_hdr, sizeof(rx_hdr)) != 0;
		sv_trk = ctx.w0.trk;
		sv_rate = ctx.w0.rate;
		sv_u = ctx.w0.u;
		sv_wrapped = ctx.w0.wrapped;
		ofdm_rx_finish(&ctx, b.words, mod, rx_bits, &info);
		for (size_t i = 0; i < (bits + 7U) / 8U; i++) {
			uint8_t d = rx_bits[i] ^ ref_bits[i];

			if (i == (bits - 1U) / 8U && (bits & 7U) != 0U) {
				d &= (uint8_t)(0xffU << (8U - (bits & 7U)));
			}
			e += (uint32_t)__builtin_popcount(d);
		}
		errs += e;
		bad += e != 0U;
		if (cmp) {
			/* The same samples through the float decoder (a diagnostic pointer forces it). */
			static EXT_RAM_BSS_ATTR float dummy[OFDM_CH_MAX];
			uint32_t ef = 0;

			ctx.w0.trk = sv_trk;
			ctx.w0.rate = sv_rate;
			ctx.w0.u = sv_u;
			ctx.w0.wrapped = sv_wrapped;
			ctx.sc_err = dummy;
			ofdm_rx_finish(&ctx, b.words, mod, rx_bits, &info);
			ctx.sc_err = NULL;
			for (size_t i = 0; i < (bits + 7U) / 8U; i++) {
				uint8_t d = rx_bits[i] ^ ref_bits[i];

				if (i == (bits - 1U) / 8U && (bits & 7U) != 0U) {
					d &= (uint8_t)(0xffU << (8U - (bits & 7U)));
				}
				ef += (uint32_t)__builtin_popcount(d);
			}
			errs_f += ef;
			/* And the vector decoder on the scalar derotation. */
			if (ofdm_vec_derot) {
				ef = 0;
				ctx.w0.trk = sv_trk;
				ctx.w0.rate = sv_rate;
				ctx.w0.u = sv_u;
			ctx.w0.wrapped = sv_wrapped;
				ofdm_vec_derot = false;
				ofdm_rx_finish(&ctx, b.words, mod, rx_bits, &info);
				ofdm_vec_derot = true;
				for (size_t i = 0; i < (bits + 7U) / 8U; i++) {
					uint8_t d = rx_bits[i] ^ ref_bits[i];

					if (i == (bits - 1U) / 8U && (bits & 7U) != 0U) {
						d &= (uint8_t)(0xffU << (8U - (bits & 7U)));
					}
					ef += (uint32_t)__builtin_popcount(d);
				}
				errs_d += ef;
			}
		}
		if (dump && e != 0U) {
			/* The capture of this frame, as "link dump" prints it, then stop. */
			shell_print(sh, "frame with %u bit errors, start %d, MER %.1f dB", e, info.start,
				    (double)info.mer_db);
			shell_print(sh, "dump %u words", (unsigned int)b.count);
			for (size_t i = 0; i < b.count; i += 8) {
				char line[96];
				int len = 0;

				for (size_t j = i; j < MIN(i + 8U, b.count); j++) {
					len += snprintf(&line[len], sizeof(line) - len, "%08x ",
							b.words[j]);
				}
				shell_print(sh, "%s", line);
			}
			shell_print(sh, "dump end");
			n = k + 1U;
			break;
		}
		mer += info.mer_db;
		mer_min = MIN(mer_min, info.mer_db);
		cfo += info.cfo_hz;
		for (int i = 0; i < 3; i++) {
			prof[i] += k_cyc_to_us_floor32(info.prof[i]);
		}
	}
	link_mac_pause(false);
	shell_print(sh, "found %u/%u, header errors %u, frame errors %u, bit errors %llu/%llu "
		    "(BER %.2e)", found, n, hbad, bad, errs, (uint64_t)found * bits,
		    found ? (double)errs / ((double)found * (double)bits) : 0.0);
	if (cmp) {
		shell_print(sh, "same samples through the float decoder: bit errors %llu (BER %.2e)",
			    errs_f, found ? (double)errs_f / ((double)found * (double)bits) : 0.0);
		shell_print(sh, "same samples, scalar derotation: bit errors %llu (BER %.2e)", errs_d,
			    found ? (double)errs_d / ((double)found * (double)bits) : 0.0);
	}
	if (found > 0) {
		shell_print(sh, "MER %.1f dB avg, %.1f min, cfo %.1f kHz; us per frame: search %llu, "
			    "channel+header %llu, data %llu (capture %llu)",
			    (double)(mer / found), (double)mer_min, (double)(cfo / found / 1e3f),
			    prof[0] / found, prof[1] / found, prof[2] / found, cap_us / n);
		{
			float emax = 0.0f;

			for (unsigned int a = 0; a < cfg.channels; a++) {
				emax = MAX(emax, MAX(fabsf(ctx.eq[a].re), fabsf(ctx.eq[a].im)));
			}
			shell_print(sh, "vector equalizer: sar 16qam %d, 64qam %d (0: float decoder); "
				    "max |eq| %.4f, fscale %.3f",
				    ctx.veq[0].sar, ctx.veq[1].sar, (double)emax, (double)ctx.fscale);
		}
		if (verbose) {
			print_channel(sh);
		}
	}
	shell_print(sh, "rx done");
	return 0;
}

/*
 * Streaming test: the sender plays one frame after the other from two DAC
 * banks without ever leaving TX (the next frame is built while the DAC loops
 * the current one, and the bank switch is one DAC retrigger). Frame n carries
 * n in its header and bits that are a function of n; the receiver regenerates
 * them and counts what it saw, what it lost and the bit errors.
 */
static EXT_RAM_BSS_ATTR uint8_t stream_bits[BITS_MAX];

static uint32_t stream_seed(uint32_t seq)
{
	return seq * 0x9e3779b1U + 12345U;
}

static void stream_hdr(uint8_t *hdr, uint32_t seq)
{
	for (int i = 0; i < 4; i++) {
		hdr[i] = (uint8_t)(seq >> (8 * i));
		hdr[4 + i] = (uint8_t)(~seq >> (8 * i));
	}
}

/* SNR per subcarrier from the decision errors of QPSK frames (always decided right), in dB. */
static int cmd_snr(const struct shell *sh, size_t argc, char **argv)
{
	static EXT_RAM_BSS_ATTR float sc[OFDM_CH_MAX];
	uint32_t n = 50, found = 0;
	enum ofdm_mod keep = mod;
	char line[100];
	int len = 0;

	for (size_t i = 1; i + 1 < argc; i += 2) {
		if (strcmp(argv[i], "-n") == 0) {
			n = (uint32_t)strtoul(argv[i + 1], NULL, 0);
		}
	}
	if (prepare(sh) != 0) {
		return -EINVAL;
	}
	mod = OFDM_QPSK;
	memset(sc, 0, sizeof(sc));
	ctx.sc_err = sc;
	link_mac_pause(true);
	for (uint32_t k = 0; k < n; k++) {
		struct esp_sdr_rx_burst b;
		struct ofdm_rx_info info;

		if (esp_sdr_rx_capture(RX_RATE, MIN(WINDOW, ofdm_rx_window(&ctx, ctx.rlen)), &b) != 0 ||
		    ofdm_rx_begin(&ctx, b.words, b.count, rx_hdr, &info) != 0) {
			continue;
		}
		ofdm_rx_finish(&ctx, b.words, OFDM_QPSK, rx_bits, &info);
		found++;
	}
	link_mac_pause(false);
	ctx.sc_err = NULL;
	mod = keep;
	shell_print(sh, "snr: %u frames, per subcarrier from the lowest frequency (dB):", found);
	for (unsigned int a = 0; a < cfg.channels && found > 0; a++) {
		float e = sc[a] / (float)(found * ctx.ndata);

		len += snprintf(&line[len], sizeof(line) - len, "%3d", (int)lrintf(-10.0f * log10f(e)));
		if (len > (int)sizeof(line) - 6 || a + 1 == cfg.channels) {
			shell_print(sh, "%s", line);
			len = 0;
		}
	}
	shell_print(sh, "rx done");
	return 0;
}

static int cmd_vec(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		ofdm_vec_enable = strtoul(argv[1], NULL, 0) != 0U;
	}
	shell_print(sh, "vector payload decoder %s", ofdm_vec_enable ? "on" : "off (float)");
	return 0;
}

static int cmd_derot(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		ofdm_vec_derot = strtoul(argv[1], NULL, 0) != 0U;
	}
	shell_print(sh, "vector derotation %s", ofdm_vec_derot ? "on" : "off (scalar)");
	return 0;
}

static int cmd_sfo(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		ofdm_sfo = (int)strtol(argv[1], NULL, 0);
	}
	shell_print(sh, "sample clock drift correction %d (0: off)", ofdm_sfo);
	return 0;
}

static int cmd_txhelp(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		link_mac_tx_help(strtoul(argv[1], NULL, 0) != 0U);
	}
	shell_print(sh, "blind stream frames built on %s", link_mac_tx_help_get() ? "both CPUs" : "one CPU");
	return 0;
}

static int cmd_chavg(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		ofdm_chavg = strtoul(argv[1], NULL, 0) != 0U;
		ctx.havg_n = 0;
	}
	if (argc > 2) {
		ofdm_finerot = strtoul(argv[2], NULL, 0) != 0U;
	}
	shell_print(sh, "channel averaged over frames: %s (%u in it); fine timing by %s",
		    ofdm_chavg ? "on" : "off", ctx.havg_n, ofdm_finerot ? "turning" : "measuring again");
	return 0;
}

static int cmd_split(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		link_mac_scan_split(strtoul(argv[1], NULL, 0) != 0U);
	}
	shell_print(sh, "preamble search on %s", link_mac_scan_split_get() ? "both CPUs" : "one CPU");
	return 0;
}

static int cmd_stx(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t ms = 5000, period_us = 8000, seq = 0, over = 0, build_max = 0;
	uint8_t hdr[HDR_BITS / 8U];
	size_t words;
	int bank = 0, ret;

	for (size_t i = 1; i + 1 < argc; i += 2) {
		if (strcmp(argv[i], "-t") == 0) {
			ms = (uint32_t)(strtof(argv[i + 1], NULL) * 1000.0f);
		} else if (strcmp(argv[i], "-p") == 0) {
			period_us = (uint32_t)strtoul(argv[i + 1], NULL, 0);
		}
	}
	if (prepare(sh) != 0) {
		return -EINVAL;
	}
	link_mac_pause(true);
	ofdm_test_bits(stream_bits, ofdm_frame_bits(&ctx, mod), stream_seed(0));
	stream_hdr(hdr, 0);
	(void)ofdm_tx_build(&ctx, &txb, mod, hdr, stream_bits, esp_sdr_tx_loop_buf(bank, &words));
	ret = esp_sdr_tx_loop_begin(TX_RATE);
	if (ret == 0) {
		uint32_t start = cyc(), deadline = start;
		uint32_t period = k_us_to_cyc_ceil32(period_us);

		ret = esp_sdr_tx_loop_start(bank, ctx.len);
		while (ret == 0 && k_cyc_to_ms_floor32(cyc() - start) < ms) {
			uint32_t t0 = cyc(), b;

			seq++;
			bank ^= 1;
			ofdm_test_bits(stream_bits, ofdm_frame_bits(&ctx, mod), stream_seed(seq));
			stream_hdr(hdr, seq);
			(void)ofdm_tx_build(&ctx, &txb, mod, hdr, stream_bits,
					    esp_sdr_tx_loop_buf(bank, &words));
			b = k_cyc_to_us_floor32(cyc() - t0);
			build_max = MAX(build_max, b);
			deadline += period;
			if ((int32_t)(cyc() - deadline) > 0) {
				over++;
				deadline = cyc();
			}
			while ((int32_t)(deadline - cyc()) > 0) {
			}
			ret = esp_sdr_tx_loop_start(bank, ctx.len);
		}
		esp_sdr_tx_loop_end();
	}
	link_mac_pause(false);
	shell_print(sh, "stx done: %u frames of %u bits, period %u us, build max %u us, "
		    "%u periods overrun (%d)", seq + 1U, (unsigned int)ofdm_frame_bits(&ctx, mod),
		    period_us, build_max, over, ret);
	return ret;
}

static int cmd_srx(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t ms = 5000, caps = 0, found = 0, hdr_bad = 0, dup = 0, bad = 0, good = 0, seen = 0;
	uint32_t last = 0, first = 0, lost = 0;
	uint64_t errs = 0;
	float mer = 0.0f;
	bool have = false;
	uint32_t start;
	size_t bits;

	for (size_t i = 1; i + 1 < argc; i += 2) {
		if (strcmp(argv[i], "-t") == 0) {
			ms = (uint32_t)(strtof(argv[i + 1], NULL) * 1000.0f);
		}
	}
	if (prepare(sh) != 0) {
		return -EINVAL;
	}
	bits = ofdm_frame_bits(&ctx, mod);
	link_mac_pause(true);
	start = cyc();
	while (k_cyc_to_ms_floor32(cyc() - start) < ms) {
		struct esp_sdr_rx_burst b;
		struct ofdm_rx_info info;
		uint32_t seq, nseq, e = 0;

		if (esp_sdr_rx_capture(RX_RATE, MIN(WINDOW, ofdm_rx_window(&ctx, ctx.rlen)), &b) != 0) {
			continue;
		}
		caps++;
		if (ofdm_rx_begin(&ctx, b.words, b.count, rx_hdr, &info) != 0) {
			continue;
		}
		found++;
		seq = (uint32_t)rx_hdr[0] | (uint32_t)rx_hdr[1] << 8 | (uint32_t)rx_hdr[2] << 16 |
		      (uint32_t)rx_hdr[3] << 24;
		nseq = (uint32_t)rx_hdr[4] | (uint32_t)rx_hdr[5] << 8 | (uint32_t)rx_hdr[6] << 16 |
		       (uint32_t)rx_hdr[7] << 24;
		if (seq != ~nseq) {
			hdr_bad++;
			continue;
		}
		if (have && seq == last) {
			dup++;
			continue;
		}
		ofdm_rx_finish(&ctx, b.words, mod, rx_bits, &info);
		ofdm_test_bits(stream_bits, bits, stream_seed(seq));
		for (size_t i = 0; i < (bits + 7U) / 8U; i++) {
			uint8_t d = rx_bits[i] ^ stream_bits[i];

			if (i == (bits - 1U) / 8U && (bits & 7U) != 0U) {
				d &= (uint8_t)(0xffU << (8U - (bits & 7U)));
			}
			e += (uint32_t)__builtin_popcount(d);
		}
		if (!have) {
			first = seq;
		} else if (seq > last) {
			lost += seq - last - 1U;
		}
		have = true;
		last = seq;
		seen++;
		errs += e;
		bad += e != 0U;
		good += e == 0U;
		mer += info.mer_db;
	}
	link_mac_pause(false);
	{
		uint32_t us = k_cyc_to_us_floor32(cyc() - start);

		shell_print(sh, "srx: %u captures, %u with a frame, %u new (seq %u..%u), %u dups, "
			    "%u header errors, %u frames missed", caps, found, seen, first, last, dup,
			    hdr_bad, lost);
		shell_print(sh, "srx: %u clean, %u with bit errors (%llu bit errors, BER %.2e), "
			    "MER %.1f dB; %.2f Mbit/s clean, %.2f Mbit/s of frames seen",
			    good, bad, errs, seen ? (double)errs / ((double)seen * (double)bits) : 0.0,
			    seen ? (double)(mer / seen) : 0.0, (double)good * (double)bits / (double)us,
			    (double)seen * (double)bits / (double)us);
	}
	shell_print(sh, "rx done");
	return 0;
}

static int cmd_bench(const struct shell *sh, size_t argc, char **argv)
{
	const unsigned int rounds = 1000;
	uint32_t t0;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (prepare(sh) != 0) {
		return -EINVAL;
	}
	for (unsigned int i = 0; i < ctx.nfft; i++) {
		ctx.w0.buf[i] = (struct ofdm_cf){(float)(i % 7) - 3.0f, (float)(i % 5) - 2.0f};
	}
	t0 = cyc();
	for (unsigned int r = 0; r < rounds; r++) {
		ofdm_test_fft(&ctx);
	}
	t0 = cyc() - t0;
	shell_print(sh, "fft %u (%s): %.2f us", ctx.nfft,
		    IS_ENABLED(CONFIG_APP_OFDM_ESP_DSP) ? "esp-dsp" : "C",
		    (double)k_cyc_to_ns_floor64(t0) / 1e3 / rounds);
	/* RS(255,223) decode with e byte errors, on the stack. */
	for (unsigned int e = 0; e <= 10; e += (e == 0 ? 2 : (e == 2 ? 3 : 5))) {
		uint8_t ref[RS_N], cw[RS_N];
		uint32_t sum = 0;
		int r = 0;

		for (unsigned int i = 0; i < RS_K; i++) {
			ref[i] = (uint8_t)(i * 37U + 11U);
		}
		rs_encode(ref);
		for (unsigned int k = 0; k < 50; k++) {
			memcpy(cw, ref, RS_N);
			for (unsigned int j = 0; j < e; j++) {
				cw[(k * 31U + j * 23U) % RS_N] ^= (uint8_t)(j + 1U);
			}
			t0 = cyc();
			r = rs_decode(cw);
			sum += cyc() - t0;
		}
		shell_print(sh, "rs_decode, %u errors: %u us (returned %d)", e,
			    k_cyc_to_us_floor32(sum / 50U), r);
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(ofdm_cmds,
	SHELL_CMD(status, NULL, "Frame layout and settings", cmd_status),
	SHELL_CMD_ARG(set, NULL, "<bw|ch|cp|mod|amp|syms|rxdiv|fs|pilots|smooth> <value> ...", cmd_set,
		      3, 16),
	SHELL_CMD_ARG(tx, NULL, "Loop the test frame: [-t <s>]", cmd_tx, 1, 2),
	SHELL_CMD_ARG(rx, NULL, "Decode captures: [-n <captures>] [-v] [-w: whole window] "
		      "[-d: dump the first frame with errors]", cmd_rx, 1, 5),
	SHELL_CMD_ARG(snr, NULL, "SNR per subcarrier of a looped frame: [-n <captures>]", cmd_snr, 1, 2),
	SHELL_CMD_ARG(derot, NULL, "Payload derotation on the vector unit: [0|1]", cmd_derot, 1, 1),
	SHELL_CMD_ARG(sfo, NULL, "Sample clock drift correction: [-1|0|1]", cmd_sfo, 1, 1),
	SHELL_CMD_ARG(txhelp, NULL, "Build stream frames on both CPUs: [0|1]", cmd_txhelp, 1, 1),
	SHELL_CMD_ARG(chavg, NULL, "Channel averaged over frames, fine timing by turning: [0|1 [0|1]]",
		      cmd_chavg, 1, 2),
	SHELL_CMD_ARG(split, NULL, "Preamble search on both CPUs: [0|1]", cmd_split, 1, 1),
	SHELL_CMD_ARG(vec, NULL, "Vector payload decoder: [0|1]", cmd_vec, 1, 1),
	SHELL_CMD_ARG(stx, NULL, "Stream test frames: [-t <s>] [-p <us per frame>]", cmd_stx, 1, 4),
	SHELL_CMD_ARG(srx, NULL, "Check the stream: [-t <s>]", cmd_srx, 1, 2),
	SHELL_CMD(bench, NULL, "Time one FFT", cmd_bench),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(ofdm, &ofdm_cmds, "Raw OFDM test (MAC paused)", NULL);
