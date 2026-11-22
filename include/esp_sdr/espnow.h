/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ESP-NOW from a multiapp boot: the espnow module (apps/multiapp/ext/espnow.c) over the Wi-Fi
 * module's driver, without joining an access point. Not for threads of the Wi-Fi task: the
 * callbacks of the blob run there, so a receive callback must not block.
 */

#ifndef ESP_SDR_ESPNOW_H_
#define ESP_SDR_ESPNOW_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#define ESP_SDR_ESPNOW_ALEN     6
#define ESP_SDR_ESPNOW_MAX_DATA 250

/* One received frame, as a k_msgq item (esp_sdr_espnow_rx_queue()). */
struct esp_sdr_espnow_msg {
	uint8_t src[ESP_SDR_ESPNOW_ALEN];
	int8_t rssi;
	uint8_t len;
	uint8_t data[ESP_SDR_ESPNOW_MAX_DATA];
};

struct esp_sdr_espnow_stats {
	uint32_t rx;         /* frames received */
	uint32_t rx_bytes;
	uint32_t rx_dropped; /* queue full */
	int32_t rssi;        /* of the last one */
	uint32_t tx_ok;      /* send callback: delivered (acknowledged, or broadcast aired) */
	uint32_t tx_fail;    /* send callback: no acknowledgement */
	uint32_t tx_refused; /* esp_now_send() itself failed */
};

typedef void (*esp_sdr_espnow_rx_cb)(const struct esp_sdr_espnow_msg *msg, void *user);

/* Station mode if the driver left none, esp_now_init(), the callbacks. 0 or -errno. Idempotent. */
int esp_sdr_espnow_init(void);

/* This station's MAC. */
int esp_sdr_espnow_mac(uint8_t mac[ESP_SDR_ESPNOW_ALEN]);

/* A plain (unencrypted) unicast peer on the current channel (0) or on <channel>. Adding one that
 * exists is not an error. */
int esp_sdr_espnow_peer_add(const uint8_t mac[ESP_SDR_ESPNOW_ALEN], uint8_t channel);
int esp_sdr_espnow_peer_del(const uint8_t mac[ESP_SDR_ESPNOW_ALEN]);

/* Send <len> bytes. With K_NO_WAIT it returns once the frame is queued; otherwise it waits for the
 * send callback (the air time and retries) and returns -EIO on a failed delivery, -ETIMEDOUT when
 * none comes. One waiting sender at a time. */
int esp_sdr_espnow_send(const uint8_t mac[ESP_SDR_ESPNOW_ALEN], const void *data, size_t len,
			k_timeout_t timeout);

/* Frames go to <q> (items of struct esp_sdr_espnow_msg), and to <cb> when set. Either may be NULL.
 * Both run in the Wi-Fi task. A full queue drops the frame and counts it. */
void esp_sdr_espnow_rx_queue(struct k_msgq *q);
void esp_sdr_espnow_rx_callback(esp_sdr_espnow_rx_cb cb, void *user);

/* Radio awake for <window_ms> of every <interval_ms> (1..65535) for receiving; 0 for the window
 * keeps the driver's default. Needed while Wi-Fi sleeps (coex). */
int esp_sdr_espnow_wake_window(uint16_t window_ms, uint16_t interval_ms);

/* Primary channel 1..14, for both ends to share one without an access point. Only valid while not
 * associated. Returns 0 or -errno; *now (if not NULL) is the channel read back. */
int esp_sdr_espnow_channel(uint8_t ch, uint8_t *now);

void esp_sdr_espnow_stats(struct esp_sdr_espnow_stats *out, bool clear);

#endif
