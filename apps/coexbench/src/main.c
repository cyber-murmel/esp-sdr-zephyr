/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * coexbench: Wi-Fi (station, net shell, wifi shell) beside lib/esp_sdr on one S3 radio.
 * Shell: "coex ps|listen|chan|cap|sdr|arb|log", "espnow ..." (espnow.c), the net and wifi shells.
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/app_version.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#include <esp_wifi.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_arb.h>
#include <esp_sdr/esp_sdr_rx.h>

#include "app_usb.h"
#include "coex.h"

#define CAP_RATE    ESP_SDR_RATE_40MSPS
#define CAP_SAMPLES 4096U
#define FULLSCALE   512.0f

void coex_channel_refresh(void)
{
	uint8_t primary = 0;
	wifi_second_chan_t second;

	if (esp_wifi_get_channel(&primary, &second) == ESP_OK) {
		phylog_set_channel(primary);
	}
}

uint32_t coex_chan_mhz(uint8_t chan)
{
	return chan == 14U ? 2484U : 2407U + 5U * chan;
}

static int cmd_ps(const struct shell *sh, size_t argc, char **argv)
{
	wifi_ps_type_t type;
	esp_err_t err;

	if (strcmp(argv[1], "none") == 0) {
		type = WIFI_PS_NONE;
	} else if (strcmp(argv[1], "min") == 0) {
		type = WIFI_PS_MIN_MODEM;
	} else if (strcmp(argv[1], "max") == 0) {
		type = WIFI_PS_MAX_MODEM;
	} else {
		return -EINVAL;
	}
	err = esp_wifi_set_ps(type);
	if (err == ESP_OK) {
		err = esp_wifi_get_ps(&type);
	}
	shell_print(sh, "ps now %d (0 none, 1 min, 2 max): %d", (int)type, err);
	return err == ESP_OK ? 0 : -EIO;
}

/* Applies at the next association, and only with MAX modem sleep. */
static int cmd_listen(const struct shell *sh, size_t argc, char **argv)
{
	wifi_config_t cfg;
	esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &cfg);

	if (err == ESP_OK) {
		cfg.sta.listen_interval = (uint16_t)atoi(argv[1]);
		err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
	}
	shell_print(sh, "listen interval %s: %d", argv[1], err);
	return err == ESP_OK ? 0 : -EIO;
}

static int cmd_chan(const struct shell *sh, size_t argc, char **argv)
{
	coex_channel_refresh();
	shell_print(sh, "channel %u (%u MHz)", phylog_channel(), coex_chan_mhz(phylog_channel()));
	return 0;
}

static int cmd_cap(const struct shell *sh, size_t argc, char **argv)
{
	int n = atoi(argv[1]);
	bool forced = argc > 2 && strcmp(argv[2], "forced") == 0;
	int interval = argc > 3 ? atoi(argv[3]) : 0;
	float pmin = 1e30f, pmax = -1e30f, psum = 0.0f;
	int ok = 0, ret;

	if (n < 1 || (argc > 2 && !forced && strcmp(argv[2], "passive") != 0)) {
		return -EINVAL;
	}
	coex_channel_refresh();
	if (phylog_channel() == 0U) {
		shell_error(sh, "no Wi-Fi channel");
		return -ENODEV;
	}
	/* Forced: the SDR owns the front end and the PLL in each capture's window. */
	(void)esp_sdr_rx_set_passive(!forced);
	if (forced) {
		ret = esp_sdr_set_freq(coex_chan_mhz(phylog_channel()));
		if (ret != 0) {
			shell_error(sh, "set_freq %d", ret);
		}
	}
	for (int i = 0; i < n; i++) {
		struct esp_sdr_rx_burst b;
		int64_t t0 = k_uptime_get();
		uint32_t wall;
		float acc = 0.0f, db;
		unsigned int peak = 0;
		int agc = esp_sdr_rx_agc_index();

		/* Forced: a radio window of its own (the arbiter), one capture long. */
		ret = forced ? esp_sdr_arb_claim(K_MSEC(2000), NULL) : 0;
		if (ret == 0) {
			ret = esp_sdr_rx_capture(CAP_RATE, CAP_SAMPLES, &b);
			if (forced) {
				esp_sdr_arb_release();
			}
		}
		wall = (uint32_t)(k_uptime_get() - t0);
		if (ret != 0) {
			shell_print(sh, "cap %d: error %d", i, ret);
		} else {
			for (size_t j = 0; j < b.count; j++) {
				int si = esp_sdr_rx_i(b.words[j]), sq = esp_sdr_rx_q(b.words[j]);

				acc += (float)(si * si + sq * sq);
				peak = MAX(peak, (unsigned int)MAX(abs(si), abs(sq)));
			}
			acc /= (float)b.count;
			db = 10.0f * log10f(MAX(acc, 1e-3f) / (FULLSCALE * FULLSCALE));
			pmin = MIN(pmin, db);
			pmax = MAX(pmax, db);
			psum += db;
			ok++;
			shell_print(sh, "cap %d %s ch %u: %.1f dBFS peak %u agc %d burst %u us wall %u ms",
				    i, forced ? "forced" : "passive", phylog_channel(), (double)db,
				    peak, agc, b.elapsed_us, wall);
		}
		if (interval > 0) {
			k_sleep(K_MSEC(interval));
		}
	}
	if (ok > 0) {
		shell_print(sh, "%d of %d ok, power min %.1f mean %.1f max %.1f dBFS", ok, n,
			    (double)pmin, (double)(psum / ok), (double)pmax);
	}
	/* Back to passive: the AGC owns the gain, the PHY's filter codes return. */
	(void)esp_sdr_rx_set_passive(true);
	return ok == n ? 0 : -EIO;
}

static int cmd_log(const struct shell *sh, size_t argc, char **argv)
{
	return phylog_shell(sh, argc > 1 && strcmp(argv[1], "clear") == 0);
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	coex_cmds,
	SHELL_CMD_ARG(ps, NULL, "none|min|max: esp_wifi_set_ps()", cmd_ps, 2, 0),
	SHELL_CMD_ARG(listen, NULL, "<n> listen interval in beacons (next association)", cmd_listen,
		      2, 0),
	SHELL_CMD_ARG(chan, NULL, "Wi-Fi primary channel", cmd_chan, 1, 0),
	SHELL_CMD_ARG(cap, NULL, "<n> [passive|forced] [interval_ms] burst captures", cmd_cap, 2, 2),
	SHELL_CMD_ARG(sdr, NULL,
		      "<seconds> [ring|burst] [freq_mhz] SDR windows in Wi-Fi's sleep (arbiter)",
		      coex_cmd_sdr, 2, 2),
	SHELL_CMD_ARG(arb, NULL, "[clear] radio arbiter counters", coex_cmd_arb, 1, 1),
	SHELL_CMD_ARG(log, NULL, "[clear] PHY event log and statistics", cmd_log, 1, 1),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(coex, &coex_cmds, "Coexistence bench", NULL);

int main(void)
{
	int ret;

	if (app_usb_init() != 0) {
		printk("USB failed\n");
	}
	printk("coexbench %s\n", APP_VERSION_STRING);
	/* Before init: the first retune would force the front end. Captures sample until asked. */
	(void)esp_sdr_rx_set_passive(true);
	ret = esp_sdr_init();
	if (ret != 0) {
		printk("esp_sdr_init failed (%d)\n", ret);
	}
	return 0;
}
