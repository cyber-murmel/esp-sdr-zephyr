/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * MicroPython on the host. The host's console driver has no input path, as the shell owned the port
 * until this boot took it from it (src/apps.c), so the port's src/zephyr_getchar.c gets the two
 * functions it calls from here. The heap takes what PSRAM this boot leaves free.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/console/uart_console.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>

#include "py/gc.h"

#include <multiapp/multiapp.h>

static const struct device *const repl_uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static uart_console_in_debug_hook_t input_hook;

static void repl_uart_cb(const struct device *dev, void *user_data)
{
	uint8_t c;

	ARG_UNUSED(user_data);
	uart_irq_update(dev);
	while (uart_irq_rx_ready(dev) > 0 && uart_fifo_read(dev, &c, 1) == 1) {
		if (input_hook != NULL) {
			(void)input_hook(c);
		}
	}
}

void uart_console_in_debug_hook_install(uart_console_in_debug_hook_t hook)
{
	input_hook = hook;
}

void uart_register_input(struct k_fifo *avail, struct k_fifo *lines,
			 uint8_t (*completion)(char *str, uint8_t len))
{
	ARG_UNUSED(avail);
	ARG_UNUSED(lines);
	ARG_UNUSED(completion);
	(void)uart_irq_callback_user_data_set(repl_uart, repl_uart_cb, NULL);
	uart_irq_rx_enable(repl_uart);
}

void multiapp_mp_heap_add(void)
{
	size_t size;
	uint8_t *mem = multiapp_psram_spare(&size);

	gc_add(mem, mem + size);
}
