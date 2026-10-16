/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Channelized burst capture over USB: the library's capture pipeline (esp_sdr_chancap.h),
 * every capture as VITA 49.2 on its channel's stream over a vendor bulk endpoint, next to
 * the CDC-ACM shell and DFU. The ring owns CPU 1; the channelizer, USB and the shell share
 * CPU 0.
 */

#include <zephyr/app_version.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_rx.h>

#include "app.h"
#include "app_cpu.h"
#include "app_usb.h"

LOG_MODULE_REGISTER(chancap, LOG_LEVEL_INF);

static struct cc_settings settings = {
	.cfg = {
		.first_freq_hz = CONFIG_APP_FIRST_FREQ_HZ,
		.spacing_hz = CONFIG_APP_SPACING_HZ,
		.n_channels = CONFIG_APP_CHANNELS,
		.channel_rate_hz = CONFIG_APP_CHANNEL_RATE_HZ,
		.wb_gate = true,
		.ch_gate = true,
		.wb_threshold_db = CONFIG_APP_WB_THRESHOLD_DB,
		.ch_threshold_db = CONFIG_APP_CH_THRESHOLD_DB,
		.lookback_us = CONFIG_APP_LOOKBACK_US,
		.hang_us = CONFIG_APP_HANG_US,
		.max_capture_us = CONFIG_APP_MAX_CAPTURE_US,
		.cb = vrt_cc_capture,
	},
	.rx_gain = CONFIG_APP_RX_GAIN,
	.rx_bw_mhz = CONFIG_APP_RX_BW_MHZ,
};

struct cc_settings *cc_settings(void)
{
	return &settings;
}

int cc_start(void)
{
	int ret;

	if (esp_sdr_chancap_active()) {
		return -EBUSY;
	}
	ret = esp_sdr_rx_set_gain(settings.rx_gain < 0 ? ESP_SDR_RX_GAIN_AUTO : settings.rx_gain);
	if (ret != 0) {
		LOG_WRN("rx gain %d: %d", settings.rx_gain, ret);
	}
	if (settings.rx_bw_mhz >= 0) {
		uint32_t bw_min, bw_max;

		esp_sdr_rx_bandwidth_range(&bw_min, &bw_max);
		ret = esp_sdr_rx_set_bandwidth(settings.rx_bw_mhz == 0 ? bw_max
								      : (uint32_t)settings.rx_bw_mhz);
	} else {
		ret = esp_sdr_rx_set_lpf(ESP_SDR_RX_LPF_AUTO);
	}
	if (ret != 0) {
		LOG_WRN("rx bandwidth %d MHz: %d", settings.rx_bw_mhz, ret);
	}
	return esp_sdr_chancap_start(&settings.cfg);
}

void cc_stop(void)
{
	esp_sdr_chancap_stop();
}

/* Before a DFU download: the ring would keep CPU 1 from taking the flash stall request. */
void app_usb_dfu_prepare(void)
{
	cc_stop();
}

int main(void)
{
	int ret;

	printk("chancap %s\n", APP_VERSION_STRING);
	/* Before USB starts: its controller interrupt lands on the CPU that allocates it. */
	app_cpu_pin_system_threads(0);
	if (app_usb_init() != 0) {
		LOG_ERR("usb init failed");
	}
	(void)vrt_cc_init();
	ret = esp_sdr_init();
	if (ret != 0) {
		LOG_ERR("radio init: %d", ret);
		return 0;
	}
	/* Again for the threads started since (usbd, UDC). */
	k_sleep(K_MSEC(500));
	app_cpu_pin_system_threads(0);
	if (IS_ENABLED(CONFIG_APP_AUTOSTART)) {
		ret = cc_start();
		if (ret != 0) {
			LOG_ERR("start: %d", ret);
		}
	}
	return 0;
}
