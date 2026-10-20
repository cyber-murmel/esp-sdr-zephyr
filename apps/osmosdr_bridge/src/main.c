/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The WHY2025 badge's ESP32-P4 as an osmosdr USB device (../osmosdr/src/usb_sdr.c, high speed
 * on its OTG HS port), its ESP32-C6 running apps/osmosdr_serial over SDIO as the radio: the
 * host's vendor requests go to the C6 as request frames and its replies come back, the C6's
 * receive blocks (already in the USB block format) go out on bulk IN as they are, the host's
 * transmit samples go to the C6 as transmit frames.
 *
 * The SDIO side is ../../common/src/esdr_sdio_host.c (the C6's SLC protocol on function 1, as
 * the MicroPython app uses it too): a reader thread takes the C6's stream and splits it into
 * frames and blocks; frames go out as one write each once the C6 has buffer tokens for them.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/usbd.h>

#include <sample_usbd.h>

#include "esdr_proto.h"
#include "esdr_sdio_host.h"
#include "usb_sdr.h"

LOG_MODULE_REGISTER(bridge, LOG_LEVEL_INF);

#define LINK_SDIO DT_CHOSEN(esdr_link_sdio)

#define SDIO_CHUNK      16384U
#define SDIO_READY_MS   5000

#define REPLY_MAX          512U
#define REQUEST_TIMEOUT_MS 1000
/* Bulk IN blocks in flight or waiting: 64 KB, 16 ms at 2 MS/s. */
#define POOL      16U
#define OUT_BUFS  2U
#define OUT_BYTES ESDR_BLOCK_BYTES

#define READER_STACK 2048
#define READER_PRIO  2

/* GPIO2: USB-C port J8 to the ESP32-C6 (high) or this chip (low). GPIO12: the C6's enable. */
#define USB_MUX_PIN 2
#define C6_EN_PIN   12

static const struct device *const sdhc = DEVICE_DT_GET(LINK_SDIO);

/* DMA buffers in internal RAM. */
static uint8_t chunk[SDIO_CHUNK] __aligned(64);
static uint8_t pool[POOL][ESDR_BLOCK_BYTES] __aligned(64);
static uint8_t outb[OUT_BUFS][OUT_BYTES] __aligned(64);
K_MSGQ_DEFINE(pool_free, sizeof(uint8_t), POOL, 1);

static struct {
	uint32_t blocks, dropped, resyncs, tx_frames;
} st;

/* ---- SDIO (../../common/src/esdr_sdio_host.c) ---- */

static int link_send(const void *a, size_t na, const void *b, size_t nb)
{
	return esdr_sdio_send(a, na, b, nb, REQUEST_TIMEOUT_MS);
}

static int link_open(void)
{
	return esdr_sdio_open(sdhc, SDIO_READY_MS);
}

/* ---- requests ---- */

static K_MUTEX_DEFINE(req_lock);
static K_SEM_DEFINE(reply_sem, 0, 1);
static struct {
	uint8_t req;
	void *buf;
	uint16_t max, len;
	int status;
} pend;

static int request(uint8_t req, uint16_t value, const void *data, uint16_t len, void *reply,
		   uint16_t reply_max)
{
	struct esdr_ser_hdr h = {
		.magic = sys_cpu_to_le32(ESDR_SER_MAGIC),
		.req = req,
		.flags = reply != NULL ? ESDR_SER_IN : 0U,
		.value = sys_cpu_to_le16(value),
		.len = sys_cpu_to_le16(reply != NULL ? reply_max : len),
	};
	int ret;

	k_mutex_lock(&req_lock, K_FOREVER);
	k_sem_reset(&reply_sem);
	pend.req = req;
	pend.buf = reply;
	pend.max = reply_max;
	pend.len = 0;
	pend.status = 0;
	ret = link_send(&h, sizeof(h), data, reply == NULL ? len : 0U);
	if (ret == 0) {
		if (k_sem_take(&reply_sem, K_MSEC(REQUEST_TIMEOUT_MS)) != 0) {
			ret = -ETIMEDOUT;
		} else {
			ret = pend.status < 0 ? pend.status : (int)pend.len;
		}
	}
	k_mutex_unlock(&req_lock);
	return ret;
}

