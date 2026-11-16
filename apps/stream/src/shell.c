/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The "sdr" shell command: receiver and transmitter controls and tests. The
 * receive stream picks changes up per burst.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/sys_heap.h>

#include <esp_attr.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_dsp.h>

#include "app_shell.h"
#include "vrt_rx.h"

#if defined(CONFIG_APP_TX)
#include <esp_sdr/esp_sdr_tx.h>

#include "vrt_tx.h"
#endif

extern struct k_heap _system_heap;

static int cmd_mem(const struct shell *sh, size_t argc, char **argv)
{
	struct sys_memory_stats st;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (sys_heap_runtime_stats_get(&_system_heap.heap, &st) != 0) {
		shell_error(sh, "no heap stats");
		return -EIO;
	}
	shell_print(sh, "system heap: %u allocated, %u free, %u peak",
		    (unsigned int)st.allocated_bytes, (unsigned int)st.free_bytes,
		    (unsigned int)st.max_allocated_bytes);
	return 0;
}

#if defined(CONFIG_APP_TX)

/*
 * Transmit synthesis. The DAC runs at its own rate (40 or 80 MS/s, never the
 * 16 MS/s the receiver uses), so every generator works in Hz and seconds and
 * converts with esp_sdr_tx_rate_hz(). One buffer serves all of them.
 */
#define TX_RATE        ESP_SDR_RATE_40MSPS
#define TX_BUF_SAMPLES 2048U /* 51 us at 40 MS/s; RAM below the capture bank is tight */
/* PSRAM: only CPU copies read it, the DAC plays from the bank. */
static EXT_RAM_BSS_ATTR uint32_t tx_buf[TX_BUF_SAMPLES];

static enum esp_sdr_rate tx_rate_arg(size_t argc, char **argv, size_t idx)
{
	return argc > idx ? (enum esp_sdr_rate)strtol(argv[idx], NULL, 0) : TX_RATE;
}

/*
 * Quick link check: a steady tone (no modulation), to confirm TX actually
 * reaches the other board at a sane level and offset.
 */
static int cmd_tonetx(const struct shell *sh, size_t argc, char **argv)
{
	double req = argc > 1 ? strtod(argv[1], NULL) : 1000000.0;
	uint32_t ms = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 0U;
	long amp = argc > 3 ? strtol(argv[3], NULL, 0) : 511;
	enum esp_sdr_rate rate = tx_rate_arg(argc, argv, 4);
	double fs = (double)esp_sdr_tx_rate_hz(rate);
	uint32_t bursts = 0;
	long cycles = 0;
	size_t n;
	int ret;

	if (fs == 0.0) {
		shell_error(sh, "transmit rate index must be 0 (80 MS/s) or 1 (40 MS/s)");
		return -EINVAL;
	}
	if (amp <= 0 || amp > 511 || fabs(req) >= fs / 2.0) {
		shell_error(sh, "amplitude must be 1..511 and |hz| below %.0f", fs / 2.0);
		return -EINVAL;
	}
	n = esp_sdr_tone_period(req, fs, ESP_SDR_SAMPLES_MIN, TX_BUF_SAMPLES, &cycles);
	for (size_t k = 0; k < n; k++) {
		double phase = esp_sdr_tone_phase(cycles, k, n);

		tx_buf[k] = esp_sdr_tx_word((int16_t)lround((double)amp * cos(phase)),
					    (int16_t)lround((double)amp * sin(phase)));
	}
	ret = esp_sdr_tx_play_for(rate, tx_buf, n, ms, &bursts);
	if (ret != 0) {
		shell_error(sh, "play: %d", ret);
		return ret;
	}
	shell_print(sh,
		    "tonetx: %u MHz %+.0f Hz, %u MS/s, period %u, amp %ld, txgain %d, %u bursts in %u ms",
		    esp_sdr_get_freq(), (double)cycles * fs / (double)n, (unsigned int)(fs / 1e6),
		    (unsigned int)n, amp, esp_sdr_tx_get_gain(), bursts, ms);
	return 0;
}

