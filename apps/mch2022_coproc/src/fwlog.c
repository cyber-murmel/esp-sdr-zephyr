/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The firmware's log in a ring, there being no console: the UARTs belong to the ESP32 and the
 * FPGA. COPROC_REQ_GET_LOG reads it over USB.
 */

#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk-hooks.h>
#include <zephyr/sys/util.h>

#include "coproc.h"

#define LOG_BYTES 2048U

static uint8_t ring[LOG_BYTES];
static uint32_t head, tail;

static int log_out(int c)
{
	unsigned int key = irq_lock();

	ring[head++ % LOG_BYTES] = (uint8_t)c;
	if (head - tail > LOG_BYTES) {
		tail = head - LOG_BYTES;
	}
	irq_unlock(key);
	return c;
}

size_t fwlog_read(uint8_t *out, size_t max)
{
	size_t n = 0;

	while (n < max) {
		unsigned int key = irq_lock();
		bool empty = head == tail;

		if (!empty) {
			out[n++] = ring[tail++ % LOG_BYTES];
		}
		irq_unlock(key);
		if (empty) {
			break;
		}
	}
	return n;
}

static int fwlog_init(void)
{
	__printk_hook_install(log_out);
	return 0;
}
SYS_INIT(fwlog_init, PRE_KERNEL_1, 0);
