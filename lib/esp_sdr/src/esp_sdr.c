/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Radio core, ported from esp-sdr main/targets/esp32s3/receiver.c and
 * main/targets/esp32c6/chip.h: bring-up, the shared LO (both directions use
 * it) and the dump bank.
 */

#include <errno.h>
#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_cal.h>
#include <esp_sdr/esp_sdr_rx.h>

#if defined(CONFIG_ESP_SDR_PHY_ONLY)
#include <esp_phy_init.h>
#include <esp_private/esp_modem_clock.h>
#include <esp_timer.h>
#else
#include <esp_wifi.h>
#endif

/* Upstream helpers, used unmodified. */
#if !defined(CONFIG_SOC_SERIES_ESP32C5)
#include "rx_lo.h"
#endif
#include "rx_tuning.h"

#include "esp_sdr_priv.h"

LOG_MODULE_REGISTER(esp_sdr, CONFIG_ESP_SDR_LOG_LEVEL);

#if defined(CONFIG_SOC_SERIES_ESP32C6)
#include <zephyr/init.h>
#include <hal/modem_lpcon_ll.h>
#include <hal/modem_syscon_ll.h>

/*
 * A chip reset (USB-Serial-JTAG, esptool) leaves the C6's modem subsystem as
 * the previous firmware left it; ESP-IDF resets it on a restart, a chip reset
 * does not. After the native 802.15.4 radio had run, the receive floor stayed
 * about 25 dB up and the vendor's channel tuning landed MHz off, until a power
 * cycle or deep sleep. Reset it before the Wi-Fi driver brings the radio up.
 */
static int esp_sdr_modem_reset(void)
{
	modem_syscon_ll_reset_all(&MODEM_SYSCON);
	modem_lpcon_ll_reset_all(&MODEM_LPCON);
	return 0;
}
SYS_INIT(esp_sdr_modem_reset, PRE_KERNEL_1, 0);
#endif

K_MUTEX_DEFINE(esp_sdr_lock);
struct esp_sdr_stats esp_sdr_counters;
bool esp_sdr_ready;
uint32_t esp_sdr_freq_mhz = CAL_CHANNEL_MHZ;
int32_t esp_sdr_fofs_khz;
int32_t esp_sdr_corr_ppb;
uint32_t esp_sdr_turn_settle_us = SDR_RETUNE_SETTLE_US;
/* Channel bandwidth flag for the vendor tuning call, 0 = 20 MHz. */
static unsigned int sdr_cbw;
bool esp_sdr_turn_retune = true;

#if !defined(CONFIG_SOC_SERIES_ESP32C5)
static bool is_channel(uint32_t mhz)
{
	return (mhz >= 2412U && mhz <= 2472U && (mhz - 2412U) % 5U == 0U) || mhz == 2484U;
}
#endif

/*
 * LO plan. Upstream's rx_lo_plan() uses the 5/6 divider from 1842 MHz only and
 * tries the PLL directly below. The VCO does not reach below about 2130 MHz
 * (ESP32-C6) to 2180 MHz (ESP32-S3), so below 2210 MHz the divider is always
 * the better choice: it reaches down to the VCO's lower limit / 1.2.
 */
#define LO_DIVIDER_BELOW_MHZ 2210U

bool esp_sdr_lo_divided(void)
{
#if defined(CONFIG_SOC_SERIES_ESP32C5)
	/* No divided LO path qualified on the C5. */
	return false;
#else
	return esp_sdr_freq_mhz < LO_DIVIDER_BELOW_MHZ;
#endif
}

/*
 * RF PLL status after the vendor calibration (both chips, I2C block 0x62):
 * bits 3:2 compare the requested frequency with the VCO's tuning window: 0
 * inside it, 1 below it (capacitor code at its maximum), 2 above it (code 0).
 * Outside the window the PLL may still lock for about 12 MHz (measured on one
 * S3 and one C6), so this errs on the safe side.
 */
#define RFPLL_BLOCK       0x62U
#define RFPLL_STATUS_REG  12U
#define RFPLL_STATUS_VCO  0x0cU

static bool pll_in_window(void)
{
#if defined(CONFIG_SOC_SERIES_ESP32C5) || defined(CONFIG_SOC_SERIES_ESP32)
	/* The PLL status bits of the C5 and the ESP32 are not mapped yet. */
	return true;
#else
	unsigned int status;

	regi2c_enter_critical();
	status = rx_lo_read(RFPLL_BLOCK, RX_LO_HOST, RFPLL_STATUS_REG);
	regi2c_exit_critical();
	return (status & RFPLL_STATUS_VCO) == 0U;
#endif
}

