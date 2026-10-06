/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ESP32-S3 radio core, ported from esp-sdr main/targets/esp32s3/receiver.c:
 * bring-up, the shared LO (both directions use it) and the dump bank.
 */

#include <errno.h>
#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <esp_sdr/esp_sdr.h>

#include <esp_wifi.h>

/* Upstream helpers, used unmodified. */
#include "rx_lo.h"
#include "rx_tuning.h"

#include "esp_sdr_priv.h"

LOG_MODULE_REGISTER(esp_sdr, CONFIG_ESP_SDR_LOG_LEVEL);

K_MUTEX_DEFINE(sdr_lock);
bool sdr_ready;
uint32_t sdr_freq_mhz = CAL_CHANNEL_MHZ;
int32_t sdr_fofs_khz;
uint32_t sdr_turn_settle_us = SDR_RETUNE_SETTLE_US;
/* Channel bandwidth flag for the vendor tuning call, 0 = 20 MHz. */
static unsigned int sdr_cbw;
bool sdr_turn_retune = true;

static bool is_channel(uint32_t mhz)
{
	return (mhz >= 2412U && mhz <= 2472U && (mhz - 2412U) % 5U == 0U) || mhz == 2484U;
}

void sdr_tune(void)
{
	rx_lo_plan_t plan = rx_lo_plan(sdr_freq_mhz);
	bool channel = sdr_fofs_khz == 0 && is_channel(sdr_freq_mhz);

	regi2c_enter_critical();
	rx_lo_select(false);
	regi2c_exit_critical();
	set_chanfreq(channel ? sdr_freq_mhz : CAL_CHANNEL_MHZ, sdr_cbw);
	if (!channel) {
		set_rf_freq_offset(0, plan.mhz, plan.offset_khz + sdr_fofs_khz);
	}
}

int sdr_bank_usage(uint32_t *usage)
{
	uintptr_t base = (uintptr_t)__esp_sdr_bank_start;
	uintptr_t bank = (base - DUMP_BANK0_ADDR) / DUMP_BANK_SIZE;

	if (base < DUMP_BANK0_ADDR || base % DUMP_BANK_SIZE != 0U || bank >= DUMP_BANKS) {
		return -ENOMEM;
	}
	*usage = BIT(bank) << SENSITIVE_INTERNAL_SRAM_MAC_DUMP_USAGE_S;
	return 0;
}

int esp_sdr_init(void)
{
	const struct device *wifi = DEVICE_DT_GET(DT_NODELABEL(wifi));
	uint32_t usage;
	int ret = 0;

	if (!device_is_ready(wifi)) {
		return -ENODEV;
	}
	if (sdr_bank_usage(&usage) != 0) {
		LOG_ERR("capture bank %p is not a dump bank", (void *)__esp_sdr_bank_start);
		return -ENOMEM;
	}

	k_mutex_lock(&sdr_lock, K_FOREVER);
	if (sdr_ready) {
		goto out;
	}
	/* The driver already applies its Kconfig power-save mode at boot. */
	if (esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK) {
		LOG_WRN("power save still enabled");
	}
	/* Promiscuous first: NULL mode only accepts a channel in promiscuous mode. */
	if (esp_wifi_set_promiscuous(true) != ESP_OK) {
		ret = -EIO;
		goto out;
	}
	if (esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
		(void)esp_wifi_set_promiscuous(false);
		ret = -EIO;
		goto out;
	}
	sdr_rx_prepare();
	sdr_ready = true;
	LOG_INF("receiver ready, bank %p", (void *)__esp_sdr_bank_start);
out:
	k_mutex_unlock(&sdr_lock);
	return ret;
}

int sdr_retune(void)
{
	if (!sdr_ready) {
		return -EAGAIN;
	}
	sdr_rx_prepare();
	return 0;
}

int esp_sdr_set_frequency(uint32_t mhz)
{
	int ret;

	if (!rx_frequency_valid(mhz)) {
		return -EINVAL;
	}
	k_mutex_lock(&sdr_lock, K_FOREVER);
	sdr_freq_mhz = mhz;
	ret = sdr_retune();
	k_mutex_unlock(&sdr_lock);
	return ret;
}

uint32_t esp_sdr_get_frequency(void)
{
	return sdr_freq_mhz;
}

int esp_sdr_set_freq_offset(int32_t khz)
{
	int ret;

	k_mutex_lock(&sdr_lock, K_FOREVER);
	sdr_fofs_khz = khz;
	ret = sdr_retune();
	k_mutex_unlock(&sdr_lock);
	return ret;
}

int esp_sdr_set_turnaround(uint32_t settle_us, bool retune)
{
	if (settle_us > SDR_RETUNE_SETTLE_US) {
		return -EINVAL;
	}
	k_mutex_lock(&sdr_lock, K_FOREVER);
	sdr_turn_settle_us = settle_us;
	sdr_turn_retune = retune;
	k_mutex_unlock(&sdr_lock);
	return 0;
}

int esp_sdr_set_channel_bw(unsigned int cbw)
{
	int ret;

	if (cbw > 2U) {
		return -EINVAL;
	}
	k_mutex_lock(&sdr_lock, K_FOREVER);
	sdr_cbw = cbw;
	ret = sdr_retune();
	k_mutex_unlock(&sdr_lock);
	return ret;
}
