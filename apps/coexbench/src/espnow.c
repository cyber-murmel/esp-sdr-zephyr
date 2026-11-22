/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * "espnow": ESP-NOW between two boards running this image (libespnow is linked with the Wi-Fi
 * blobs, nothing gates it in Kconfig).
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>

#include <esp_now.h>
#include <esp_wifi.h>

#include "coex.h"

static bool inited;
static uint8_t peer_mac[ESP_NOW_ETH_ALEN];
static bool have_peer;
static atomic_t rx_count, rx_bytes, tx_ok, tx_fail;
static volatile int rx_rssi;
static struct k_sem send_done;

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
	ARG_UNUSED(data);
	atomic_inc(&rx_count);
	atomic_add(&rx_bytes, len);
	if (info->rx_ctrl != NULL) {
		rx_rssi = info->rx_ctrl->rssi;
	}
}

static void on_send(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
	ARG_UNUSED(info);
	atomic_inc(status == ESP_NOW_SEND_SUCCESS ? &tx_ok : &tx_fail);
	k_sem_give(&send_done);
}

static int need_init(const struct shell *sh)
{
	if (!inited) {
		shell_error(sh, "espnow init first");
		return -ENODEV;
	}
	return 0;
}

static int cmd_init(const struct shell *sh, size_t argc, char **argv)
{
	wifi_mode_t mode;
	esp_err_t err;
	uint8_t mac[6];

	if (inited) {
		shell_print(sh, "already initialised");
		return 0;
	}
	err = esp_wifi_get_mode(&mode);
	if (err == ESP_OK && mode == ESP32_WIFI_MODE_NULL) {
		/* The driver starts Wi-Fi in NULL mode: STA, without connecting. */
		err = esp_wifi_set_mode(ESP32_WIFI_MODE_STA);
	}
	if (err != ESP_OK) {
		shell_error(sh, "wifi mode: %d", err);
		return -EIO;
	}
	err = esp_now_init();
	if (err != ESP_OK) {
		shell_error(sh, "esp_now_init: %d", err);
		return -EIO;
	}
	k_sem_init(&send_done, 0, 1);
	(void)esp_now_register_recv_cb(on_recv);
	(void)esp_now_register_send_cb(on_send);
	inited = true;
	(void)esp_wifi_get_mac(WIFI_IF_STA, mac);
	shell_print(sh, "espnow up, own mac %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
		    mac[3], mac[4], mac[5]);
	return 0;
}

static int cmd_peer(const struct shell *sh, size_t argc, char **argv)
{
	esp_now_peer_info_t peer = {.channel = 0, .ifidx = WIFI_IF_STA, .encrypt = false};
	unsigned int m[6];
	esp_err_t err;

	if (need_init(sh) != 0) {
		return -ENODEV;
	}
	if (sscanf(argv[1], "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
		shell_error(sh, "mac as aa:bb:cc:dd:ee:ff");
		return -EINVAL;
	}
	for (int i = 0; i < 6; i++) {
		peer_mac[i] = m[i];
		peer.peer_addr[i] = m[i];
	}
	if (have_peer) {
		(void)esp_now_del_peer(peer_mac);
	}
	err = esp_now_add_peer(&peer);
	if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
		shell_error(sh, "add_peer: %d", err);
		return -EIO;
	}
	have_peer = true;
	shell_print(sh, "peer set (channel: the current one)");
	return 0;
}

