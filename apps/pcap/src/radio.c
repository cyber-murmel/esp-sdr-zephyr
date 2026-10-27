/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The C6 over SDIO (radio.h): ../../common/src/esdr_sdio_host.c and esdr_link_host.c, a reader
 * thread splitting the radio's stream into reply frames and blocks, a pool of block buffers in
 * internal RAM; after ../../osmosdr_bridge/src/main.c.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "app_usb.h"
#include "esdr_link_host.h"
#include "esdr_sdio_host.h"
#include "radio.h"

LOG_MODULE_REGISTER(radio, LOG_LEVEL_INF);

#define LINK_SDIO DT_CHOSEN(esdr_link_sdio)
/* zephyr,user: the C6's enable and, on the WHY2025 badge, USB-C port J8's multiplexer. */
#define USER_NODE DT_PATH(zephyr_user)

#define SDIO_CHUNK    16384U
#define SDIO_READY_MS 5000
/* Block buffers: 48 KB, 12 ms of the gapless stream's 4 MB/s. */
#define POOL          12U
#define READER_STACK  2048
#define READER_PRIO   2

static const struct device *const sdhc = DEVICE_DT_GET(LINK_SDIO);

/* DMA buffers in internal RAM. */
static uint8_t chunk[SDIO_CHUNK] __aligned(64);
static uint8_t pool[POOL][ESDR_BLOCK_BYTES] __aligned(64);
K_MSGQ_DEFINE(pool_free, sizeof(uint8_t), POOL, 1);

static radio_block_fn sink;
static struct radio_stats st;

static uint8_t slot(const uint8_t *b)
{
	return (uint8_t)((b - pool[0]) / ESDR_BLOCK_BYTES);
}

void radio_block_free(uint8_t *blk)
{
	uint8_t i = slot(blk);

	(void)k_msgq_put(&pool_free, &i, K_NO_WAIT);
}

void radio_set_sink(radio_block_fn fn)
{
	sink = fn;
}

static int link_send(const void *a, size_t na, const void *b, size_t nb)
{
	return esdr_sdio_send(a, na, b, nb, ESDR_LINK_TIMEOUT_MS);
}

static uint8_t *get_block(void *user)
{
	uint8_t i;

	ARG_UNUSED(user);
	return k_msgq_get(&pool_free, &i, K_NO_WAIT) == 0 ? pool[i] : NULL;
}

static void block_done(uint8_t *b, void *user)
{
	radio_block_fn fn = sink;

	ARG_UNUSED(user);
	st.blocks++;
	if (b == NULL) {
		st.dropped++;
		return;
	}
	if (fn == NULL) {
		radio_block_free(b);
		return;
	}
	fn(b);
}

static struct esdr_link link = {
	.send = link_send,
	.get_block = get_block,
	.on_block = block_done,
};

int radio_request(uint8_t req, uint16_t value, const void *data, uint16_t len, void *reply,
		  uint16_t reply_max)
{
	return esdr_link_request(&link, req, value, data, len, reply, reply_max);
}

void radio_get_stats(struct radio_stats *out)
{
	*out = st;
	out->resyncs = link.resyncs;
}

/* The radio's stream: frames and blocks. */
static void reader_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		int n = esdr_sdio_read(chunk, SDIO_CHUNK);

		if (n < 0) {
			/* A new session (esdr_sdio_host.h): the frame or block in hand never ends. */
			uint8_t *blk = esdr_link_abort(&link);

			if (blk != NULL) {
				radio_block_free(blk);
			}
		}
		if (n <= 0) {
			k_sleep(K_TICKS(1));
			continue;
		}
		esdr_link_feed(&link, chunk, (uint32_t)n);
	}
}
K_THREAD_STACK_DEFINE(reader_stack, READER_STACK);
static struct k_thread reader_thread;

/* ---- USB-C port J8 to the C6 (ESDR_REQ_USB_TO_C6) ---- */

#if DT_NODE_HAS_PROP(USER_NODE, usb_mux_gpios)
static const struct gpio_dt_spec mux = GPIO_DT_SPEC_GET(USER_NODE, usb_mux_gpios);
static uint32_t away_s;

static void back_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)gpio_pin_set_dt(&mux, 0);
	app_usb_attach();
	LOG_INF("J8 back from the C6");
}
static K_WORK_DELAYABLE_DEFINE(back_work, back_fn);

static void away_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	LOG_INF("J8 to the C6 for %u s", away_s);
	(void)gpio_pin_set_dt(&mux, 1);
	(void)k_work_reschedule(&back_work, K_SECONDS(away_s));
}
static K_WORK_DELAYABLE_DEFINE(away_work, away_fn);
#endif

int radio_usb_to_c6(uint32_t seconds)
{
#if DT_NODE_HAS_PROP(USER_NODE, usb_mux_gpios)
	away_s = seconds != 0U ? seconds : 30U;
	/* The host sees the device leave before the port switches. */
	app_usb_detach();
	(void)k_work_reschedule(&away_work, K_MSEC(200));
	return 0;
#else
	ARG_UNUSED(seconds);
	return -ENOTSUP;
#endif
}

int radio_init(void)
{
	struct esdr_info info;
	int ret;

#if DT_NODE_HAS_PROP(USER_NODE, usb_mux_gpios)
	/* The port to this chip's OTG HS controller. */
	(void)gpio_pin_configure_dt(&mux, GPIO_OUTPUT_INACTIVE);
#endif
#if DT_NODE_HAS_PROP(USER_NODE, c6_enable_gpios)
	/* A reset, so that the C6 runs its image whatever state it was left in. */
	const struct gpio_dt_spec c6_en = GPIO_DT_SPEC_GET(USER_NODE, c6_enable_gpios);

	(void)gpio_pin_configure_dt(&c6_en, GPIO_OUTPUT_INACTIVE);
	k_msleep(10);
	(void)gpio_pin_set_dt(&c6_en, 1);
#endif
	for (uint8_t i = 0; i < POOL; i++) {
		(void)k_msgq_put(&pool_free, &i, K_NO_WAIT);
	}
	esdr_link_init(&link);
	ret = esdr_sdio_open(sdhc, SDIO_READY_MS);
	/* The reader enumerates the C6 again whenever it answers nothing (a reset, a new image). */
	k_thread_create(&reader_thread, reader_stack, K_THREAD_STACK_SIZEOF(reader_stack),
			reader_fn, NULL, NULL, NULL, READER_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&reader_thread, "sdio_reader");
	if (ret != 0) {
		LOG_ERR("SDIO to the C6: %d", ret);
		return ret;
	}
	ret = radio_request(ESDR_REQ_GET_INFO, 0, NULL, 0, &info, sizeof(info));
	if (ret < 0 || sys_le32_to_cpu(info.magic) != ESDR_INFO_MAGIC) {
		LOG_ERR("C6 info: %d", ret);
		return ret < 0 ? ret : -EIO;
	}
	LOG_INF("radio: %s, receive up to %u S/s", info.firmware,
		sys_le32_to_cpu(info.rx_rate_max_hz));
	return 0;
}
