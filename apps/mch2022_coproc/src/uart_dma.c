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
 *
 * The FPGA's fast line has a ring of its own, and a PIO in place of the UART: a PL011 stops at clk_peri
 * over 16, 7.8 Mbaud; this oversamples 8 times, and the FPGA sends 15 Mbaud.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zephyr/drivers/misc/pio_rpi_pico/pio_rpi_pico.h>

#include <hardware/clocks.h>
#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/pio.h>
#include <hardware/structs/uart.h>
#include <hardware/regs/uart.h>

#include "coproc.h"

LOG_MODULE_REGISTER(uart_dma, LOG_LEVEL_INF);

#define RX_BITS_ESP32 14U
#define RX_BITS_FPGA  12U
/* At 1.4 MB/s, the ready line's reaction is the bridge thread's millisecond: plenty of room. */
#define RX_BITS_FAST  15U
#define TX_BYTES      4096U
#define RX_COUNT      0xFFFFFFFFU
#define DMA0_IRQ_LINE   11
#define DMA0_IRQ_PRIO  1

static uint8_t rx_esp32[BIT(RX_BITS_ESP32)] __aligned(BIT(RX_BITS_ESP32));
static uint8_t rx_fpga[BIT(RX_BITS_FPGA)] __aligned(BIT(RX_BITS_FPGA));
static uint8_t rx_fast[BIT(RX_BITS_FAST)] __aligned(BIT(RX_BITS_FAST));

#define FAST_PIO   pio1
#define FAST_SM    1U
#define FAST_RX    25U
#define FAST_READY 24U
#define FAST_OVERSAMPLE 8U

/* Eight times a bit, 8N1, a byte at a time (Raspberry Pi's pio/uart_rx, BSD-3-Clause). */
RPI_PICO_PIO_DEFINE_PROGRAM(fast_rx, 1, 8,
	/* wait 1 pin, 0 */
	0x20a0,
	/* wait 0 pin, 0 */
	0x2020,
	/* set x, 7 [10] */
	0xea27,
	/* in pins, 1 */
	0x4001,
	/* jmp x--, 3 [6] */
	0x0643,
	/* jmp pin, 8 */
	0x00c8,
	/* irq nowait 4 rel: a framing error */
	0xc014,
	/* jmp 0 */
	0x0000,
	/* push block */
	0x8020);

static struct port {
	const struct device *dev;
	uart_hw_t *hw;
	uint8_t *rx;
	uint32_t rx_size;
	int rx_ch, tx_ch;
	/* Bytes received up to this channel's run, and consumed. */
	uint64_t rx_base, rx_done;
	/* The channel was stopped, at this count: an abort leaves its transfer count at 0. */
	bool rx_stopped;
	uint64_t rx_end;
	/* The transmit ring: written, sent, and the piece the channel is on (0: none). */
	uint8_t tx[TX_BYTES];
	uint32_t tx_head, tx_tail, tx_piece;
	struct k_mutex tx_lock;
} ports[PORT_FAST + 1];

static uint64_t rx_produced(struct port *p)
{
	unsigned int key = irq_lock();
	uint64_t n = p->rx_stopped ? p->rx_end
				   : p->rx_base + (RX_COUNT - dma_hw->ch[p->rx_ch].transfer_count);

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

		if (port == PORT_FAST) {
			coproc_stats.fast_overrun += (uint32_t)(pending - keep);
		} else {
			coproc_stats.ring_overrun[port] += (uint32_t)(pending - keep);
		}
		p->rx_done = produced - keep;
		pending = keep;
	}
	if (port == PORT_FAST) {
		coproc_stats.fast_rx = produced;
	} else {
		coproc_stats.uart_rx[port] = produced;
	}
	off = (uint32_t)p->rx_done & (p->rx_size - 1U);
	*data = &p->rx[off];
	return (size_t)MIN(pending, (uint64_t)(p->rx_size - off));
}

void uart_rx_consume(int port, size_t n)
{
	ports[port].rx_done += n;
}

