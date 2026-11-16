/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The pcapng stream (output.h): blocks built by pcapng.c into a small staging buffer, copied
 * into transfer buffers of internal RAM (the OTG controller's DMA reads them), which go to the
 * vendor interface's bulk IN when full or after OUTPUT_FLUSH_MS. Producers: the decoder thread
 * (packets, polling) and the control thread (sections, the end); a mutex keeps them apart.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/app_version.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/version.h>

#include "app.h"
#include "output.h"
#include "pcapng.h"
#include "usb_pcap.h"

LOG_MODULE_REGISTER(output, LOG_LEVEL_INF);

#define OUT_BUFS  6U
#define OUT_BYTES 4096U
/* The largest block: an EPB of a burst record with a comment, with room to spare. */
#define BLOCK_MAX 1024U
#define ISB_MS    1000

static uint8_t outb[OUT_BUFS][OUT_BYTES] __aligned(64);
K_MSGQ_DEFINE(out_free, sizeof(uint8_t), OUT_BUFS, 1);
static K_MUTEX_DEFINE(out_lock);

static struct {
	/* The buffer being filled (-1: none), its fill, when its first byte came. */
	int cur;
	uint32_t fill;
	int64_t t_first;
	bool started;
	int64_t t_isb;
	uint64_t start_ns;
	struct output_stats st;
	/* Buffers out on the bus: taken back by the USB thread, on the other core. */
	atomic_t busy;
	uint8_t block[BLOCK_MAX] __aligned(4);
} out = {.cur = -1};

static void submit(void)
{
	uint32_t busy;
	uint8_t i;

	if (out.cur < 0 || out.fill == 0U) {
		return;
	}
	i = (uint8_t)out.cur;
	out.cur = -1;
	/* Counted before the transfer can finish (output_in_done()). */
	busy = (uint32_t)atomic_inc(&out.busy) + 1U;
	if (usb_pcap_submit(outb[i], out.fill) != 0) {
		/* No host: what the buffer held is lost (counted where the packets were). */
		(void)atomic_dec(&out.busy);
		(void)k_msgq_put(&out_free, &i, K_NO_WAIT);
		return;
	}
	/* Counted here: a finished IN transfer's buffer comes back emptied. */
	out.st.usb_bytes += out.fill;
	out.st.peak = MAX(out.st.peak, busy);
}

/* Bytes the buffers can still take, the one being filled and the free ones. */
static uint32_t room(void)
{
	uint32_t r = k_msgq_num_used_get(&out_free) * OUT_BYTES;

	if (out.cur >= 0) {
		r += OUT_BYTES - out.fill;
	}
	return r;
}

/* A block of @p len bytes into the buffers, spilling into the next ones. */
static int put(const uint8_t *p, uint32_t len)
{
	if (!usb_pcap_enabled() || room() < len) {
		return -ENOSPC;
	}
	while (len > 0U) {
		uint32_t n;

		if (out.cur < 0) {
			uint8_t i;

			if (k_msgq_get(&out_free, &i, K_NO_WAIT) != 0) {
				return -ENOSPC;
			}
			out.cur = i;
			out.fill = 0;
			out.t_first = k_uptime_get();
		}
		n = MIN(len, OUT_BYTES - out.fill);
		memcpy(outb[out.cur] + out.fill, p, n);
		out.fill += n;
		p += n;
		len -= n;
		if (out.fill == OUT_BYTES) {
			submit();
		}
	}
	return 0;
}

static const struct pcapng_idb idbs[PCAP_IF_COUNT] = {
	[PCAP_IF_BLE] = {.linktype = PCAP_LINKTYPE_BLE_LL_WITH_PHDR, .name = "esdr-ble",
			 .description = "Bluetooth LE link layer, decoded by the ESP32-P4",
			 .tsresol = 9},
	[PCAP_IF_WPAN] = {.linktype = PCAP_LINKTYPE_IEEE802_15_4_TAP, .name = "esdr-wpan",
			  .description = "IEEE 802.15.4 O-QPSK 2.4 GHz frames, decoded by the ESP32-P4",
			  .tsresol = 9},
	[PCAP_IF_BURST] = {.linktype = PCAP_LINKTYPE_USER0, .name = "esdr-burst",
			   .description = "Bursts no decoder claimed, with their metadata "
					  "(tools/esdr_burst.lua)",
			   .tsresol = 9},
};

static void section_locked(void)
{
	int n = pcapng_shb(out.block, sizeof(out.block), CONFIG_BOARD, "Zephyr " KERNEL_VERSION_STRING,
			   "esp-sdr pcap " APP_VERSION_STRING);

	if (n > 0) {
		(void)put(out.block, (uint32_t)n);
	}
	for (unsigned int k = 0; k < PCAP_IF_COUNT; k++) {
		n = pcapng_idb(out.block, sizeof(out.block), &idbs[k]);
		if (n > 0) {
			(void)put(out.block, (uint32_t)n);
		}
	}
}

void output_start(void)
{
	k_mutex_lock(&out_lock, K_FOREVER);
	memset(&out.st, 0, sizeof(out.st));
	out.started = true;
	out.start_ns = timebase_now_ns();
	out.t_isb = k_uptime_get();
	section_locked();
	k_mutex_unlock(&out_lock);
}

void output_section(void)
{
	k_mutex_lock(&out_lock, K_FOREVER);
	if (out.started) {
		section_locked();
	}
	k_mutex_unlock(&out_lock);
}

int output_packet(uint32_t iface, uint64_t t_ns, const void *data, uint32_t len,
		  const char *comment)
{
	struct pcapng_epb e = {
		.interface = iface,
		.timestamp = t_ns,
		.data = data,
		.len = len,
		.comment = comment,
		.flags = PCAPNG_EPB_FLAG_INBOUND,
		.dropcount = UINT64_MAX,
	};
	int n, ret = -ENOSPC;

	if (iface >= PCAP_IF_COUNT) {
		return -EINVAL;
	}
	k_mutex_lock(&out_lock, K_FOREVER);
	n = out.started ? pcapng_epb(out.block, sizeof(out.block), &e) : -EAGAIN;
	if (n > 0) {
		ret = put(out.block, (uint32_t)n);
	}
	if (ret == 0) {
		out.st.packets[iface]++;
	} else {
		out.st.dropped[iface]++;
	}
	k_mutex_unlock(&out_lock);
	return ret;
}

static void isb_locked(void)
{
	uint64_t now = timebase_now_ns();

	for (unsigned int k = 0; k < PCAP_IF_COUNT; k++) {
		const struct pcapng_isb s = {
			.interface = k,
			.timestamp = now,
			.starttime = out.start_ns,
			.ifrecv = out.st.packets[k] + out.st.dropped[k],
			.ifdrop = out.st.dropped[k],
		};
		int n = pcapng_isb(out.block, sizeof(out.block), &s);

		if (n > 0) {
			(void)put(out.block, (uint32_t)n);
		}
	}
}

void output_poll(void)
{
	int64_t now = k_uptime_get();

	k_mutex_lock(&out_lock, K_FOREVER);
	if (out.started && now - out.t_isb >= ISB_MS) {
		out.t_isb = now;
		isb_locked();
	}
	if (out.cur >= 0 && now - out.t_first >= OUTPUT_FLUSH_MS) {
		submit();
	}
	k_mutex_unlock(&out_lock);
}

void output_stop(void)
{
	k_mutex_lock(&out_lock, K_FOREVER);
	if (out.started) {
		isb_locked();
		submit();
		out.started = false;
	}
	k_mutex_unlock(&out_lock);
}

void output_in_done(uint8_t *data, size_t len, int err)
{
	uint8_t i = (uint8_t)((data - outb[0]) / OUT_BYTES);

	ARG_UNUSED(len);
	ARG_UNUSED(err);
	(void)atomic_dec(&out.busy);
	(void)k_msgq_put(&out_free, &i, K_NO_WAIT);
}

void output_get_stats(struct output_stats *st)
{
	k_mutex_lock(&out_lock, K_FOREVER);
	*st = out.st;
	k_mutex_unlock(&out_lock);
}

static int output_init(void)
{
	for (uint8_t i = 0; i < OUT_BUFS; i++) {
		(void)k_msgq_put(&out_free, &i, K_NO_WAIT);
	}
	return 0;
}
SYS_INIT(output_init, APPLICATION, 0);