static int cmd_send(const struct shell *sh, size_t argc, char **argv)
{
	static uint8_t payload[ESP_NOW_MAX_DATA_LEN];
	int count = atoi(argv[1]), interval = atoi(argv[2]);
	int bytes = argc > 3 ? atoi(argv[3]) : 16;
	int refused = 0;
	int64_t t0 = k_uptime_get();
	uint32_t ok0, fail0;

	if (need_init(sh) != 0) {
		return -ENODEV;
	}
	if (!have_peer || count < 1 || interval < 0 || bytes < 1 || bytes > ESP_NOW_MAX_DATA_LEN) {
		shell_error(sh, "peer first; send <count> <interval_ms> [bytes 1..%d]",
			    ESP_NOW_MAX_DATA_LEN);
		return -EINVAL;
	}
	ok0 = atomic_get(&tx_ok);
	fail0 = atomic_get(&tx_fail);
	k_sem_reset(&send_done);
	for (int i = 0; i < count; i++) {
		int64_t next = t0 + (int64_t)(i + 1) * interval;
		esp_err_t err;

		memset(payload, i, sizeof(payload));
		memcpy(payload, &i, sizeof(i));
		err = esp_now_send(peer_mac, payload, bytes);
		if (err != ESP_OK) {
			refused++;
		} else {
			/* The send callback follows the air time, or the retries' end. */
			(void)k_sem_take(&send_done, K_MSEC(2000));
		}
		int64_t left = next - k_uptime_get();

		if (left > 0) {
			k_sleep(K_MSEC(left));
		}
	}
	shell_print(sh, "sent %d: callback ok %u fail %u, refused %d, %lld ms", count,
		    (uint32_t)atomic_get(&tx_ok) - ok0, (uint32_t)atomic_get(&tx_fail) - fail0,
		    refused, k_uptime_get() - t0);
	return 0;
}

static int cmd_stats(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "rx %u bytes %u last rssi %d, tx ok %u fail %u", (uint32_t)atomic_get(&rx_count),
		    (uint32_t)atomic_get(&rx_bytes), rx_rssi, (uint32_t)atomic_get(&tx_ok),
		    (uint32_t)atomic_get(&tx_fail));
	if (argc > 1) {
		atomic_clear(&rx_count);
		atomic_clear(&rx_bytes);
		atomic_clear(&tx_ok);
		atomic_clear(&tx_fail);
		shell_print(sh, "cleared");
	}
	return 0;
}

/* esp_now.h: window in ms awake per interval (0..65535); esp_wifi.h: interval in ms (1..65535). */
static int cmd_window(const struct shell *sh, size_t argc, char **argv)
{
	int window = atoi(argv[1]), interval = atoi(argv[2]);
	esp_err_t err;

	if (need_init(sh) != 0) {
		return -ENODEV;
	}
	if (window < 0 || window > 65535 || interval < 1 || interval > 65535) {
		return -EINVAL;
	}
	err = esp_wifi_connectionless_module_set_wake_interval(interval);
	if (err == ESP_OK) {
		err = esp_now_set_wake_window(window);
	}
	shell_print(sh, "window %d ms every %d ms: %d", window, interval, err);
	return err == ESP_OK ? 0 : -EIO;
}

static int cmd_chan(const struct shell *sh, size_t argc, char **argv)
{
	int ch = atoi(argv[1]);
	uint8_t primary = 0;
	wifi_second_chan_t second;
	esp_err_t err;

	if (need_init(sh) != 0) {
		return -ENODEV;
	}
	if (ch < 1 || ch > 14) {
		return -EINVAL;
	}
	err = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
	(void)esp_wifi_get_channel(&primary, &second);
	shell_print(sh, "channel %d: %d, now %u", ch, err, primary);
	return err == ESP_OK ? 0 : -EIO;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	espnow_cmds,
	SHELL_CMD_ARG(init, NULL, "Set STA mode if needed, start ESP-NOW", cmd_init, 1, 0),
	SHELL_CMD_ARG(peer, NULL, "<mac> unicast peer, current channel", cmd_peer, 2, 0),
	SHELL_CMD_ARG(send, NULL, "<count> <interval_ms> [bytes]", cmd_send, 3, 1),
	SHELL_CMD_ARG(stats, NULL, "[clear] rx and tx counters", cmd_stats, 1, 1),
	SHELL_CMD_ARG(chan, NULL, "<1..14> primary channel (not associated)", cmd_chan, 2, 0),
	SHELL_CMD_ARG(window, NULL, "<window_ms> <interval_ms> wake window", cmd_window, 3, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(espnow, &espnow_cmds, "ESP-NOW bench", NULL);
