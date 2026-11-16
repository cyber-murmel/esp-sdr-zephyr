/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The radio comes up as its module loads, after the module's constructors, before USB starts
 * (src/apps.c): boots without an app that needs it never bring it up.
 */

#include <zephyr/sys/printk.h>

#include <esp_sdr/esp_sdr.h>

int multiapp_module_up(void)
{
	int ret = esp_sdr_init();

	if (ret != 0) {
		printk("multiapp: radio init failed (%d)\n", ret);
	}
	return ret;
}
