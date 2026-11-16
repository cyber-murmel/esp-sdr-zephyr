/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The host link: USB-Serial-JTAG through Zephyr's interrupt driven UART API. Bytes go
 * through ring buffers; a thread feeds the ZNP and drops a partial frame after an idle gap.
 */
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

#include "zstack.h"

LOG_MODULE_REGISTER(transport, LOG_LEVEL_INF);

#define USB_PACKET 64

static const struct device *const uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_zstack_uart));

RING_BUF_DECLARE(rx_rb, CONFIG_ZSTACK_RX_BUF);
RING_BUF_DECLARE(tx_rb, CONFIG_ZSTACK_TX_BUF);
static K_SEM_DEFINE(rx_sem, 0, 1);
static struct znp *znp;
static uint32_t rx_dropped, tx_dropped;

static void uart_isr(const struct device *dev, void *user)
{
	ARG_UNUSED(user);

	uart_irq_update(dev);
	while (uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			uint8_t buf[USB_PACKET];
			int n = uart_fifo_read(dev, buf, sizeof(buf));

			if (n > 0) {
				if ((uint32_t)ring_buf_put(&rx_rb, buf, n) != (uint32_t)n) {
					rx_dropped++;
				}
				k_sem_give(&rx_sem);
			}
		}
		if (uart_irq_tx_ready(dev)) {
			uint8_t buf[USB_PACKET];
			uint32_t n = ring_buf_peek(&tx_rb, buf, sizeof(buf));

			if (n == 0U) {
				uart_irq_tx_disable(dev);
				continue;
			}
			n = uart_fifo_fill(dev, buf, n);
			ring_buf_consume(&tx_rb, n);
		}
	}
}

void transport_tx(void *user, const uint8_t *buf, size_t len)
{
	ARG_UNUSED(user);

	/* Whole frames only: a cut one would put the host's parser out of step. */
	if (ring_buf_space_get(&tx_rb) < len) {
		tx_dropped++;
		return;
	}
	ring_buf_put(&tx_rb, buf, len);
	uart_irq_tx_enable(uart);
}

static void rx_thread(void *a, void *b, void *c)
{
	uint8_t buf[USB_PACKET];
	uint32_t n;

	for (;;) {
		if (k_sem_take(&rx_sem, K_MSEC(CONFIG_ZSTACK_IDLE_MS)) != 0) {
			znp_rx_idle(znp);
			continue;
		}
		while ((n = ring_buf_get(&rx_rb, buf, sizeof(buf))) > 0U) {
			znp_rx(znp, buf, n);
		}
	}
}

K_THREAD_STACK_DEFINE(rx_stack, 3072);
static struct k_thread rx_thread_data;

void transport_start(struct znp *z)
{
	znp = z;
	if (!device_is_ready(uart)) {
		LOG_ERR("host link %s not ready", uart->name);
		return;
	}
	uart_irq_callback_user_data_set(uart, uart_isr, NULL);
	uart_irq_rx_enable(uart);
	k_thread_create(&rx_thread_data, rx_stack, K_THREAD_STACK_SIZEOF(rx_stack), rx_thread, NULL,
			NULL, NULL, K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
	k_thread_name_set(&rx_thread_data, "znp_rx");
}