/* ---- the C6's stream: frames and blocks ---- */

static struct {
	uint8_t hdr[sizeof(struct esdr_ser_hdr) + REPLY_MAX];
	/* Bytes of the frame or block in hand; the block's pool buffer (NULL: dropped). */
	uint32_t have, need;
	uint8_t *blk;
	bool is_blk;
} rx;

static void block_done(uint8_t *b)
{
	st.blocks++;
	if (b == NULL) {
		st.dropped++;
		return;
	}
	if (!usb_sdr_enabled() || usb_sdr_submit_in(b, ESDR_BLOCK_BYTES) != 0) {
		uint8_t i = (uint8_t)((b - pool[0]) / ESDR_BLOCK_BYTES);

		st.dropped++;
		(void)k_msgq_put(&pool_free, &i, K_NO_WAIT);
	}
}

static void frame_done(void)
{
	struct esdr_ser_hdr h;

	memcpy(&h, rx.hdr, sizeof(h));
	if (h.req != pend.req) {
		return;
	}
	pend.status = (int16_t)sys_le16_to_cpu(h.status);
	pend.len = MIN(sys_le16_to_cpu(h.len), pend.max);
	if (pend.buf != NULL) {
		memcpy(pend.buf, rx.hdr + sizeof(h), pend.len);
	}
	k_sem_give(&reply_sem);
}

static void parse(const uint8_t *p, uint32_t n)
{
	while (n > 0U) {
		if (rx.have < 4U) {
			uint8_t c = *p++;

			n--;
			if (rx.have == 0U && c == 0U) {
				/* Padding to whole words between frames. */
				continue;
			}
			rx.hdr[rx.have++] = c;
			if (rx.have == 4U) {
				uint32_t magic = sys_get_le32(rx.hdr);

				if (magic == ESDR_BLOCK_MAGIC) {
					uint8_t i;

					rx.is_blk = true;
					rx.need = ESDR_BLOCK_BYTES;
					rx.blk = k_msgq_get(&pool_free, &i, K_NO_WAIT) == 0 ? pool[i]
											    : NULL;
					if (rx.blk != NULL) {
						memcpy(rx.blk, rx.hdr, 4);
					}
				} else if (magic == ESDR_SER_MAGIC) {
					rx.is_blk = false;
					rx.need = sizeof(struct esdr_ser_hdr);
				} else {
					/* Not a magic: slide by a byte. */
					memmove(rx.hdr, rx.hdr + 1, 3);
					rx.have = 3;
					st.resyncs++;
				}
			}
			continue;
		}
		uint32_t k = MIN(n, rx.need - rx.have);

		if (rx.is_blk) {
			if (rx.blk != NULL) {
				memcpy(rx.blk + rx.have, p, k);
			}
		} else {
			memcpy(rx.hdr + rx.have, p, k);
		}
		rx.have += k;
		p += k;
		n -= k;
		if (rx.have < rx.need) {
			continue;
		}
		if (rx.is_blk) {
			block_done(rx.blk);
			rx.have = 0;
			continue;
		}
		if (rx.need == sizeof(struct esdr_ser_hdr)) {
			/* A reply's data, if any, follows its header. */
			uint32_t len = sys_le16_to_cpu(((struct esdr_ser_hdr *)rx.hdr)->len);

			if (len > 0U && len <= REPLY_MAX) {
				rx.need += len;
				continue;
			}
		}
		frame_done();
		rx.have = 0;
	}
}

static void reader_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		int n = esdr_sdio_read(chunk, SDIO_CHUNK);

		if (n <= 0) {
			k_sleep(K_TICKS(1));
			continue;
		}
		parse(chunk, (uint32_t)n);
	}
}
K_THREAD_STACK_DEFINE(reader_stack, READER_STACK);
static struct k_thread reader_thread;

/* ---- USB ---- */

int usb_sdr_control_out(uint8_t req, uint16_t value, const uint8_t *data, size_t len)
{
	int ret = request(req, value, data, (uint16_t)len, NULL, 0);

	return ret < 0 ? ret : 0;
}