static int cmd_sweep(const struct shell *sh, size_t argc, char **argv)
{
	float f0 = strtof(argv[1], NULL), f1 = strtof(argv[2], NULL);
	uint32_t ms = (uint32_t)strtoul(argv[3], NULL, 0);
	int amp = argc > 4 ? (int)strtol(argv[4], NULL, 0) : 256;
	enum esp_sdr_rate rate = tx_rate_arg(argc, argv, 5);
	struct esp_sdr_tx_sweep_stats st;
	int ret = esp_sdr_tx_sweep(rate, f0, f1, ms, amp, &st);
	double fs = (double)esp_sdr_tx_rate_hz(rate);

	if (ret != 0) {
		shell_error(sh, "sweep: %d (|hz| below rate/2, ms 1..%u, amp 1..511)", ret,
			    ESP_SDR_TX_PLAY_MAX_MS);
		return ret;
	}
	shell_print(sh,
		    "sweep: %+.0f -> %+.0f Hz in %u ms at %u MS/s, txgain %d, amp %d: %u segments, "
		    "%u samples, duty %.0f %%",
		    (double)f0, (double)f1, st.elapsed_ms, (unsigned int)(fs / 1e6),
		    esp_sdr_tx_get_gain(), amp, st.segments, st.samples,
		    100.0 * st.samples / fs / (st.elapsed_ms / 1e3));
	return 0;
}

#define SDR_TX_CMD                                                                                \
	SHELL_CMD_ARG(tonetx, NULL, "[hz] [ms] [amp] [rate 0|1] tone at 80|40 MS/s, default +1 MHz",\
		      cmd_tonetx, 1, 4),                                                          \
	SHELL_CMD_ARG(sweep, NULL, "<f0_hz> <f1_hz> <ms> [amp] [rate 0|1] linear frequency sweep",\
		      cmd_sweep, 4, 2),
#else
#define SDR_TX_CMD
#endif

static int set_freq(const struct shell *sh, const char *value)
{
	long mhz;

	if (app_shell_long(sh, value, ESP_SDR_FREQ_MIN_MHZ, ESP_SDR_FREQ_MAX_MHZ, &mhz) != 0) {
		return -EINVAL;
	}
	return esp_sdr_set_freq((uint32_t)mhz);
}

static int set_rxgain(const struct shell *sh, const char *value)
{
	long g = ESP_SDR_RX_GAIN_AUTO;

	if (strcmp(value, "auto") != 0 &&
	    app_shell_long(sh, value, ESP_SDR_RX_GAIN_AUTO, esp_sdr_rx_gain_max(), &g) != 0) {
		return -EINVAL;
	}
	return esp_sdr_rx_set_gain((int)g);
}

static int set_decim(const struct shell *sh, const char *value)
{
	if (strcmp(value, "cic") == 0) {
		return vrt_rx_set_decim(VRT_RX_DECIM_CIC);
	}
	if (strcmp(value, "fold") == 0) {
		return vrt_rx_set_decim(VRT_RX_DECIM_FOLD);
	}
	shell_error(sh, "decim: cic or fold");
	return -EINVAL;
}

#if defined(CONFIG_APP_TX)
static int set_txgain(const struct shell *sh, const char *value)
{
	long step;

	if (app_shell_long(sh, value, 0, esp_sdr_tx_gain_max(), &step) != 0) {
		return -EINVAL;
	}
	return esp_sdr_tx_set_gain((int)step);
}

static void print_tx_status(const struct shell *sh)
{
	struct esp_sdr_tx_stats bs;
	struct esp_sdr_tx_dac_stats ds;
	struct vrt_tx_stats st;
	uint64_t freq;
	uint32_t rate;

	vrt_tx_get_stats(&st);
	vrt_tx_get_settings(&freq, &rate);
	esp_sdr_tx_get_stats(&bs);
	esp_sdr_tx_dac_get_stats(&ds);
	shell_print(sh, "tx %s, %llu Hz, %u S/s", esp_sdr_tx_active() ? "active" : "idle",
		    (unsigned long long)freq, rate);
	shell_print(sh,
		    "packets %u data, %u commands, %u acks, %u gaps, %u bad, %u errors; "
		    "samples %u hole-filled, %u late",
		    st.data, st.commands, st.acks, st.gaps, st.bad, st.errors, st.holes, st.late);
	shell_print(sh, "backend %llu samples in %u writes, %u errors",
		    (unsigned long long)bs.samples, bs.writes, bs.errors);
	shell_print(sh,
		    "dac x%u: %llu played, %llu dropped (%.0f %% on air), %u switches, %u underruns, "
		    "%u overruns, %u errors, %u ms",
		    ds.interp, (unsigned long long)ds.played, (unsigned long long)ds.dropped,
		    ds.played + ds.dropped ? 100.0 * ds.played / (ds.played + ds.dropped) : 0.0,
		    ds.switches, ds.underruns, ds.overruns, ds.errors, ds.elapsed_us / 1000U);
	shell_print(sh, "dac interpolator: %s", ds.simd ? "vector (PIE)" : "C");
	shell_print(sh, "dac loop: %u switches, %u us mean apart, %u us max; slack min %d us, "
		    "%u restarts",
		    ds.switches, ds.switches > 1U ? ds.switch_us_sum / (ds.switches - 1U) : 0U,
		    ds.switch_us_max, ds.slack_us_min, ds.restarts);
	shell_print(sh, "dac leader: fill max %u us, woke late max %u us, switch-to-fill max %u us",
		    ds.fill_us_max, ds.wake_late_us_max, ds.idle_us_max);
	for (int f = 0; f < 2; f++) {
		shell_print(sh, "dac filler %d: %u blocks, %u ms filling, cpu %u", f,
			    ds.fill_blocks[f], ds.fill_us[f] / 1000U, ds.fill_cpu[f]);
	}
}

