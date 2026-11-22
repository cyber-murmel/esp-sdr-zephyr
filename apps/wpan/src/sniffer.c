/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Adapted from Zephyr samples/net/ieee802154/sniffer (cf45cd71868) for apps/wpan.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>

#include "sniffer.h"
#include "sniffer_line.h"
#include "wpan.h"

#if defined(CONFIG_ESP_SDR)
/* Internal RAM below the dump banks is tight on the S3: the categories' regions. */
#include <esp_sdr/esp_sdr_place.h>
#else
#define ESP_SDR_STACK_DEFINE(nm, size)                    static K_KERNEL_STACK_DEFINE(nm, size)
#define ESP_SDR_BUF_DEFINE(cat, type, nm, dims, align, f) static type nm dims __aligned(align)
#endif

LOG_MODULE_REGISTER(wpan_sniffer, LOG_LEVEL_INF);

#define PSDU_MAX      127U
#define CHAN_MIN      11U
#define CHAN_MAX      26U
/* Peak 1360 bytes on the S3 (Xtensa windows), 816 on the C6, at 200 frames/s. */
#define EMIT_STACK    1792

struct sniffer_msg {
	uint64_t time_us;
	int16_t rssi;
	uint8_t len;
	uint8_t lqi;
	uint8_t psdu[PSDU_MAX];
};

ESP_SDR_BUF_DEFINE(BULK, uint8_t, msgq_buf, [CONFIG_WPAN_SNIFFER_QUEUE * sizeof(struct sniffer_msg)],
		   4, NONE);
static struct k_msgq msgq;
ESP_SDR_STACK_DEFINE(emit_stack, EMIT_STACK);
static struct k_thread emit_thread;

/* The shell that ran `sniffer start`: the lines go out through it, so they never interleave
 * with command replies and the transport is whatever the shell sits on (CDC-ACM, USB-JTAG).
 */
static const struct shell *sniffer_sh;
static K_MUTEX_DEFINE(state_lock);
static atomic_t capturing;
/* What the extcap assumes until told otherwise, so `speed` has an answer before any set. */
static uint32_t console_speed = 115200;

static atomic_t st_enq, st_drop, st_tx_rec, st_tx_bytes, st_hwm;

static char line[SNIFFER_LINE_MAX];

bool sniffer_capturing(void)
{
	return atomic_get(&capturing) != 0;
}

void sniffer_frame(const uint8_t *psdu, size_t len, uint8_t lqi, int16_t rssi, uint64_t time_us)
{
	struct sniffer_msg msg;
	atomic_val_t used;

	if (!sniffer_capturing()) {
		return;
	}
	if (len > PSDU_MAX) {
		len = PSDU_MAX;
	}
	msg.time_us = time_us;
	msg.rssi = rssi;
	msg.len = (uint8_t)len;
	msg.lqi = lqi;
	memcpy(msg.psdu, psdu, len);

	if (k_msgq_put(&msgq, &msg, K_NO_WAIT) != 0) {
		atomic_inc(&st_drop);
		return;
	}
	atomic_inc(&st_enq);
	used = (atomic_val_t)k_msgq_num_used_get(&msgq);
	/* High-water mark: raise it only, retrying when another thread raised it meanwhile. */
	for (;;) {
		atomic_val_t hwm = atomic_get(&st_hwm);

		if (used <= hwm || atomic_cas(&st_hwm, hwm, used)) {
			break;
		}
	}
}

static void emit_fn(void *a, void *b, void *c)
{
	struct sniffer_msg msg;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		const struct shell *sh;
		size_t n;

		(void)k_msgq_get(&msgq, &msg, K_FOREVER);
		sh = sniffer_sh;
		if (sh == NULL || !sniffer_capturing()) {
			/* Stopped meanwhile: the queue drains without output. */
			continue;
		}
		n = sniffer_line(line, sizeof(line), msg.psdu, msg.len, msg.lqi, msg.rssi,
				 msg.time_us);
		shell_fprintf_normal(sh, "%s", line);
		atomic_inc(&st_tx_rec);
		atomic_add(&st_tx_bytes, (atomic_val_t)n);
	}
}

void sniffer_init(void)
{
	k_msgq_init(&msgq, msgq_buf, sizeof(struct sniffer_msg), CONFIG_WPAN_SNIFFER_QUEUE);
	k_thread_create(&emit_thread, emit_stack, K_KERNEL_STACK_SIZEOF(emit_stack), emit_fn, NULL,
			NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&emit_thread, "wpan_sniffer");
}

/* ------------------------------------------------------------------ shell */
/* Replies as the upstream sample's shell.c, which its Wireshark extcap matches on. */