/*
 * The crystal correction: a constant (esp_sdr_set_freq_corr_ppb()) or a model of
 * the error over the die temperature (esp_sdr_set_freq_corr_model()), evaluated at
 * every full retune (esp_sdr_rx_prepare(), esp_sdr_tx_prepare(), the paced
 * transmit start), not in the turnarounds, which must stay short; guarded by
 * esp_sdr_lock. A calibration run freezes it, so that all its estimates are
 * against one correction.
 */
static int32_t corr_const_ppb;
static struct esp_sdr_cal_record corr_model;
bool esp_sdr_corr_frozen;

void esp_sdr_corr_refresh(void)
{
	int16_t dc;
	int32_t ppb;

	if (esp_sdr_corr_frozen) {
		return;
	}
	if (corr_model.points > 0U && esp_sdr_get_temp_dc(&dc) == 0 &&
	    esp_sdr_cal_eval(&corr_model, dc, &ppb) == 0) {
		esp_sdr_corr_ppb = CLAMP(ppb, -ESP_SDR_FREQ_CORR_MAX_PPB, ESP_SDR_FREQ_CORR_MAX_PPB);
	} else if (corr_model.points == 0U) {
		esp_sdr_corr_ppb = corr_const_ppb;
	}
	/* A model without a temperature now keeps the last value it gave. */
}

void esp_sdr_corr_model_get(struct esp_sdr_cal_record *rec)
{
	k_mutex_lock(&esp_sdr_lock, K_FOREVER);
	*rec = corr_model;
	if (rec->points == 0U) {
		*rec = (struct esp_sdr_cal_record){.ppb = corr_const_ppb, .cell_id = -1,
						   .temp_dc = ESP_SDR_CAL_TEMP_UNKNOWN};
	}
	k_mutex_unlock(&esp_sdr_lock);
}

#if !defined(CONFIG_ESP_SDR_TEMP)
int esp_sdr_get_temp_dc(int16_t *dc)
{
	ARG_UNUSED(dc);
	return -ENOTSUP;
}
#endif

/*
 * The PLL runs off the crystal, so its error scales every frequency: asking for
 * f / (1 + ppb) gives f. Whole kHz, as the vendor calls take them.
 */
int32_t esp_sdr_corr_khz(uint32_t pll_khz)
{
	int64_t num = -(int64_t)pll_khz * esp_sdr_corr_ppb;
	int64_t den = 1000000000LL + esp_sdr_corr_ppb;

	return (int32_t)((num + (num < 0 ? -den / 2 : den / 2)) / den);
}

#if defined(CONFIG_SOC_SERIES_ESP32C5)
void esp_sdr_tune(void)
{
	int32_t khz = esp_sdr_fofs_khz;

	khz += esp_sdr_corr_khz(esp_sdr_freq_mhz * 1000U + (uint32_t)khz);
	/* A full channel setup at any MHz: the PHY takes values above 14 as MHz. */
	phy_set_chanfreq(esp_sdr_freq_mhz, sdr_cbw);
	if (khz != 0) {
		phy_set_rf_freq_offset(phy_param[PHY_PARAM_XTAL], esp_sdr_freq_mhz, khz);
	}
}
#else
void esp_sdr_tune(void)
{
	bool divided = esp_sdr_lo_divided();
	uint32_t pll_khz = esp_sdr_freq_mhz * (divided ? 1200U : 1000U);
	int32_t corr = esp_sdr_corr_khz(pll_khz + (uint32_t)esp_sdr_fofs_khz);

	rx_lo_plan_t plan = {pll_khz / 1000U, (int)(pll_khz % 1000U) + corr, divided};
	bool channel = esp_sdr_fofs_khz == 0 && corr == 0 && is_channel(esp_sdr_freq_mhz);

	regi2c_enter_critical();
	rx_lo_select(false);
	regi2c_exit_critical();
#if defined(CONFIG_SOC_SERIES_ESP32S3)
	set_chanfreq(channel ? esp_sdr_freq_mhz : CAL_CHANNEL_MHZ, sdr_cbw);
	if (!channel) {
		set_rf_freq_offset(0, plan.mhz, plan.offset_khz + esp_sdr_fofs_khz);
	}
#elif defined(CONFIG_SOC_SERIES_ESP32)
	set_chanfreq(channel ? esp_sdr_freq_mhz : CAL_CHANNEL_MHZ, sdr_cbw);
	if (!channel) {
		int32_t khz = plan.offset_khz + esp_sdr_fofs_khz;
		unsigned int cap;

		/* The channel setup holds its capacitor code; released, the PLL locks anew. */
		regi2c_enter_critical();
		cap = ram_chip_i2c_readReg(RFPLL_BLOCK, RFPLL_HOST, RFPLL_CAP_REG);
		ram_chip_i2c_writeReg(RFPLL_BLOCK, RFPLL_HOST, RFPLL_CAP_REG,
				      cap & ~RFPLL_CAP_HOLD);
		regi2c_exit_critical();
		/* 40 MHz crystal; the ROM takes 1/1024 MHz. */
		rom_set_rf_freq_offset(0, plan.mhz, (khz * 1024 + (khz < 0 ? -500 : 500)) / 1000);
	}
#else
	/* The channel call goes through mhz2ieee and loses off-grid requests. */
	chip_v7_set_chan(channel ? esp_sdr_freq_mhz : CAL_CHANNEL_MHZ, sdr_cbw);
	if (!channel) {
		phy_set_freq(plan.mhz, plan.offset_khz + esp_sdr_fofs_khz);
	}
#endif
}
#endif

