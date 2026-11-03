/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The RP2040 of the MCH2022 badge (boards/badgeteam/mch2022_rp2040): all the stock firmware does,
 * in its order: the ESP32 starts once its I2C target is up.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "coproc.h"

LOG_MODULE_REGISTER(coproc, LOG_LEVEL_INF);

struct coproc_stats coproc_stats __aligned(8);

#define CHECK(call)                                                                                \
	do {                                                                                       \
		int ret_ = (call);                                                                 \
		if (ret_ != 0) {                                                                   \
			LOG_ERR(#call ": %d", ret_);                                               \
		}                                                                                  \
	} while (false)

int main(void)
{
	LOG_INF("reset reason 0x%08x, ROSC 0x%08x%s", sysctl_reset_reason(), sysctl_rosc_status(),
		sysctl_crashed() ? ", after a crash" : "");
	CHECK(regs_init());
	CHECK(uart_dma_init());
	CHECK(esp32_ctl_init());
	CHECK(pio_out_init());
	CHECK(periph_init());
	CHECK(regs_start());
	esp32_reset(false);
	CHECK(bridge_start());
	CHECK(usb_dev_init());
	return 0;
}
