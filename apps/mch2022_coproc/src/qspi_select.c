/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The SELECT button hangs on the flash's chip select (it is the RP2040's BOOTSEL), so it can only
 * be read with the flash out of use: from RAM, with every interrupt off, its pin let go for a
 * moment. After pico-examples' picoboard/button (Raspberry Pi, BSD-3-Clause). Nothing in here may
 * call into flash, and the compiler must not make a call of it: no helpers, only registers.
 */

#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>

#include <hardware/structs/ioqspi.h>
#include <hardware/structs/sio.h>

#include "coproc.h"

#define CS_INDEX 1U
/* Some 30 us at 125 MHz, for the pin to settle: 1k to the button against the pad's pull-up. */
#define SETTLE 400U

__ramfunc bool select_pressed(void)
{
	volatile uint32_t *const ctrl = &ioqspi_hw->io[CS_INDEX].ctrl;
	unsigned int key = irq_lock();
	uint32_t saved = *ctrl;
	bool released;

	*ctrl = (saved & ~IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS) |
		(IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_VALUE_DISABLE
		 << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB);
	for (volatile uint32_t i = 0; i < SETTLE; i++) {
	}
	released = (sio_hw->gpio_hi_in & (1U << CS_INDEX)) != 0U;
	*ctrl = saved;
	irq_unlock(key);
	return !released;
}