int esp_sdr_bank_usage(uint32_t *usage)
{
	uintptr_t base = (uintptr_t)__esp_sdr_bank_start;
	uintptr_t bank = (base - DUMP_BANK0_ADDR) / DUMP_BANK_SIZE;

	if (base < DUMP_BANK0_ADDR || (base - DUMP_BANK0_ADDR) % DUMP_BANK_SIZE != 0U ||
	    bank >= DUMP_BANKS) {
		return -ENOMEM;
	}
#if defined(CONFIG_SOC_SERIES_ESP32)
	*usage = FIELD_PREP(DUMP_USAGE_M, DUMP_USAGE_MODE);
#else
	*usage = (BIT(bank) << DUMP_USAGE_S) | DUMP_USAGE_ALLOC;
#endif
	return 0;
}

#if defined(CONFIG_ESP_SDR_PHY_ONLY)
/* The Wi-Fi driver's power-up (wifi_init.c, esp_adapter.c) without the driver. */
static int radio_up(void)
{
	/* The PHY's PLL tracking timer needs it; the radio drivers start it at boot. */
	esp_err_t err = esp_timer_init();

	if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
		return -EIO;
	}
	esp_wifi_bt_power_domain_on();
	modem_clock_module_mac_reset(PERIPH_WIFI_MODULE);
	modem_clock_module_enable(PERIPH_WIFI_MODULE);
	esp_phy_enable(PHY_MODEM_WIFI);
	phy_wifi_enable_set(1);
	return 0;
}
#else
static int radio_up(void)
{
	/* The driver already applies its Kconfig power-save mode at boot. */
	if (esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK) {
		LOG_WRN("power save still enabled");
	}
	/* Promiscuous first: NULL mode only accepts a channel in promiscuous mode. */
	if (esp_wifi_set_promiscuous(true) != ESP_OK) {
		return -EIO;
	}
	if (esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
		(void)esp_wifi_set_promiscuous(false);
		return -EIO;
	}
	return 0;
}
#endif

int esp_sdr_init(void)
{
	uint32_t usage;
	int ret = 0;

#if !defined(CONFIG_ESP_SDR_PHY_ONLY)
	if (!device_is_ready(DEVICE_DT_GET(DT_NODELABEL(wifi)))) {
		return -ENODEV;
	}
#endif
	if (esp_sdr_bank_usage(&usage) != 0) {
		LOG_ERR("capture bank %p is not a dump bank", (void *)__esp_sdr_bank_start);
		return -ENOMEM;
	}

	k_mutex_lock(&esp_sdr_lock, K_FOREVER);
	if (esp_sdr_ready) {
		goto out;
	}
	ret = radio_up();
	if (ret != 0) {
		goto out;
	}
#if defined(CONFIG_ESP_SDR_CALSTORE)
	esp_sdr_cal_apply_stored();
#endif
	esp_sdr_rx_prepare();
	esp_sdr_ready = true;
	LOG_INF("receiver ready, bank %p", (void *)__esp_sdr_bank_start);
out:
	k_mutex_unlock(&esp_sdr_lock);
	return ret;
}

int esp_sdr_retune(void)
{
	if (!esp_sdr_ready) {
		return -EAGAIN;
	}
	esp_sdr_rx_prepare();
	return 0;
}

