/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The ESP32-C6 SDIO slave driven through the HAL's LL layer (there is no Zephyr driver):
 * function 1, 4-bit, the host's SLC protocol as esp-hosted uses it.
 *
 * - Host to slave: RX_BUFS buffers of RX_BUF_BYTES, each loaded one raises the token count the
 *   host reads before it writes. Writes land in them in order; the app reads them as a byte
 *   stream and hands each emptied buffer back.
 * - Slave to host: a queue of descriptors over the caller's buffers. While the DMA is idle,
 *   everything queued goes out as one chain and the byte counter (PKT_LEN) the host polls
 *   rises by its length; once the host has read the chain (EOF), its buffers are retired.
 *
 * - A host that (re)starts raises general-purpose interrupt 0 (it writes bit 0 of its
 *   register 0x08C): the send queue is dropped (its blocks count as done), the byte counter
 *   and the tokens start again, so that a host enumerating anew finds no chain of the last
 *   session in the way.
 *
 * Nothing here takes interrupts: the paced and gapless receivers run with them locked and pump
 * between samples, so what they call lives in IRAM (a flash cache miss would stall them).
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <esp_attr.h>
#include <hal/gpio_ll.h>
#include <hal/sdio_slave_ll.h>
#include <soc/gpio_struct.h>

#include "link_sdio.h"

/* Slot 0's IOMUX pins (function 0). */
#define PIN_CMD 18
#define PIN_CLK 19
#define PIN_D0  20
#define PIN_D1  21
#define PIN_D2  22
#define PIN_D3  23

#define RX_BUFS      16U
#define RX_BUF_BYTES 512U
/* Send queue entries, a power of 2; at most CHAIN_MAX in one chain, so that the rest stay the
 * app's to fill while the host reads it. */
#define SQ 32U
#define CHAIN_MAX 4U

static slc_dev_t *slc;

static sdio_slave_ll_desc_t rx_desc[RX_BUFS] __aligned(4);
static uint8_t rx_buf[RX_BUFS][RX_BUF_BYTES] __aligned(4);
static uint32_t rx_cur, rx_off, rx_tail;

static struct {
	sdio_slave_ll_desc_t desc[SQ];
	bool block[SQ];
	/* Running counters: queued up to head, the chain in flight is [start, end). */
	uint32_t head, start, end;
	uint32_t pkt_len, blocks_done;
} tx __aligned(4);

static void pin(int gpio, bool pullup)
{
	gpio_ll_pulldown_dis(&GPIO, gpio);
	gpio_ll_input_enable(&GPIO, gpio);
	gpio_ll_func_sel(&GPIO, gpio, 0);
	gpio_ll_set_drive_capability(&GPIO, gpio, GPIO_DRIVE_CAP_3);
	if (pullup) {
		gpio_ll_pullup_en(&GPIO, gpio);
	} else {
		gpio_ll_pullup_dis(&GPIO, gpio);
	}
}

void sdio_link_init(void)
{
	hinf_dev_t *hinf = sdio_slave_ll_get_hinf(0);
	host_dev_t *host = sdio_slave_ll_get_host(0);

	slc = sdio_slave_ll_get_slc(0);
	pin(PIN_CLK, false);
	pin(PIN_CMD, true);
	pin(PIN_D0, true);
	pin(PIN_D1, true);
	pin(PIN_D2, true);
	pin(PIN_D3, true);

	sdio_slave_ll_enable_bus_clock(true);
	sdio_slave_ll_reset_register();
	sdio_slave_ll_init(slc);
	sdio_slave_ll_init_version(hinf);
	sdio_slave_ll_enable_hs(hinf, true);
	sdio_slave_ll_set_timing(host, SDIO_SLAVE_TIMING_PSEND_PSAMPLE);

	/* Send: the done check reads the masked status (enabled, not routed to the CPU). */
	sdio_slave_ll_send_write_len(slc, 0);
	sdio_slave_ll_send_intr_ena(slc, true);
	/* The host's restart (general-purpose interrupt 0), polled likewise. */
	sdio_slave_ll_slvint_t ena = SDIO_SLAVE_LL_SLVINT_0;

	sdio_slave_ll_slvint_set_ena(slc, &ena);

	/* Receive: all buffers loaded, one token each. */
	for (uint32_t k = 0; k < RX_BUFS; k++) {
		rx_desc[k] = (sdio_slave_ll_desc_t){
			.size = RX_BUF_BYTES,
			.buf = rx_buf[k],
			.owner = 1,
		};
		rx_desc[k].qe.stqe_next = k + 1U < RX_BUFS ? &rx_desc[k + 1U] : NULL;
	}
	rx_cur = 0;
	rx_off = 0;
	rx_tail = RX_BUFS - 1U;
	sdio_slave_ll_recv_reset(slc);
	sdio_slave_ll_recv_size_reset(slc);
	sdio_slave_ll_recv_start(slc, &rx_desc[0]);
	for (uint32_t k = 0; k < RX_BUFS; k++) {
		sdio_slave_ll_recv_size_inc(slc);
	}

	sdio_slave_ll_set_ioready(hinf, true);
}

