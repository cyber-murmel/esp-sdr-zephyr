/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The espnow module: libespnow's esp_now_ API (espnow.o) with a Zephyr-side API
 * (include/esp_sdr/espnow.h) and a shell, as the "espnow" commands of apps/coexbench:
 *
 *   espnow init                               station mode if needed, ESP-NOW up, own MAC
 *   espnow peer <aa:bb:cc:dd:ee:ff>           the unicast peer, current channel
 *   espnow send <count> <interval_ms> [bytes] frames with a counter, send status counted
 *   espnow stats [clear]                      receive and send counters
 *   espnow window <window_ms> <interval_ms>   wake window for receiving under power save
 *   espnow chan <1..14>                       primary channel, only while not associated
 *   espnow rx [n]                             the last n frames received (default 4)
 *
 * The module's imports are the wifi module's (wifi.ver); it has no internal RAM.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>

#include <esp_now.h>
#include <esp_wifi.h>

#include <esp_sdr/espnow.h>

BUILD_ASSERT(ESP_SDR_ESPNOW_MAX_DATA == ESP_NOW_MAX_DATA_LEN);

static bool inited;
static struct k_msgq *rx_q;
static esp_sdr_espnow_rx_cb rx_cb;
static void *rx_user;
static atomic_t rx_count, rx_bytes, rx_dropped, tx_ok, tx_fail, tx_refused;
static volatile int rx_rssi;
static volatile bool tx_good;
static K_SEM_DEFINE(send_done, 0, 1);

/* The last frames, for "espnow rx". */
#define LAST_N 8
static struct esp_sdr_espnow_msg last[LAST_N];
static uint32_t last_head;

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
	struct esp_sdr_espnow_msg msg;
	uint32_t n = len > ESP_SDR_ESPNOW_MAX_DATA ? ESP_SDR_ESPNOW_MAX_DATA : len;

	atomic_inc(&rx_count);
	atomic_add(&rx_bytes, len);
	memcpy(msg.src, info->src_addr, sizeof(msg.src));
	msg.rssi = info->rx_ctrl != NULL ? info->rx_ctrl->rssi : 0;
	msg.len = n;
	memcpy(msg.data, data, n);
	rx_rssi = msg.rssi;
	last[last_head++ % LAST_N] = msg;
	if (rx_q != NULL && k_msgq_put(rx_q, &msg, K_NO_WAIT) != 0) {
		atomic_inc(&rx_dropped);
	}
	if (rx_cb != NULL) {
		rx_cb(&msg, rx_user);
	}
}

static void on_send(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
	ARG_UNUSED(info);
	tx_good = status == ESP_NOW_SEND_SUCCESS;
	atomic_inc(tx_good ? &tx_ok : &tx_fail);
	k_sem_give(&send_done);
}

static int err_to_errno(esp_err_t err)
{
	switch (err) {
	case ESP_OK:
		return 0;
	case ESP_ERR_ESPNOW_NOT_INIT:
		return -ENODEV;
	case ESP_ERR_ESPNOW_ARG:
		return -EINVAL;
	case ESP_ERR_ESPNOW_NO_MEM:
		return -ENOMEM;
	case ESP_ERR_ESPNOW_FULL:
		return -ENOSPC;
	case ESP_ERR_ESPNOW_NOT_FOUND:
		return -ENOENT;
	default:
		return -EIO;
	}
}

int esp_sdr_espnow_init(void)
{
	wifi_mode_t mode;
	esp_err_t err;

	if (inited) {
		return 0;
	}
	err = esp_wifi_get_mode(&mode);
	if (err == ESP_OK && mode == ESP32_WIFI_MODE_NULL) {
		/* The driver starts Wi-Fi in NULL mode: station, without connecting. */
		err = esp_wifi_set_mode(ESP32_WIFI_MODE_STA);
	}
	if (err == ESP_OK) {
		err = esp_now_init();
	}
	if (err != ESP_OK) {
		return err_to_errno(err);
	}
	k_sem_reset(&send_done);
	(void)esp_now_register_recv_cb(on_recv);
	(void)esp_now_register_send_cb(on_send);
	inited = true;
	return 0;
}

int esp_sdr_espnow_mac(uint8_t mac[ESP_SDR_ESPNOW_ALEN])
{
	return err_to_errno(esp_wifi_get_mac(WIFI_IF_STA, mac));
}

int esp_sdr_espnow_peer_add(const uint8_t mac[ESP_SDR_ESPNOW_ALEN], uint8_t channel)
{
	esp_now_peer_info_t peer = {.channel = channel, .ifidx = WIFI_IF_STA, .encrypt = false};
	esp_err_t err;

	if (!inited) {
		return -ENODEV;
	}
	memcpy(peer.peer_addr, mac, sizeof(peer.peer_addr));
	err = esp_now_add_peer(&peer);
	return err == ESP_ERR_ESPNOW_EXIST ? 0 : err_to_errno(err);
}

int esp_sdr_espnow_peer_del(const uint8_t mac[ESP_SDR_ESPNOW_ALEN])
{
	return inited ? err_to_errno(esp_now_del_peer(mac)) : -ENODEV;
}

