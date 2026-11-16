/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Both UARTs on DMA. Zephyr's PL011 driver sets them up and changes their line settings; it has no
 * DMA, and the DMA driver no ring mode, so this file owns DMACR and DR, with the hal's DMA calls
 * (the devicetree's DMA node stays off, the driver would not see these channels as taken).
 *
 * Receive: a channel writes a ring of its own for good, and the count it has left says how far it
 * got; the stock firmware's poll of the 32-byte FIFO loses bytes above some 2 Mbaud. Transmit: a
 * channel takes what lies together in a ring, its end interrupt starts the next piece.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <hardware/dma.h>
#include <hardware/structs/uart.h>
#include <hardware/regs/uart.h>

#include "coproc.h"

LOG_MODULE_REGISTER(uart_dma, LOG_LEVEL_INF);

#define RX_BITS_ESP32 14U
#define RX_BITS_FPGA  12U
#define TX_BYTES      4096U
#define RX_COUNT      0xFFFFFFFFU
#define DMA0_IRQ_LINE   11
#define DMA0_IRQ_PRIO  1

static uint8_t rx_esp32[BIT(RX_BITS_ESP32)] __aligned(BIT(RX_BITS_ESP32));
static uint8_t rx_fpga[BIT(RX_BITS_FPGA)] __aligned(BIT(RX_BITS_FPGA));

static struct port {
	const struct device *dev;
	uart_hw_t *hw;
	uint8_t *rx;
	uint32_t rx_size;
	int rx_ch, tx_ch;
	/* Bytes received up to this channel's run, and consumed. */
	uint64_t rx_base, rx_done;
	/* The transmit ring: written, sent, and the piece the channel is on (0: none). */
	uint8_t tx[TX_BYTES];
	uint32_t tx_head, tx_tail, tx_piece;
	struct k_mutex tx_lock;
} ports[PORT_COUNT];

static uint64_t rx_produced(struct port *p)
{
	unsigned int key = irq_lock();
	uint64_t n = p->rx_base + (RX_COUNT - dma_hw->ch[p->rx_ch].transfer_count);

	irq_unlock(key);
	return n;
}

size_t uart_rx_peek(int port, const uint8_t **data)
{
	struct port *p = &ports[port];
	uint64_t produced = rx_produced(p);
	uint64_t pending = produced - p->rx_done;
	uint32_t off;

	/* A margin: the channel may be writing there. */
	if (pending > p->rx_size - 64U) {
		uint64_t keep = p->rx_size / 2U;

		coproc_stats.ring_overrun[port] += (uint32_t)(pending - keep);
		p->rx_done = produced - keep;
		pending = keep;
	}
	coproc_stats.uart_rx[port] = produced;
	off = (uint32_t)p->rx_done & (p->rx_size - 1U);
	*data = &p->rx[off];
	return (size_t)MIN(pending, (uint64_t)(p->rx_size - off));
}

void uart_rx_consume(int port, size_t n)
{
	ports[port].rx_done += n;
}

/* With the lock held or from the interrupt: the channel on the next piece, if it is free. */
static void tx_start(struct port *p)
{
	unsigned int key = irq_lock();

	if (p->tx_piece == 0U && p->tx_head != p->tx_tail) {
		uint32_t off = p->tx_tail % TX_BYTES;
		uint32_t n = MIN(p->tx_head - p->tx_tail, TX_BYTES - off);

		p->tx_piece = n;
		dma_channel_transfer_from_buffer_now(p->tx_ch, &p->tx[off], n);
	}
	irq_unlock(key);
}

size_t uart_tx_room(int port)
{
	struct port *p = &ports[port];

	return TX_BYTES - (p->tx_head - p->tx_tail);
}

size_t uart_tx_write(int port, const uint8_t *data, size_t n)
{
	struct port *p = &ports[port];
	size_t done = 0;

	k_mutex_lock(&p->tx_lock, K_FOREVER);
	while (done < n) {
		uint32_t room = TX_BYTES - (p->tx_head - p->tx_tail);
		uint32_t off = p->tx_head % TX_BYTES;
		uint32_t len = MIN(MIN((uint32_t)(n - done), room), TX_BYTES - off);

		if (len == 0U) {
			break;
		}
		memcpy(&p->tx[off], data + done, len);
		p->tx_head += len;
		done += len;
	}
	k_mutex_unlock(&p->tx_lock);
	tx_start(p);
	return done;
}

bool uart_tx_drained(int port)
{
	struct port *p = &ports[port];

	return p->tx_head == p->tx_tail && (p->hw->fr & UART_UARTFR_BUSY_BITS) == 0U;
}

int uart_set_line(int port, const struct uart_config *cfg)
{
	return uart_configure(ports[port].dev, cfg);
}

void uart_poll_errors(int port)
{
	uart_hw_t *hw = ports[port].hw;
	uint32_t rsr = hw->rsr;

	if (rsr == 0U) {
		return;
	}
	hw->rsr = 0U;
	if ((rsr & UART_UARTRSR_OE_BITS) != 0U) {
		coproc_stats.uart_overrun[port]++;
	}
	if ((rsr & (UART_UARTRSR_FE_BITS | UART_UARTRSR_PE_BITS | UART_UARTRSR_BE_BITS)) != 0U) {
		coproc_stats.uart_framing[port]++;
	}
}

static void dma_isr(const void *arg)
{
	uint32_t st = dma_hw->ints0;

	ARG_UNUSED(arg);
	dma_hw->ints0 = st;
	for (int k = 0; k < PORT_COUNT; k++) {
		struct port *p = &ports[k];

		if ((st & BIT(p->rx_ch)) != 0U) {
			/* After hours at any rate. The FIFO holds what comes meanwhile. */
			p->rx_base += RX_COUNT;
			dma_channel_set_trans_count(p->rx_ch, RX_COUNT, true);
		}
		if ((st & BIT(p->tx_ch)) != 0U) {
			coproc_stats.uart_tx[k] += p->tx_piece;
			p->tx_tail += p->tx_piece;
			p->tx_piece = 0;
			tx_start(p);
			bridge_wake();
		}
	}
}

static int port_init(int k, const struct device *dev, uart_hw_t *hw, uint8_t *rx, uint32_t rx_bits,
		     unsigned int dreq_rx, unsigned int dreq_tx)
{
	struct port *p = &ports[k];
	dma_channel_config c;

	if (!device_is_ready(dev)) {
		return -ENODEV;
	}
	p->dev = dev;
	p->hw = hw;
	p->rx = rx;
	p->rx_size = BIT(rx_bits);
	k_mutex_init(&p->tx_lock);
	p->rx_ch = dma_claim_unused_channel(true);
	p->tx_ch = dma_claim_unused_channel(true);

	c = dma_channel_get_default_config(p->rx_ch);
	channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
	channel_config_set_read_increment(&c, false);
	channel_config_set_write_increment(&c, true);
	channel_config_set_ring(&c, true, rx_bits);
	channel_config_set_dreq(&c, dreq_rx);
	dma_channel_configure(p->rx_ch, &c, rx, &hw->dr, RX_COUNT, true);

	c = dma_channel_get_default_config(p->tx_ch);
	channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
	channel_config_set_read_increment(&c, true);
	channel_config_set_write_increment(&c, false);
	channel_config_set_dreq(&c, dreq_tx);
	dma_channel_configure(p->tx_ch, &c, &hw->dr, p->tx, 0, false);

	dma_channel_set_irq0_enabled(p->rx_ch, true);
	dma_channel_set_irq0_enabled(p->tx_ch, true);
	hw->dmacr = UART_UARTDMACR_RXDMAE_BITS | UART_UARTDMACR_TXDMAE_BITS;
	return 0;
}

int uart_dma_init(void)
{
	int ret;

	IRQ_CONNECT(DMA0_IRQ_LINE, DMA0_IRQ_PRIO, dma_isr, NULL, 0);
	ret = port_init(PORT_ESP32, DEVICE_DT_GET(DT_NODELABEL(uart0)), uart0_hw, rx_esp32,
			RX_BITS_ESP32, DREQ_UART0_RX, DREQ_UART0_TX);
	if (ret == 0) {
		ret = port_init(PORT_FPGA, DEVICE_DT_GET(DT_NODELABEL(uart1)), uart1_hw, rx_fpga,
				RX_BITS_FPGA, DREQ_UART1_RX, DREQ_UART1_TX);
	}
	if (ret == 0) {
		irq_enable(DMA0_IRQ_LINE);
	}
	return ret;
}