int usb_sdr_control_in(uint8_t req, uint16_t value, uint8_t *buf, size_t max)
{
	return request(req, value, NULL, 0, buf, (uint16_t)MIN(max, REPLY_MAX));
}

void usb_sdr_in_done(uint8_t *data, size_t len, int err)
{
	uint8_t i = (uint8_t)((data - pool[0]) / ESDR_BLOCK_BYTES);

	ARG_UNUSED(len);
	ARG_UNUSED(err);
	(void)k_msgq_put(&pool_free, &i, K_NO_WAIT);
}

void usb_sdr_out_done(uint8_t *data, size_t len, int err)
{
	if (err == 0 && len > 0U) {
		struct esdr_ser_hdr h = {
			.magic = sys_cpu_to_le32(ESDR_SER_MAGIC),
			.req = ESDR_SER_TX_DATA,
			.len = sys_cpu_to_le16((uint16_t)len),
		};

		if (link_send(&h, sizeof(h), data, len) == 0) {
			st.tx_frames++;
		}
	}
	if (err != -ECONNABORTED && usb_sdr_enabled()) {
		(void)usb_sdr_submit_out(data, OUT_BYTES);
	}
}

static void link_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	if (usb_sdr_enabled()) {
		for (uint32_t k = 0; k < OUT_BUFS; k++) {
			(void)usb_sdr_submit_out(outb[k], OUT_BYTES);
		}
	} else {
		/* The host went: the radio stops. */
		(void)request(ESDR_REQ_SET_MODE, ESDR_MODE_OFF, NULL, 0, NULL, 0);
	}
}
static K_WORK_DEFINE(link_work, link_work_fn);

void usb_sdr_link(bool up)
{
	ARG_UNUSED(up);
	k_work_submit(&link_work);
}

static void msg_cb(struct usbd_context *const ctx, const struct usbd_msg *const msg)
{
	if (usbd_can_detect_vbus(ctx)) {
		if (msg->type == USBD_MSG_VBUS_READY) {
			(void)usbd_enable(ctx);
		}
		if (msg->type == USBD_MSG_VBUS_REMOVED) {
			(void)usbd_disable(ctx);
		}
	}
}

int main(void)
{
	const struct device *gpio = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	struct usbd_context *ctx;
	struct esdr_info info;
	int ret;

	/* J8 to this chip's OTG HS port; the C6 out of reset. */
	(void)gpio_pin_configure(gpio, USB_MUX_PIN, GPIO_OUTPUT_LOW);
	(void)gpio_pin_configure(gpio, C6_EN_PIN, GPIO_OUTPUT_HIGH);

	for (uint8_t i = 0; i < POOL; i++) {
		(void)k_msgq_put(&pool_free, &i, K_NO_WAIT);
	}
	ret = link_open();
	if (ret != 0) {
		LOG_ERR("SDIO to the C6: %d", ret);
		return 0;
	}
	k_thread_create(&reader_thread, reader_stack, K_THREAD_STACK_SIZEOF(reader_stack),
			reader_fn, NULL, NULL, NULL, READER_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&reader_thread, "sdio_reader");

	ret = request(ESDR_REQ_GET_INFO, 0, NULL, 0, &info, sizeof(info));
	if (ret < 0 || sys_le32_to_cpu(info.magic) != ESDR_INFO_MAGIC) {
		LOG_ERR("C6 info: %d", ret);
	} else {
		LOG_INF("radio: %s, receive up to %u S/s", info.firmware,
			sys_le32_to_cpu(info.rx_rate_max_hz));
	}

	ctx = sample_usbd_init_device(msg_cb);
	if (ctx == NULL) {
		LOG_ERR("USB device init failed");
		return 0;
	}
	if (!usbd_can_detect_vbus(ctx)) {
		ret = usbd_enable(ctx);
		if (ret != 0) {
			LOG_ERR("USB enable: %d", ret);
			return 0;
		}
	}
	LOG_INF("USB up");
	for (;;) {
		k_sleep(K_SECONDS(10));
		LOG_INF("blocks %u, dropped %u, resyncs %u, tx frames %u", st.blocks, st.dropped,
			st.resyncs, st.tx_frames);
	}
	return 0;
}