int esp_sdr_espnow_send(const uint8_t mac[ESP_SDR_ESPNOW_ALEN], const void *data, size_t len,
			k_timeout_t timeout)
{
	esp_err_t err;

	if (!inited) {
		return -ENODEV;
	}
	if (len < 1 || len > ESP_SDR_ESPNOW_MAX_DATA) {
		return -EINVAL;
	}
	if (!K_TIMEOUT_EQ(timeout, K_NO_WAIT)) {
		k_sem_reset(&send_done);
	}
	err = esp_now_send(mac, data, len);
	if (err != ESP_OK) {
		atomic_inc(&tx_refused);
		return err_to_errno(err);
	}
	if (K_TIMEOUT_EQ(timeout, K_NO_WAIT)) {
		return 0;
	}
	/* The callback follows the air time, or the retries' end. */
	if (k_sem_take(&send_done, timeout) != 0) {
		return -ETIMEDOUT;
	}
	return tx_good ? 0 : -EIO;
}

void esp_sdr_espnow_rx_queue(struct k_msgq *q)
{
	rx_q = q;
}

void esp_sdr_espnow_rx_callback(esp_sdr_espnow_rx_cb cb, void *user)
{
	rx_user = user;
	rx_cb = cb;
}

/* esp_now.h: the window in ms awake per interval; esp_wifi.h: the interval in ms (1..65535). */
int esp_sdr_espnow_wake_window(uint16_t window_ms, uint16_t interval_ms)
{
	esp_err_t err;

	if (!inited) {
		return -ENODEV;
	}
	if (interval_ms < 1) {
		return -EINVAL;
	}
	err = esp_wifi_connectionless_module_set_wake_interval(interval_ms);
	if (err == ESP_OK) {
		err = esp_now_set_wake_window(window_ms);
	}
	return err_to_errno(err);
}

int esp_sdr_espnow_channel(uint8_t ch, uint8_t *now)
{
	uint8_t primary = 0;
	wifi_second_chan_t second;
	esp_err_t err = ESP_OK;

	if (ch < 1 || ch > 14) {
		return -EINVAL;
	}
	if (!inited) {
		return -ENODEV;
	}
	err = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
	if (esp_wifi_get_channel(&primary, &second) == ESP_OK && now != NULL) {
		*now = primary;
	}
	return err_to_errno(err);
}

void esp_sdr_espnow_stats(struct esp_sdr_espnow_stats *out, bool clear)
{
	out->rx = atomic_get(&rx_count);
	out->rx_bytes = atomic_get(&rx_bytes);
	out->rx_dropped = atomic_get(&rx_dropped);
	out->rssi = rx_rssi;
	out->tx_ok = atomic_get(&tx_ok);
	out->tx_fail = atomic_get(&tx_fail);
	out->tx_refused = atomic_get(&tx_refused);
	if (clear) {
		atomic_clear(&rx_count);
		atomic_clear(&rx_bytes);
		atomic_clear(&rx_dropped);
		atomic_clear(&tx_ok);
		atomic_clear(&tx_fail);
		atomic_clear(&tx_refused);
	}
}

/* The shell. */

static uint8_t peer_mac[ESP_SDR_ESPNOW_ALEN];
static bool have_peer;

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
	uint8_t mac[ESP_SDR_ESPNOW_ALEN] = {0};
	int ret;

	if (inited) {
		shell_print(sh, "already initialised");
		return 0;
	}
	ret = esp_sdr_espnow_init();
	if (ret != 0) {
		shell_error(sh, "espnow init: %d", ret);
		return ret;
	}
	(void)esp_sdr_espnow_mac(mac);
	shell_print(sh, "espnow up, own mac %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
		    mac[3], mac[4], mac[5]);
	return 0;
}