#endif

static const struct setting {
	const char *key, *help;
	int (*set)(const struct shell *sh, const char *value);
} settings[] = {
	{"freq", "<MHz>: LO, shared by receive and transmit", set_freq},
	{"rxgain", "auto|<index>: hardware AGC or a fixed gain index", set_rxgain},
	{"decim", "cic|fold: decimation below the capture rate (fold: full-band power)", set_decim},
#if defined(CONFIG_APP_TX)
	{"txgain", "<step>: transmit power step, 0 weakest", set_txgain},
#endif
};

/* set [<key> <value> ...]: settings, applied in order; without arguments the keys. */
static int cmd_set(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 1) {
		ARRAY_FOR_EACH(settings, k) {
			shell_print(sh, "  %-7s %s", settings[k].key, settings[k].help);
		}
		return 0;
	}
	if ((argc - 1U) % 2U != 0U) {
		shell_error(sh, "usage: set <key> <value> [<key> <value> ...]");
		return -EINVAL;
	}
	for (size_t a = 1; a + 1U < argc; a += 2U) {
		const struct setting *st = NULL;
		int ret;

		ARRAY_FOR_EACH(settings, k) {
			if (strcmp(argv[a], settings[k].key) == 0) {
				st = &settings[k];
			}
		}
		if (st == NULL) {
			shell_error(sh, "unknown key %s (sdr set lists them)", argv[a]);
			return -EINVAL;
		}
		ret = st->set(sh, argv[a + 1U]);
		shell_print(sh, "%s %s: %d", argv[a], argv[a + 1U], ret);
		if (ret != 0) {
			return ret;
		}
	}
	return 0;
}

/* status: receive settings, and the transmit path's state and counters. */
static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	int gain = esp_sdr_rx_get_gain();
	const char *decim = vrt_rx_get_decim() == VRT_RX_DECIM_FOLD ? "fold" : "cic";

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (gain < 0) {
		shell_print(sh, "rx: freq %u MHz, rxgain auto (max %d), decim %s, %u S/s, %u bit",
			    esp_sdr_get_freq(), esp_sdr_rx_gain_max(), decim, vrt_rx_get_rate(),
			    vrt_rx_get_bits());
	} else {
		shell_print(sh, "rx: freq %u MHz, rxgain %d (max %d), decim %s, %u S/s, %u bit",
			    esp_sdr_get_freq(), gain, esp_sdr_rx_gain_max(), decim, vrt_rx_get_rate(),
			    vrt_rx_get_bits());
	}
#if defined(CONFIG_APP_TX)
	shell_print(sh, "txgain %d (max %d, vendor target power %d)", esp_sdr_tx_get_gain(),
		    esp_sdr_tx_gain_max(), esp_sdr_tx_gain_power(esp_sdr_tx_get_gain()));
	print_tx_status(sh);
#endif
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sdr_cmds,
			       SHELL_CMD(status, NULL, "settings, transmit path state and counters",
					 cmd_status),
			       SHELL_CMD_ARG(set, NULL,
					     "[<key> <value> ...]: settings (without arguments: the keys)",
					     cmd_set, 1, 8),
			       SHELL_CMD(mem, NULL, "system heap use", cmd_mem),
			       SDR_TX_CMD SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(sdr, &sdr_cmds, "ESP-SDR radio", NULL);