size_t uart_rx_pending(int port)
{
	struct port *p = &ports[port];

	return (size_t)(rx_produced(p) - p->rx_done);
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
	for (int k = 0; k < ARRAY_SIZE(ports); k++) {
		struct port *p = &ports[k];

		if (p->rx != NULL && (st & BIT(p->rx_ch)) != 0U) {
			/* After hours at any rate. The FIFO holds what comes meanwhile. */
			p->rx_base += RX_COUNT;
			dma_channel_set_trans_count(p->rx_ch, RX_COUNT, true);
		}
		if (k < PORT_COUNT && (st & BIT(p->tx_ch)) != 0U) {
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

int fast_rx_start(uint32_t baud)
{
	static uint offset;
	static bool loaded;
	struct port *p = &ports[PORT_FAST];
	pio_sm_config c = pio_get_default_sm_config();
	dma_channel_config d;

	if (baud == 0U || baud > clock_get_hz(clk_sys) / FAST_OVERSAMPLE) {
		return -EINVAL;
	}
	if (!loaded) {
		offset = pio_add_program(FAST_PIO, RPI_PICO_PIO_GET_PROGRAM(fast_rx));
		p->rx_ch = dma_claim_unused_channel(true);
		p->rx = rx_fast;
		p->rx_size = BIT(RX_BITS_FAST);
		loaded = true;
	}
	fast_rx_stop();
	/* Whatever was in the ring belonged to an earlier run. */
	p->rx_base = 0;
	p->rx_done = 0;

	sm_config_set_in_pins(&c, FAST_RX);
	sm_config_set_jmp_pin(&c, FAST_RX);
	sm_config_set_in_shift(&c, true, false, 0);
	sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
	sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / ((float)baud * FAST_OVERSAMPLE));
	sm_config_set_wrap(&c, offset + RPI_PICO_PIO_GET_WRAP_TARGET(fast_rx),
			   offset + RPI_PICO_PIO_GET_WRAP(fast_rx));
	pio_gpio_init(FAST_PIO, FAST_RX);
	pio_sm_set_consecutive_pindirs(FAST_PIO, FAST_SM, FAST_RX, 1, false);
	pio_sm_init(FAST_PIO, FAST_SM, offset, &c);

	d = dma_channel_get_default_config(p->rx_ch);
	channel_config_set_transfer_data_size(&d, DMA_SIZE_8);
	channel_config_set_read_increment(&d, false);
	channel_config_set_write_increment(&d, true);
	channel_config_set_ring(&d, true, RX_BITS_FAST);
	channel_config_set_dreq(&d, pio_get_dreq(FAST_PIO, FAST_SM, false));
	/* The byte is in the FIFO word's top: the shift register goes right. */
	dma_channel_set_irq0_enabled(p->rx_ch, true);
	dma_channel_configure(p->rx_ch, &d, p->rx, (const uint8_t *)&FAST_PIO->rxf[FAST_SM] + 3,
			      RX_COUNT, true);
	p->rx_stopped = false;
	pio_sm_set_enabled(FAST_PIO, FAST_SM, true);

	gpio_set_function(FAST_READY, GPIO_FUNC_SIO);
	gpio_set_dir(FAST_READY, true);
	gpio_put(FAST_READY, true);
	return 0;
}

void fast_rx_stop(void)
{
	struct port *p = &ports[PORT_FAST];

	if (p->rx == NULL) {
		return;
	}
	pio_sm_set_enabled(FAST_PIO, FAST_SM, false);
	/* The bridge may be halfway through reading the ring. */
	p->rx_end = rx_produced(p);
	p->rx_stopped = true;
	/*
	 * An abort may raise the channel's end interrupt, whose handler would start it again,
	 * and the abort would wait for it to stop for ever: its interrupt off first.
	 */
	dma_channel_set_irq0_enabled(p->rx_ch, false);
	dma_channel_abort(p->rx_ch);
	dma_hw->ints0 = BIT(p->rx_ch);
	/* Back to the UART, whose settings stayed. */
	gpio_set_function(FAST_RX, GPIO_FUNC_UART);
	gpio_set_function(FAST_READY, GPIO_FUNC_UART);
}

bool fast_rx_framing_error(void)
{
	/* The program's "irq nowait 4 rel". */
	const uint32_t flag = BIT(4U + FAST_SM);
	const bool set = (FAST_PIO->irq & flag) != 0U;

	if (set) {
		FAST_PIO->irq = flag;
	}
	return set;
}

void fast_rx_ready(bool ready)
{
	gpio_put(FAST_READY, ready);
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
