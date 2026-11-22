/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The radio comes up as its module loads, after the module's constructors, before USB starts
 * (src/apps.c): boots without an app that needs it never bring it up.
 */

#include <zephyr/sys/printk.h>

#include <esp_sdr/esp_sdr.h>
#if defined(CONFIG_ESP_SDR_DIAG16)
#include <esp_sdr/esp_sdr_diag16.h>
#endif
#if defined(CONFIG_ESP_SDR_RING)
#include <esp_sdr/esp_sdr_ring.h>
#endif
#include <multiapp/multiapp.h>

/*
 * Whether a ring runs: the host keeps the PC's drive away after a device write until it stops, so
 * that the PC's rescan does not land in a stream (src/drive_proxy.h). Called from the USB drive's
 * thread; a read of two flags.
 */
static bool ring_runs(void)
{
	bool busy = false;

#if defined(CONFIG_ESP_SDR_DIAG16)
	busy = busy || esp_sdr_diag16_active();
#endif
#if defined(CONFIG_ESP_SDR_RING)
	busy = busy || esp_sdr_ring_active();
#endif
	return busy;
}

int multiapp_module_up(void)
{
	int ret;

#if defined(CONFIG_SOC_SERIES_ESP32S3) || defined(CONFIG_SOC_SERIES_ESP32S31)
	/* Before the first claim: this boot's app keeps module code and data in the capture bank (S31: group 3). */
	if (multiapp_bank2_reserved()) {
		esp_sdr_bank2_reserve();
	}
#endif
	ret = esp_sdr_init();

	if (ret != 0) {
		printk("multiapp: radio init failed (%d)\n", ret);
	}
	multiapp_drive_busy_hook(ring_runs);
	return ret;
}

/*
 * The inverse (src/apps.c, when the next boot's modules leave this one out). ESP32-S3: the ring,
 * the DAC's threads, the arbiter's claim and hooks. ESP32-S31: a ring run still going, a tone or
 * DAC loop the app left, the PHY off and the modem clock given back (esp_sdr_release_radio()).
 */
int multiapp_module_down(void)
{
	/* Before the module's code goes: no call of it is left when this returns. */
	int ret;

	multiapp_drive_busy_hook(NULL);
	ret = esp_sdr_release_radio();
	if (ret != 0) {
		/* The module stays loaded. */
		multiapp_drive_busy_hook(ring_runs);
	}
	return ret;
}