static int cmd_start(const struct shell *sh, size_t argc, char **argv)
{
	int err = 0;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	k_mutex_lock(&state_lock, K_FOREVER);
	if (sniffer_capturing()) {
		k_mutex_unlock(&state_lock);
		shell_print(sh, "already started");
		return 0;
	}
	err = wpan_set_promisc(true);
	if (err != 0) {
		LOG_WRN("promiscuous mode failed (%d)", err);
	}
	if (!wpan_radio_is_on()) {
		err = wpan_radio_on();
		if (err != 0) {
			k_mutex_unlock(&state_lock);
			shell_error(sh, "start failed (%d)", err);
			return err;
		}
	}
	sniffer_sh = sh;
	atomic_set(&capturing, 1);
	k_mutex_unlock(&state_lock);
	shell_print(sh, "started");
	return 0;
}

static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	int err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	k_mutex_lock(&state_lock, K_FOREVER);
	if (!sniffer_capturing()) {
		k_mutex_unlock(&state_lock);
		shell_print(sh, "already stopped");
		return 0;
	}
	atomic_set(&capturing, 0);
	err = wpan_radio_off();
	k_mutex_unlock(&state_lock);
	if (err != 0) {
		shell_error(sh, "stop failed (%d)", err);
		return err;
	}
	shell_print(sh, "stopped");
	return 0;
}

static int cmd_channel(const struct shell *sh, size_t argc, char **argv)
{
	char *endp = NULL;
	long ch;
	int err;

	if (argc == 1) {
		shell_print(sh, "%u", (unsigned int)wpan_get_channel());
		return 0;
	}
	ch = strtol(argv[1], &endp, 0);
	if (endp == NULL || *endp != '\0') {
		shell_error(sh, "invalid channel: %s", argv[1]);
		return -EINVAL;
	}
	if (ch < (long)CHAN_MIN || ch > (long)CHAN_MAX) {
		shell_error(sh, "channel out of range: %ld", ch);
		return -ERANGE;
	}
	k_mutex_lock(&state_lock, K_FOREVER);
	err = sniffer_capturing() ? -EBUSY : wpan_set_channel((uint16_t)ch);
	k_mutex_unlock(&state_lock);
	if (err == -EBUSY) {
		shell_error(sh, "cannot change channel while running; run 'sniffer stop' first");
		return err;
	}
	if (err != 0) {
		shell_error(sh, "set channel failed (%d)", err);
		return err;
	}
	shell_print(sh, "channel %u", (unsigned int)wpan_get_channel());
	return 0;
}

/* Both transports are USB: there is no rate to set. The command only keeps the extcap's
 * handshake (`speed <baud>` echoed) working.
 */
static int cmd_speed(const struct shell *sh, size_t argc, char **argv)
{
	char *endp = NULL;
	unsigned long baud;

	if (argc == 1) {
		shell_print(sh, "speed %u", (unsigned int)console_speed);
		return 0;
	}
	baud = strtoul(argv[1], &endp, 0);
	if (endp == NULL || *endp != '\0') {
		shell_error(sh, "invalid baud rate: %s", argv[1]);
		return -EINVAL;
	}
	if (baud == 0UL || baud > UINT32_MAX) {
		shell_error(sh, "baud rate out of range: %s", argv[1]);
		return -ERANGE;
	}
	console_speed = (uint32_t)baud;
	shell_print(sh, "speed %lu", baud);
	return 0;
}

static int cmd_stats(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "enq=%u drop=%u tx_rec=%u tx_bytes=%u q_hwm=%u q_used=%u/%u",
		    (unsigned int)atomic_get(&st_enq), (unsigned int)atomic_get(&st_drop),
		    (unsigned int)atomic_get(&st_tx_rec), (unsigned int)atomic_get(&st_tx_bytes),
		    (unsigned int)atomic_get(&st_hwm), (unsigned int)k_msgq_num_used_get(&msgq),
		    (unsigned int)CONFIG_WPAN_SNIFFER_QUEUE);
	return 0;
}

static int cmd_stats_reset(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	atomic_set(&st_enq, 0);
	atomic_set(&st_drop, 0);
	atomic_set(&st_tx_rec, 0);
	atomic_set(&st_tx_bytes, 0);
	atomic_set(&st_hwm, 0);
	shell_print(sh, "stats reset");
	return 0;
}

/* clang-format off */
SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_sniffer,
	SHELL_CMD(start, NULL, "Start sniffing (enable RX).", cmd_start),
	SHELL_CMD(stop, NULL, "Stop sniffing (disable RX).", cmd_stop),
	SHELL_CMD(channel, NULL, "Get/set channel: channel [11..26].", cmd_channel),
	SHELL_CMD(speed, NULL, "Get/set console baud rate: speed [baud] (no-op, USB).", cmd_speed),
	SHELL_CMD(stats, NULL, "Show stream queue and traffic stats.", cmd_stats),
	SHELL_CMD(stats_reset, NULL, "Reset stream stats counters.", cmd_stats_reset),
	SHELL_SUBCMD_SET_END
);
/* clang-format on */

SHELL_CMD_REGISTER(sniffer, &sub_sniffer, "IEEE 802.15.4 sniffer control.", NULL);