int esp_sdr_set_freq(uint32_t mhz)
{
	int ret;

	if (!rx_frequency_valid(mhz)) {
		return -EINVAL;
	}
	k_mutex_lock(&esp_sdr_lock, K_FOREVER);
	esp_sdr_freq_mhz = mhz;
	ret = esp_sdr_retune();
	if (ret == 0 && !pll_in_window()) {
		ret = -ERANGE;
	}
	k_mutex_unlock(&esp_sdr_lock);
	return ret;
}

uint32_t esp_sdr_get_freq(void)
{
	return esp_sdr_freq_mhz;
}

int esp_sdr_set_freq_offset(int32_t khz)
{
	int ret;

	k_mutex_lock(&esp_sdr_lock, K_FOREVER);
	esp_sdr_fofs_khz = khz;
	ret = esp_sdr_retune();
	if (ret == 0 && !pll_in_window()) {
		ret = -ERANGE;
	}
	k_mutex_unlock(&esp_sdr_lock);
	return ret;
}

int esp_sdr_set_freq_corr_ppb(int32_t ppb)
{
	int ret;

	if (ppb < -ESP_SDR_FREQ_CORR_MAX_PPB || ppb > ESP_SDR_FREQ_CORR_MAX_PPB) {
		return -EINVAL;
	}
	k_mutex_lock(&esp_sdr_lock, K_FOREVER);
	corr_const_ppb = ppb;
	corr_model.points = 0U;
	esp_sdr_corr_ppb = ppb;
	ret = esp_sdr_ready ? esp_sdr_retune() : 0;
	if (ret == 0 && esp_sdr_ready && !pll_in_window()) {
		ret = -ERANGE;
	}
	k_mutex_unlock(&esp_sdr_lock);
	return ret;
}

int esp_sdr_set_freq_corr_model(const struct esp_sdr_cal_record *rec)
{
	int ret;

	k_mutex_lock(&esp_sdr_lock, K_FOREVER);
	corr_model = *rec;
	corr_model.points = MIN(corr_model.points, (uint8_t)ESP_SDR_CAL_POINTS);
	corr_const_ppb = CLAMP(rec->ppb, -ESP_SDR_FREQ_CORR_MAX_PPB, ESP_SDR_FREQ_CORR_MAX_PPB);
	esp_sdr_corr_ppb = corr_const_ppb;
	esp_sdr_corr_refresh();
	ret = esp_sdr_ready ? esp_sdr_retune() : 0;
	if (ret == 0 && esp_sdr_ready && !pll_in_window()) {
		ret = -ERANGE;
	}
	k_mutex_unlock(&esp_sdr_lock);
	return ret;
}

int32_t esp_sdr_get_freq_corr_ppb(void)
{
	return esp_sdr_corr_ppb;
}

int esp_sdr_set_turnaround(uint32_t settle_us, bool retune)
{
	if (settle_us > SDR_RETUNE_SETTLE_US) {
		return -EINVAL;
	}
	k_mutex_lock(&esp_sdr_lock, K_FOREVER);
	esp_sdr_turn_settle_us = settle_us;
	esp_sdr_turn_retune = retune;
	k_mutex_unlock(&esp_sdr_lock);
	return 0;
}

int esp_sdr_set_channel_bw(unsigned int cbw)
{
	int ret;

	if (cbw > 2U) {
		return -EINVAL;
	}
	k_mutex_lock(&esp_sdr_lock, K_FOREVER);
	sdr_cbw = cbw;
	ret = esp_sdr_retune();
	k_mutex_unlock(&esp_sdr_lock);
	return ret;
}

int esp_sdr_reset_settings(void)
{
	int ret;

	if (!esp_sdr_ready) {
		return -EAGAIN;
	}
	k_mutex_lock(&esp_sdr_lock, K_FOREVER);
	esp_sdr_freq_mhz = CAL_CHANNEL_MHZ;
	esp_sdr_fofs_khz = 0;
	sdr_cbw = 0;
	esp_sdr_turn_settle_us = SDR_RETUNE_SETTLE_US;
	esp_sdr_turn_retune = true;
	ret = esp_sdr_retune();
	k_mutex_unlock(&esp_sdr_lock);
	if (ret == 0) {
		ret = esp_sdr_rx_set_gain(ESP_SDR_RX_GAIN_AUTO);
	}
	if (ret == 0) {
		ret = esp_sdr_rx_set_lpf(ESP_SDR_RX_LPF_AUTO);
	}
	return ret;
}

void esp_sdr_get_stats(struct esp_sdr_stats *stats)
{
	*stats = esp_sdr_counters;
}