static int cmd_peer(const struct shell *sh, size_t argc, char **argv)
{
	unsigned int m[6];
	uint8_t mac[6];
	int ret;

	if (need_init(sh) != 0) {
		return -ENODEV;
	}
	if (sscanf(argv[1], "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
		shell_error(sh, "mac as aa:bb:cc:dd:ee:ff");
		return -EINVAL;
	}
	for (int i = 0; i < 6; i++) {
		mac[i] = m[i];
	}
	if (have_peer) {
		(void)esp_sdr_espnow_peer_del(peer_mac);
	}
	ret = esp_sdr_espnow_peer_add(mac, 0);
	if (ret != 0) {
		shell_error(sh, "add peer: %d", ret);
		return ret;
	}
	memcpy(peer_mac, mac, sizeof(peer_mac));
	have_peer = true;
	shell_print(sh, "peer set (channel: the current one)");
	return 0;
}

static int cmd_send(const struct shell *sh, size_t argc, char **argv)
{
	static uint8_t payload[ESP_SDR_ESPNOW_MAX_DATA];
	int count = atoi(argv[1]), interval = atoi(argv[2]);
	int bytes = argc > 3 ? atoi(argv[3]) : 16;
	int refused = 0;
	int64_t t0 = k_uptime_get();
	struct esp_sdr_espnow_stats before, after;

	if (need_init(sh) != 0) {
		return -ENODEV;
	}
	if (!have_peer || count < 1 || interval < 0 || bytes < 1 ||
	    bytes > ESP_SDR_ESPNOW_MAX_DATA) {
		shell_error(sh, "peer first; send <count> <interval_ms> [bytes 1..%d]",
			    ESP_SDR_ESPNOW_MAX_DATA);
		return -EINVAL;
	}
	esp_sdr_espnow_stats(&before, false);
	for (int i = 0; i < count; i++) {
		int64_t next = t0 + (int64_t)(i + 1) * interval;
		int64_t left;
		int ret;

		memset(payload, i, sizeof(payload));
		memcpy(payload, &i, sizeof(i));
		ret = esp_sdr_espnow_send(peer_mac, payload, bytes, K_MSEC(2000));
		if (ret == -ENOMEM || ret == -EINVAL || ret == -ENOENT || ret == -ENODEV) {
			refused++;
		}
		left = next - k_uptime_get();
		if (left > 0) {
			k_sleep(K_MSEC(left));
		}
	}
	esp_sdr_espnow_stats(&after, false);
	shell_print(sh, "sent %d: callback ok %u fail %u, refused %d, %lld ms", count,
		    after.tx_ok - before.tx_ok, after.tx_fail - before.tx_fail, refused,
		    k_uptime_get() - t0);
	return 0;
}

static int cmd_stats(const struct shell *sh, size_t argc, char **argv)
{
	struct esp_sdr_espnow_stats st;

	esp_sdr_espnow_stats(&st, argc > 1);
	shell_print(sh, "rx %u bytes %u dropped %u last rssi %d, tx ok %u fail %u refused %u",
		    st.rx, st.rx_bytes, st.rx_dropped, st.rssi, st.tx_ok, st.tx_fail,
		    st.tx_refused);
	if (argc > 1) {
		shell_print(sh, "cleared");
	}
	return 0;
}

static int cmd_window(const struct shell *sh, size_t argc, char **argv)
{
	int window = atoi(argv[1]), interval = atoi(argv[2]);
	int ret;

	if (need_init(sh) != 0) {
		return -ENODEV;
	}
	if (window < 0 || window > 65535 || interval < 1 || interval > 65535) {
		return -EINVAL;
	}
	ret = esp_sdr_espnow_wake_window(window, interval);
	shell_print(sh, "window %d ms every %d ms: %d", window, interval, ret);
	return ret;
}

static int cmd_rx(const struct shell *sh, size_t argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 4;
	uint32_t total = atomic_get(&rx_count);

	if (n < 1 || n > LAST_N) {
		return -EINVAL;
	}
	if ((uint32_t)n > total) {
		n = total;
	}
	for (int i = n; i > 0; i--) {
		const struct esp_sdr_espnow_msg *m = &last[(last_head - i) % LAST_N];

		shell_print(sh, "%02x:%02x:%02x:%02x:%02x:%02x rssi %d len %u first %02x %02x %02x %02x",
			    m->src[0], m->src[1], m->src[2], m->src[3], m->src[4], m->src[5],
			    m->rssi, m->len, m->data[0], m->data[1], m->data[2], m->data[3]);
	}
	return 0;
}

static int cmd_chan(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t now = 0;
	int ret;

	if (need_init(sh) != 0) {
		return -ENODEV;
	}
	ret = esp_sdr_espnow_channel(atoi(argv[1]), &now);
	shell_print(sh, "channel %s: %d, now %u", argv[1], ret, now);
	return ret;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	espnow_cmds,
	SHELL_CMD_ARG(init, NULL, "Set STA mode if needed, start ESP-NOW", cmd_init, 1, 0),
	SHELL_CMD_ARG(peer, NULL, "<mac> unicast peer, current channel", cmd_peer, 2, 0),
	SHELL_CMD_ARG(send, NULL, "<count> <interval_ms> [bytes]", cmd_send, 3, 1),
	SHELL_CMD_ARG(stats, NULL, "[clear] rx and tx counters", cmd_stats, 1, 1),
	SHELL_CMD_ARG(window, NULL, "<window_ms> <interval_ms> wake window", cmd_window, 3, 0),
	SHELL_CMD_ARG(chan, NULL, "<1..14> primary channel (not associated)", cmd_chan, 2, 0),
	SHELL_CMD_ARG(rx, NULL, "[n] the last frames received", cmd_rx, 1, 1),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(espnow, &espnow_cmds, "ESP-NOW", NULL);

/*
 * The inverse of esp_sdr_espnow_init(), called by the host before it unloads the module: the blob's
 * ESP-NOW state and the callbacks into this module's code. The wake window and the channel go with
 * the Wi-Fi module.
 */
int multiapp_module_down(void)
{
	if (inited) {
		(void)esp_now_unregister_recv_cb();
		(void)esp_now_unregister_send_cb();
		if (esp_now_deinit() != ESP_OK) {
			return -EIO;
		}
		inited = false;
	}
	rx_q = NULL;
	rx_cb = NULL;
	return 0;
}
