/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The ESP32-C6 radio over SDIO (esdr_sdio_host.h).
 */

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/sdhc.h>
#include <zephyr/kernel.h>
#include <zephyr/sd/sd.h>
#include <zephyr/sd/sd_spec.h>
#include <zephyr/sd/sdio.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "esdr_proto.h"
#include "esdr_sdio_host.h"

/*
 * The C6's host-visible SLC registers on function 1: the token count of its loaded receive
 * buffers (bits 27:16) and the running count of bytes it offers (PKT_LEN, 20 bits). Data moves
 * through a window that ends at SLC_END_ADDR: N bytes start at SLC_END_ADDR - N.
 */
#define SLC_TOKEN_RDATA 0x044
#define SLC_PKT_LEN     0x060
#define SLC_END_ADDR    0x1F800
/* Host to radio general-purpose interrupts (bit 0: this host (re)starts). */
#define SLC_HOST_INT    0x08C
/* SDIO blocks: larger ones gain nothing and corrupt data on the C6. */
#define SLC_BLOCK       512U
/* The radio's receive buffers (apps/osmosdr_serial/src/link_sdio.c RX_BUF_BYTES). */
#define SLC_RX_BUF      512U
/* After a bus error, enumeration is tried again at most this often. */
#define RECOVER_MS      100

static const struct device *sdhc;
static struct sd_card card;
static struct sdio_func f1;
static K_MUTEX_DEFINE(bus_lock);
static K_MUTEX_DEFINE(send_lock);
static uint32_t rx_got, tx_bufs;
static int64_t next_recover;
/* A frame being written: DMA from internal RAM. */
static uint8_t wbuf[ROUND_UP(sizeof(struct esdr_ser_hdr) + ESDR_BLOCK_BYTES, 4)] __aligned(64);

/* N bytes (a multiple of 4) through the window: whole blocks, then the rest in byte mode. */
static int slc_xfer(bool write, uint8_t *p, uint32_t n)
{
	while (n > 0U) {
		uint32_t k = n >= SLC_BLOCK ? n / SLC_BLOCK * SLC_BLOCK : n;
		int ret = write ? sdio_write_addr(&f1, SLC_END_ADDR - n, p, k)
				: sdio_read_addr(&f1, SLC_END_ADDR - n, p, k);

		if (ret != 0) {
			return ret;
		}
		p += k;
		n -= k;
	}
	return 0;
}

static int slc_reg(uint32_t reg, uint32_t *v)
{
	uint8_t le[4];
	int ret = sdio_read_addr(&f1, reg, le, sizeof(le));

	*v = sys_get_le32(le);
	return ret;
}

/*
 * The I/O reset (CCCR I/O abort, RES): a radio this host enumerated before it restarted is
 * still in the transfer state and does not answer the enumeration; this takes it back.
 */
static void sdio_io_reset(void)
{
	struct sdhc_command cmd = {
		.opcode = SDIO_RW_DIRECT,
		.arg = BIT(SDIO_CMD_ARG_RW_SHIFT) | (SDIO_CCCR_ABORT << SDIO_CMD_ARG_REG_ADDR_SHIFT) |
		       BIT(3),
		.response_type = SD_RSP_TYPE_R5,
		.timeout_ms = 100,
	};

	(void)sdhc_request(sdhc, &cmd, NULL);
}

/* Function 1 on an enumerated radio, and a new session. Bus lock held. */
static int session_start(void)
{
	int ret = sdio_init_func(&card, &f1, SDIO_FUNC_NUM_1);

	if (ret == 0) {
		ret = sdio_enable_func(&f1);
	}
	if (ret == 0) {
		ret = sdio_set_block_size(&f1, SLC_BLOCK);
	}
	if (ret == 0) {
		/* The radio drops a session it may have had with this host before (link_sdio.c),
		 * polling at least every millisecond; then both counters start from zero.
		 */
		ret = sdio_write_byte(&f1, SLC_HOST_INT, BIT(0));
	}
	if (ret == 0) {
		k_msleep(5);
		rx_got = 0;
		tx_bufs = 0;
	}
	return ret;
}

/*
 * After a bus error: a radio that restarted (a reset, a new image) comes back not enumerated
 * and answers nothing until this host enumerates it again. Bus lock held.
 */
static void recover(void)
{
	if (k_uptime_get() < next_recover) {
		return;
	}
	next_recover = k_uptime_get() + RECOVER_MS;
	sdio_io_reset();
	if (sd_init(sdhc, &card) == 0) {
		(void)session_start();
	}
}

int esdr_sdio_open(const struct device *dev, uint32_t ready_ms)
{
	int64_t end = k_uptime_get() + ready_ms;
	int ret;

	if (!device_is_ready(dev)) {
		return -ENODEV;
	}
	sdhc = dev;
	k_mutex_lock(&bus_lock, K_FOREVER);
	/* The radio answers once its image has armed the slave. */
	while ((ret = sd_init(sdhc, &card)) != 0) {
		if (k_uptime_get() > end) {
			goto out;
		}
		sdio_io_reset();
		k_msleep(10);
	}
	ret = session_start();
out:
	k_mutex_unlock(&bus_lock);
	return ret;
}

int esdr_sdio_read(uint8_t *buf, uint32_t max)
{
	uint32_t len, n = 0;
	int ret;

	k_mutex_lock(&bus_lock, K_FOREVER);
	ret = slc_reg(SLC_PKT_LEN, &len);
	if (ret == 0) {
		n = MIN((len - rx_got) & 0xFFFFFU, max) & ~3U;
		if (n > 0U) {
			ret = slc_xfer(false, buf, n);
		}
	}
	if (ret == 0) {
		rx_got += n;
	} else {
		recover();
	}
	k_mutex_unlock(&bus_lock);
	return ret == 0 ? (int)n : ret;
}

int esdr_sdio_send(const void *a, size_t na, const void *b, size_t nb, uint32_t timeout_ms)
{
	size_t n = na + nb, padded = ROUND_UP(n, 4U);
	uint32_t need = DIV_ROUND_UP(padded, SLC_RX_BUF);
	int64_t end = k_uptime_get() + timeout_ms;
	int ret = -ETIMEDOUT;

	if (padded > sizeof(wbuf)) {
		return -EMSGSIZE;
	}
	k_mutex_lock(&send_lock, K_FOREVER);
	memcpy(wbuf, a, na);
	if (nb > 0U) {
		memcpy(wbuf + na, b, nb);
	}
	memset(wbuf + n, 0, padded - n);
	while (k_uptime_get() <= end) {
		uint32_t token;
		bool room;

		k_mutex_lock(&bus_lock, K_FOREVER);
		ret = slc_reg(SLC_TOKEN_RDATA, &token);
		room = ret == 0 && (((token >> 16) - tx_bufs) & 0xFFFU) >= need;
		if (room) {
			ret = slc_xfer(true, wbuf, padded);
			if (ret == 0) {
				tx_bufs += need;
			}
		}
		if (ret != 0) {
			recover();
		}
		k_mutex_unlock(&bus_lock);
		if (ret == 0 && room) {
			break;
		}
		ret = ret != 0 ? ret : -ETIMEDOUT;
		k_sleep(K_TICKS(1));
	}
	k_mutex_unlock(&send_lock);
	return ret;
}
