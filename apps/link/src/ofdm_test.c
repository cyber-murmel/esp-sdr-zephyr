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

#define RATE      ESP_SDR_RATE_80MSPS
#define RX_RATE   (cfg.rx_div == 2U ? ESP_SDR_RATE_40MSPS : ESP_SDR_RATE_80MSPS)
#define WINDOW    ESP_SDR_SAMPLES_MAX
#define TX_BANK   1
#define BITS_SEED 1U
#define BITS_MAX  (WINDOW / 4U)

/* FFT tables and buffers above the capture bank: internal SRAM, outside the libc heap. */
static ESP_SDR_HIGH_RAM struct ofdm_ctx ctx __aligned(16);
static EXT_RAM_BSS_ATTR uint8_t ref_bits[BITS_MAX], rx_bits[BITS_MAX];

static struct ofdm_cfg cfg = {
	.sample_rate_hz = 80000000U,
	.bandwidth_hz = CONFIG_APP_OFDM_BW_KHZ * 1000U,
	.channels = CONFIG_APP_OFDM_CHANNELS,
	.cp_div = 8,
	.mod = OFDM_QPSK,
	.amp = 90,
	.symbols = 0,
	/* A looped frame at most half the window: a whole copy always fits. */
	.max_samples = (WINDOW + 1U) / 2U,
	.rx_div = CONFIG_APP_OFDM_RX_DIV,
};
static bool ready;

static const char *const mod_names[OFDM_MODS] = {"bpsk", "qpsk"};

static uint32_t cyc(void)
{
	return k_cycle_get_32();
}

static int prepare(const struct shell *sh)
{
	if (!ready) {
		if (ofdm_init(&ctx, &cfg) != 0) {
			shell_error(sh, "no frame layout for these settings");
			return -EINVAL;
		}
		if (ofdm_frame_bits(&ctx) > BITS_MAX * 8U) {
			shell_error(sh, "frame too long");
			return -EINVAL;
		}
		ofdm_test_bits(ref_bits, ofdm_frame_bits(&ctx), BITS_SEED);
		ofdm_clock = cyc;
		ready = true;
	}
	return 0;
}

static void show(const struct shell *sh)
{
	size_t bits = ofdm_frame_bits(&ctx);
	float us = (float)ctx.len * 1e6f / (float)cfg.sample_rate_hz;

	shell_print(sh, "%s, %u subcarriers, %.3f MHz (asked %.3f), spacing %.1f kHz, fft %u, "
		    "cp %u, %u data symbols", mod_names[cfg.mod], cfg.channels,
		    (double)(ctx.bw_hz / 1e6f), (double)(cfg.bandwidth_hz / 1e6f),
		    (double)(ctx.spacing_hz / 1e3f), ctx.nfft, ctx.cp, ctx.ndata);
	shell_print(sh, "frame %u samples (%.1f us), %u bits: %.2f Mbit/s on air, tx rms %d, "
		    "rx at %u MS/s (fft %u)%s",
		    (unsigned int)ctx.len, (double)us, (unsigned int)bits, (double)(bits / us),
		    cfg.amp, cfg.sample_rate_hz / cfg.rx_div / 1000000U, ctx.rnfft,
		    IS_ENABLED(CONFIG_APP_OFDM_ESP_DSP) ? ", esp-dsp fft" : ", C fft");
}

static int cmd_show(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (prepare(sh) != 0) {
		return -EINVAL;
	}
	show(sh);
	return 0;
}

/* One key into @p c; false if unknown. */
static bool set_key(struct ofdm_cfg *c, const char *k, const char *v)
{
	if (strcmp(k, "bw") == 0) {
		c->bandwidth_hz = (uint32_t)(strtof(v, NULL) * 1e6f);
	} else if (strcmp(k, "ch") == 0) {
		c->channels = (unsigned int)strtoul(v, NULL, 0);
	} else if (strcmp(k, "cp") == 0) {
		c->cp_div = (unsigned int)strtoul(v, NULL, 0);
	} else if (strcmp(k, "amp") == 0) {
		c->amp = (int)strtol(v, NULL, 0);
	} else if (strcmp(k, "rxdiv") == 0) {
		c->rx_div = (unsigned int)strtoul(v, NULL, 0);
	} else if (strcmp(k, "syms") == 0) {
		c->symbols = (unsigned int)strtoul(v, NULL, 0);
	} else if (strcmp(k, "mod") == 0) {
		c->mod = strcmp(v, "bpsk") == 0 ? OFDM_BPSK
			 : strcmp(v, "qpsk") == 0 ? OFDM_QPSK : OFDM_MODS;
	} else {
		return false;
	}
	return true;
}