/* An emptied receive buffer back to the tail of the DMA's list, with its token. */
static IRAM_ATTR void rx_recycle(uint32_t k)
{
	sdio_slave_ll_desc_t *d = &rx_desc[k];

	d->eof = 0;
	d->length = 0;
	d->qe.stqe_next = NULL;
	d->owner = 1;
	rx_desc[rx_tail].qe.stqe_next = d;
	rx_tail = k;
	sdio_slave_ll_recv_restart(slc);
	sdio_slave_ll_recv_size_inc(slc);
}

/* The host restarted: drop what it will not read, start the counters again. */
static IRAM_ATTR void host_restart(void)
{
	sdio_slave_ll_send_stop(slc);
	sdio_slave_ll_send_reset(slc);
	sdio_slave_ll_send_intr_clr(slc);
	for (uint32_t k = tx.start; k != tx.head; k++) {
		if (tx.block[k % SQ]) {
			tx.blocks_done++;
		}
	}
	tx.start = tx.end = tx.head;
	tx.pkt_len = 0;
	sdio_slave_ll_send_write_len(slc, 0);
	/* Tokens: one per buffer the DMA holds (the rest come back as the app empties them). */
	sdio_slave_ll_recv_size_reset(slc);
	for (uint32_t k = 0; k < RX_BUFS; k++) {
		if (rx_desc[k].owner != 0U) {
			sdio_slave_ll_recv_size_inc(slc);
		}
	}
}

IRAM_ATTR bool sdio_link_readable(void)
{
	sdio_slave_ll_slvint_t slv;

	sdio_slave_ll_slvint_fetch_clear(slc, &slv);
	if ((slv & SDIO_SLAVE_LL_SLVINT_0) != 0U) {
		host_restart();
	}
	for (;;) {
		const sdio_slave_ll_desc_t *d = &rx_desc[rx_cur];

		if (d->owner != 0U) {
			return false;
		}
		if (rx_off < d->length) {
			return true;
		}
		rx_recycle(rx_cur);
		rx_cur = (rx_cur + 1U) % RX_BUFS;
		rx_off = 0;
	}
}

IRAM_ATTR uint8_t sdio_link_getc(void)
{
	return rx_buf[rx_cur][rx_off++];
}

IRAM_ATTR bool sdio_link_queue(const uint8_t *p, size_t n, bool block)
{
	sdio_slave_ll_desc_t *d;

	if (tx.head - tx.start >= SQ || n == 0U || n > 16380U) {
		return false;
	}
	d = &tx.desc[tx.head % SQ];
	*d = (sdio_slave_ll_desc_t){
		.size = (n + 3U) & ~3U,
		.length = n,
		.buf = p,
	};
	tx.block[tx.head % SQ] = block;
	tx.head++;
	return true;
}

IRAM_ATTR void sdio_link_pump(void)
{
	if (tx.end != tx.start) {
		if (!sdio_slave_ll_send_done(slc)) {
			return;
		}
		for (uint32_t k = tx.start; k != tx.end; k++) {
			if (tx.block[k % SQ]) {
				tx.blocks_done++;
			}
		}
		tx.start = tx.end;
	}
	if (tx.head == tx.end) {
		return;
	}
	/* What is queued, CHAIN_MAX at most, as one chain; the host's counter rises by its length. */
	uint32_t bytes = 0;
	uint32_t stop = tx.head - tx.end > CHAIN_MAX ? tx.end + CHAIN_MAX : tx.head;

	for (uint32_t k = tx.end; k != stop; k++) {
		sdio_slave_ll_desc_t *d = &tx.desc[k % SQ];
		bool last = k + 1U == stop;

		d->eof = last;
		d->owner = 1;
		d->qe.stqe_next = last ? NULL : &tx.desc[(k + 1U) % SQ];
		bytes += d->length;
	}
	sdio_slave_ll_send_stop(slc);
	sdio_slave_ll_send_reset(slc);
	sdio_slave_ll_send_intr_clr(slc);
	sdio_slave_ll_send_start(slc, &tx.desc[tx.end % SQ]);
	tx.pkt_len += bytes;
	sdio_slave_ll_send_write_len(slc, tx.pkt_len);
	tx.end = stop;
}

IRAM_ATTR uint32_t sdio_link_pending(void)
{
	return tx.head - tx.start;
}

IRAM_ATTR uint32_t sdio_link_blocks_done(void)
{
	return tx.blocks_done;
}
