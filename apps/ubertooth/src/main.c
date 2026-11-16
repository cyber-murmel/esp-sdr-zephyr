/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ubertooth One device firmware on the ESP32-S3 SDR: the USB protocol of the
 * upstream host tools, the radio work on the ring receive and the DAC.
 */

#include <zephyr/app_version.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app_cpu.h"
#include "app_usb.h"
#include "ubt_plat.h"
#include "ubt_radio.h"

LOG_MODULE_REGISTER(ubertooth, LOG_LEVEL_INF);

int main(void)
{
	printk("ubertooth %s\n", APP_VERSION_STRING);
	/* Before USB starts: its controller interrupt lands on the CPU that allocates it. */
	app_cpu_pin_system_threads(0);
	if (ubt_init() != 0) {
		LOG_ERR("init failed");
		return 0;
	}
	if (app_usb_init() != 0) {
		LOG_ERR("usb init failed");
	}
	if (ubt_radio_init() != 0) {
		LOG_ERR("radio init failed");
	}
	/* Again for the threads started since (usbd, UDC). */
	k_sleep(K_MSEC(500));
	app_cpu_pin_system_threads(0);
	return 0;
}
