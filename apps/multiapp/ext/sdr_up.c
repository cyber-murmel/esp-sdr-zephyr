/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The radio comes up as its module loads, after the module's constructors, before USB starts
 * (src/apps.c): boots without an app that needs it never bring it up.
 */

#include <zephyr/sys/printk.h>

#include <esp_sdr/esp_sdr.h>
#include <multiapp/multiapp.h>

int multiapp_module_up(void)
{
	int ret;

#if defined(CONFIG_SOC_SERIES_ESP32S3)
	/* Before the first claim: this boot's app keeps module code and data in dump bank 2. */
	if (multiapp_bank2_reserved()) {
		esp_sdr_bank2_reserve();
	}
#endif
	ret = esp_sdr_init();

	if (ret != 0) {
		printk("multiapp: radio init failed (%d)\n", ret);
	}
	return ret;
}

/*
 * The inverse (src/apps.c, when the next boot's modules leave this one out). ESP32-S3: the ring,
 * the DAC's threads, the arbiter's claim and hooks. ESP32-S31: a ring run still going, a tone or
 * DAC loop the app left, the PHY off and the modem clock given back (esp_sdr_release_radio()).
 */
int multiapp_module_down(void)
{
	return esp_sdr_release_radio();
}