/* Several pairs at once: the layout is checked on the whole combination. */
static int cmd_set(const struct shell *sh, size_t argc, char **argv)
{
	struct ofdm_cfg c = cfg;

	for (size_t i = 1; i + 1 < argc; i += 2) {
		if (!set_key(&c, argv[i], argv[i + 1])) {
			argc = 0;
			break;
		}
	}
	if (argc % 2U == 0U) {
		shell_error(sh, "<key> <value> ...; keys: bw <MHz> ch <n> cp <fft/cp> "
			    "mod bpsk|qpsk amp <rms> syms <n, 0: fill> rxdiv 1|2");
		return -EINVAL;
	}
	if (ofdm_init(&ctx, &c) != 0 || c.amp < 1 || c.amp > 400) {
		ready = false;
		(void)prepare(sh);
		shell_error(sh, "no frame layout for that, unchanged");
		return -EINVAL;
	}
	cfg = c;
	ready = false;
	(void)prepare(sh);
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
	(void)ofdm_tx_build(&ctx, ref_bits, bank);
	t0 = k_cyc_to_us_floor32(cyc() - t0);
	ret = esp_sdr_tx_loop_begin(RATE);
	if (ret == 0) {
		ret = esp_sdr_tx_loop_start(TX_BANK, ctx.len);
		if (ret == 0) {
			shell_print(sh, "looping %u samples for %u ms (built in %u us, %u clipped)",
				    (unsigned int)ctx.len, ms, t0, ctx.clipped);
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
	uint32_t n = 100, found = 0, bad = 0;
	uint64_t errs = 0, prof[3] = {0}, cap_us = 0;
	float mer = 0.0f, cfo = 0.0f, mer_min = 99.0f;
	bool verbose = false;
	size_t bits;

	for (size_t i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
			n = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (strcmp(argv[i], "-v") == 0) {
			verbose = true;
		}
	}
	if (prepare(sh) != 0) {
		return -EINVAL;
	}
	bits = ofdm_frame_bits(&ctx);
	link_mac_pause(true);
	for (uint32_t k = 0; k < n; k++) {
		struct esp_sdr_rx_burst b;
		struct ofdm_rx_info info;
		uint32_t t0 = cyc(), e = 0;

		/* Two frames hold a whole copy wherever the capture starts. */
		if (esp_sdr_rx_capture(RX_RATE, MIN(WINDOW, 2U * ctx.rlen + ctx.rnfft), &b) != 0) {
			continue;
		}
		cap_us += k_cyc_to_us_floor32(cyc() - t0);
		if (ofdm_rx_decode(&ctx, b.words, b.count, rx_bits, &info) != 0) {
			continue;
		}
		found++;
		for (size_t i = 0; i < (bits + 7U) / 8U; i++) {
			uint8_t d = rx_bits[i] ^ ref_bits[i];

			if (i == (bits - 1U) / 8U && (bits & 7U) != 0U) {
				d &= (uint8_t)(0xffU << (8U - (bits & 7U)));
			}
			e += (uint32_t)__builtin_popcount(d);
		}
		errs += e;
		bad += e != 0U;
		mer += info.mer_db;
		mer_min = MIN(mer_min, info.mer_db);
		cfo += info.cfo_hz;
		for (int i = 0; i < 3; i++) {
			prof[i] += k_cyc_to_us_floor32(info.prof[i]);
		}
	}
	link_mac_pause(false);
	shell_print(sh, "found %u/%u, frame errors %u, bit errors %llu/%llu (BER %.2e)", found, n,
		    bad, errs, (uint64_t)found * bits,
		    found ? (double)errs / ((double)found * (double)bits) : 0.0);
	if (found > 0) {
		shell_print(sh, "MER %.1f dB avg, %.1f min, cfo %.1f kHz; us per frame: search %llu, "
			    "channel %llu, data %llu (capture %llu)",
			    (double)(mer / found), (double)mer_min, (double)(cfo / found / 1e3f),
			    prof[0] / found, prof[1] / found, prof[2] / found, cap_us / n);
		if (verbose) {
			print_channel(sh);
		}
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
		ctx.buf[i] = (struct ofdm_cf){(float)(i % 7) - 3.0f, (float)(i % 5) - 2.0f};
	}
	t0 = cyc();
	for (unsigned int r = 0; r < rounds; r++) {
		ofdm_test_fft(&ctx);
	}
	t0 = cyc() - t0;
	shell_print(sh, "fft %u (%s): %.2f us", ctx.nfft,
		    IS_ENABLED(CONFIG_APP_OFDM_ESP_DSP) ? "esp-dsp" : "C",
		    (double)k_cyc_to_ns_floor64(t0) / 1e3 / rounds);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(ofdm_cmds,
	SHELL_CMD(show, NULL, "Frame layout", cmd_show),
	SHELL_CMD_ARG(set, NULL, "<bw|ch|cp|mod|amp|syms|rxdiv> <value> ...", cmd_set, 3, 12),
	SHELL_CMD_ARG(tx, NULL, "Loop the test frame: [-t <s>]", cmd_tx, 1, 2),
	SHELL_CMD_ARG(rx, NULL, "Decode captures: [-n <captures>] [-v]", cmd_rx, 1, 3),
	SHELL_CMD(bench, NULL, "Time one FFT", cmd_bench),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(ofdm, &ofdm_cmds, "Raw OFDM test (MAC paused)", NULL);
