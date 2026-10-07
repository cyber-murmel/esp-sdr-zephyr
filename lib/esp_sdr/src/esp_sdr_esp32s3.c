/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ESP32-S3 burst receiver, ported from esp-sdr main/targets/esp32s3/receiver.c.
 */

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <esp_sdr/esp_sdr.h>

#include <esp_attr.h>
#include <esp_wifi.h>
#include <soc/sensitive_reg.h>
#include <soc/soc.h>

/* Upstream helpers, used unmodified. */
#include "rx_bandwidth.h"
#include "rx_lo.h"
#include "rx_tuning.h"

#include "esp_sdr_dac.h"

LOG_MODULE_REGISTER(esp_sdr, CONFIG_ESP_SDR_LOG_LEVEL);

/* Undocumented Wi-Fi MAC dump engine; layout from upstream. */
#define DUMP_CTRL_REG     0x60033d5cU
#define DUMP_CTRL_RUN     BIT(31)
#define DUMP_CTRL_TRIGGER BIT(19)
#define DUMP_CTRL_DONE    BIT(18)
#define DUMP_CTRL_16MSPS  BIT(16)
#define DUMP_CTRL_40MSPS  BIT(15)
#define DUMP_CTRL_COUNT   GENMASK(13, 0)
#define DUMP_CONFIG_REG   0x60033d90U
/* Four 6-bit lane selectors on sources 0..3: receive I/Q. */
#define DUMP_CONFIG_IQ    0x000c2040U
#define DUMP_TIMEOUT_US   20000U

/*
 * The dump engine's DAC-side mirror, 8 bytes above DUMP_CTRL_REG. Same RUN/
 * TRIGGER/DONE/rate bit positions as the ADC side; librftest.a's dactrig()
 * fills the bank with a ramp before arming this, which esp_sdr_play() skips
 * so the caller's own samples survive.
 */
#define DAC_TRIG_REG 0x60033d64U
/* Measured: the DAC side runs at 40 MS/s, bit 15 selects 80 MS/s; bits 16 and 27:20 do
 * nothing to the rate (dactrig() writes 27:20).
 */
#define DAC_CTRL_80MSPS BIT(15)

/* The dump engine writes one of four 64 KiB banks, one-hot in MAC_DUMP_USAGE. */
#define DUMP_BANK0_ADDR 0x3fcb0000U
#define DUMP_BANK_SIZE  0x10000U
#define DUMP_BANKS      3U /* bank 3 also holds ROM data */

#define SENTINEL 0xa5a0055aU

/* BBTOP analog baseband: I/Q low-pass capacitor codes in registers 4 and 5. */
#define BBTOP_BLOCK    0x67U
#define BBTOP_HOST     0U
#define BBTOP_LPF_REG  4U
#define BBTOP_LPF_DCAP GENMASK(5, 0)

/* Wi-Fi AGC: forced index 31:24, force 23, largest calibrated index 14:8. */
#define AGC_FORCE_REG      0x6001c02cU
#define AGC_GAIN_MAX       GENMASK(14, 8)
/* Larger maxima on the S3 point at uncalibrated table slots. */
#define AGC_GAIN_MAX_VALID 82U
/* Index upstream passes while the hardware AGC is in charge. */
#define AGC_DEFAULT_INDEX  40U
/*
 * Vendor TX power ladder (phy_tx_gain.o .rodata, read by tx_gain_set()):
 * gain memory index, baseband gain code and target power per step, strongest
 * first. esp_sdr_set_tx_gain() counts the other way, 0 = weakest.
 */
static const struct {
	uint8_t gain;
	uint16_t bb;
	int16_t power;
} tx_ladder[] = {
	{127, 0x0a0, 48},  {127, 0x020, 40},  {127, 0x100, 32},  {127, 0x080, 24},
	{127, 0x000, 16},  {111, 0x000, 12},  {95, 0x000, 6},    {107, 0x000, 0},
	{119, 0x000, -7},  {87, 0x000, -16},  {115, 0x000, -30}, {82, 0x000, -53},
	{81, 0x000, -63},  {49, 0x000, -78},  {48, 0x000, -99},  {32, 0x000, -110},
	{16, 0x000, -124}, {0, 0x000, -148},
};
#define TX_GAIN_INDEX_MAX ((int)ARRAY_SIZE(tx_ladder) - 1)
/* Weakest step by default; raise deliberately, with the attenuated link connected. */
#define TX_GAIN_DEFAULT_INDEX 0

/* Default calibration channel for direct PLL tuning. */
#define CAL_CHANNEL_MHZ 2412U

/* ROM and PHY library entry points. */
extern void rom_pbus_workmode(void);
extern void rom_pbus_xpd_rx_on(unsigned int en);
extern void rom_pbus_xpd_tx_off(void);
extern void rom_pbus_xpd_tx_on(unsigned int en);
extern void rom_set_rxclk_en(unsigned int en);
extern void set_chanfreq(unsigned int mhz, unsigned int bw);
extern void set_rf_freq_offset(unsigned int mode, unsigned int mhz, int khz);
extern void stop_tx_tone(unsigned int en);
#if defined(CONFIG_ESP_SDR_MANUAL_GAIN)
/* librftest.a: force, index, bt (0 selects the Wi-Fi AGC). */
extern void force_rx_gain(unsigned int force, unsigned int index, unsigned int bt);
/*
 * librftest.a: gain memory index, baseband gain code, digital gain. Writes
 * gain memory slot 0 (the one force_txon_mode(1, 0, 0) selects) and turns
 * the vendor's TX power tracking off.
 */
extern void force_tx_gain(unsigned int gain, unsigned int bb, int dig);
/* librftest.a: en sets the front end's force-TX-on bit (0x60006000 bit 1). */
extern void force_txon_mode(unsigned int en, unsigned int mode, unsigned int ofs);
#endif

/* Reserved by the module linker snippet. */
extern uint32_t __esp_sdr_bank_start[];
/* Transmit bank, only linked with CONFIG_ESP_SDR_TX_DAC (esp_sdr_bank1.ld). */
extern uint32_t __esp_sdr_bank1_start[];

static K_MUTEX_DEFINE(sdr_lock);
static bool sdr_ready;
static uint32_t freq_mhz = CAL_CHANNEL_MHZ;
static int32_t fofs_khz;
static int lpf_code = ESP_SDR_LPF_AUTO;
static int gain_index = ESP_SDR_GAIN_AUTO;
static int tx_gain_index = TX_GAIN_DEFAULT_INDEX;
static uint32_t lpf_saved[2];

static bool is_channel(uint32_t mhz)
{
	return (mhz >= 2412U && mhz <= 2472U && (mhz - 2412U) % 5U == 0U) || mhz == 2484U;
}

static void tune(void)
{
	rx_lo_plan_t plan = rx_lo_plan(freq_mhz);
	bool channel = fofs_khz == 0 && is_channel(freq_mhz);

	rx_lo_select(false);
	set_chanfreq(channel ? freq_mhz : CAL_CHANNEL_MHZ, 0);
	if (!channel) {
		set_rf_freq_offset(0, plan.mhz, plan.offset_khz + fofs_khz);
	}
}

static unsigned int gain_max(void)
{
	unsigned int max = FIELD_GET(AGC_GAIN_MAX, REG_READ(AGC_FORCE_REG));

	return max <= AGC_GAIN_MAX_VALID ? max : 0U;
}

static void gain_apply(void)
{
#if defined(CONFIG_ESP_SDR_MANUAL_GAIN)
	bool manual = gain_index >= 0;
	unsigned int index = manual ? MIN((unsigned int)gain_index, gain_max())
				    : MIN(AGC_DEFAULT_INDEX, gain_max());

	/* Releasing the force (manual false) hands control back to the AGC. */
	force_rx_gain(manual, index, 0);
#endif
}

static void prepare_rx(void)
{
	tune();
	stop_tx_tone(1);
#if defined(CONFIG_ESP_SDR_MANUAL_GAIN)
	force_txon_mode(0, 0, 0);
#endif
	rom_pbus_workmode();
	rom_pbus_xpd_tx_off();
	rom_pbus_xpd_rx_on(1);
	rom_set_rxclk_en(1);
	gain_apply();
	/* The 5/6 LO divider is selected after RX setup, as upstream. */
	rx_lo_select(rx_lo_plan(freq_mhz).alternate);
	k_busy_wait(3000);
}

/*
 * Mirror of prepare_rx() for esp_sdr_play(): swap the analog front end from
 * RX to TX before arming the DAC trigger. Call unprepare_tx() (= prepare_rx()
 * again) afterwards; the module's resting state is always RX-ready.
 */
/* Ladder step 0 (weakest) .. TX_GAIN_INDEX_MAX into gain memory slot 0. */
static void tx_gain_apply(int step)
{
#if defined(CONFIG_ESP_SDR_MANUAL_GAIN)
	force_tx_gain(tx_ladder[TX_GAIN_INDEX_MAX - step].gain,
		      tx_ladder[TX_GAIN_INDEX_MAX - step].bb, 0);
#else
	ARG_UNUSED(step);
#endif
}

static void prepare_tx(void)
{
	tune();
	rom_pbus_workmode();
	rom_pbus_xpd_rx_on(0);
	rom_pbus_xpd_tx_on(1);
	tx_gain_apply(tx_gain_index);
#if defined(CONFIG_ESP_SDR_MANUAL_GAIN)
	/* Without a frame in flight the MAC never enables TX on its own. */
	force_txon_mode(1, 0, 0);
#endif
	k_busy_wait(3000);
}

static void lpf_apply(void)
{
	for (unsigned int j = 0; j < ARRAY_SIZE(lpf_saved); j++) {
		unsigned int reg = BBTOP_LPF_REG + j;

		lpf_saved[j] = rom_chip_i2c_readReg(BBTOP_BLOCK, BBTOP_HOST, reg);
		if (lpf_code >= 0) {
			unsigned int val =
				(lpf_saved[j] & ~BBTOP_LPF_DCAP) | (unsigned int)lpf_code;

			rom_chip_i2c_writeReg(BBTOP_BLOCK, BBTOP_HOST, reg, val);
		}
	}
}

static void lpf_restore(void)
{
	if (lpf_code < 0) {
		return;
	}
	for (unsigned int j = 0; j < ARRAY_SIZE(lpf_saved); j++) {
		rom_chip_i2c_writeReg(BBTOP_BLOCK, BBTOP_HOST, BBTOP_LPF_REG + j, lpf_saved[j]);
	}
}

static int bank_usage(uint32_t *usage)
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
	if (bank_usage(&usage) != 0) {
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
	prepare_rx();
	sdr_ready = true;
	LOG_INF("receiver ready, bank %p", (void *)__esp_sdr_bank_start);
out:
	k_mutex_unlock(&sdr_lock);
	return ret;
}

static int retune(void)
{
	if (!sdr_ready) {
		return -EAGAIN;
	}
	prepare_rx();
	return 0;
}

int esp_sdr_set_frequency(uint32_t mhz)
{
	int ret;

	if (!rx_frequency_valid(mhz)) {
		return -EINVAL;
	}
	k_mutex_lock(&sdr_lock, K_FOREVER);
	freq_mhz = mhz;
	ret = retune();
	k_mutex_unlock(&sdr_lock);
	return ret;
}

uint32_t esp_sdr_get_frequency(void)
{
	return freq_mhz;
}

int esp_sdr_set_freq_offset(int32_t khz)
{
	int ret;

	k_mutex_lock(&sdr_lock, K_FOREVER);
	fofs_khz = khz;
	ret = retune();
	k_mutex_unlock(&sdr_lock);
	return ret;
}

int esp_sdr_set_gain(int index)
{
	int ret;

	if (index < ESP_SDR_GAIN_AUTO) {
		return -EINVAL;
	}
	if (index >= 0 && !IS_ENABLED(CONFIG_ESP_SDR_MANUAL_GAIN)) {
		return -ENOTSUP;
	}
	k_mutex_lock(&sdr_lock, K_FOREVER);
	if (index > (int)gain_max()) {
		ret = -EINVAL;
	} else {
		/* As upstream: a forced index alone can leave stale RX state. */
		gain_index = index;
		ret = retune();
	}
	k_mutex_unlock(&sdr_lock);
	return ret;
}

int esp_sdr_get_gain(void)
{
	return gain_index;
}

int esp_sdr_gain_max(void)
{
	return (int)gain_max();
}

int esp_sdr_set_tx_gain(int index)
{
	if (!IS_ENABLED(CONFIG_ESP_SDR_MANUAL_GAIN)) {
		return -ENOTSUP;
	}
	if (index < 0 || index > TX_GAIN_INDEX_MAX) {
		return -EINVAL;
	}
	k_mutex_lock(&sdr_lock, K_FOREVER);
	/* Picked up by prepare_tx() on the next esp_sdr_play(); no immediate effect. */
	tx_gain_index = index;
	k_mutex_unlock(&sdr_lock);
	return 0;
}

int esp_sdr_get_tx_gain(void)
{
	return tx_gain_index;
}

int esp_sdr_tx_gain_max(void)
{
	return TX_GAIN_INDEX_MAX;
}

int esp_sdr_tx_gain_power(int index)
{
	if (index < 0 || index > TX_GAIN_INDEX_MAX) {
		return INT_MIN;
	}
	return tx_ladder[TX_GAIN_INDEX_MAX - index].power;
}

int esp_sdr_set_bandwidth(uint32_t mhz)
{
	if (mhz != 0U && (mhz < RX_BANDWIDTH_MIN || mhz > RX_BANDWIDTH_MAX)) {
		return -EINVAL;
	}
	return esp_sdr_set_lpf(rx_bandwidth_dcap(mhz));
}

void esp_sdr_bandwidth_range(uint32_t *min, uint32_t *max)
{
	*min = RX_BANDWIDTH_MIN;
	*max = RX_BANDWIDTH_MAX;
}

int esp_sdr_set_lpf(int code)
{
	if (code < ESP_SDR_LPF_AUTO || code > ESP_SDR_LPF_MAX) {
		return -EINVAL;
	}
	k_mutex_lock(&sdr_lock, K_FOREVER);
	lpf_code = code;
	k_mutex_unlock(&sdr_lock);
	return 0;
}

int esp_sdr_get_lpf(void)
{
	return lpf_code;
}

uint32_t esp_sdr_rate_hz(enum esp_sdr_rate rate)
{
	switch (rate) {
	case ESP_SDR_RATE_80MSPS:
		return 80000000U;
	case ESP_SDR_RATE_40MSPS:
		return 40000000U;
	case ESP_SDR_RATE_16MSPS:
		return 16000000U;
	default:
		return 0U;
	}
}

static uint32_t rate_bits(enum esp_sdr_rate rate)
{
	switch (rate) {
	case ESP_SDR_RATE_40MSPS:
		return DUMP_CTRL_40MSPS;
	case ESP_SDR_RATE_16MSPS:
		return DUMP_CTRL_16MSPS;
	default:
		return 0U;
	}
}

/* One capture into the bank; caller holds sdr_lock. t0 is the cycle count at arming. */
static int capture_locked(enum esp_sdr_rate rate, size_t count, uint32_t usage,
			  uint32_t *elapsed_us, uint64_t *t0)
{
	volatile uint32_t *words = __esp_sdr_bank_start;
	uint32_t saved, ctrl, start, elapsed;
	bool done;

	for (size_t j = 0; j < count; j++) {
		words[j] = SENTINEL;
	}
	lpf_apply();

	saved = REG_READ(SENSITIVE_INTERNAL_SRAM_USAGE_3_REG);
	*t0 = k_cycle_get_64();
	start = (uint32_t)*t0;
	REG_WRITE(DUMP_CTRL_REG, 0);
	REG_WRITE(DUMP_CONFIG_REG, DUMP_CONFIG_IQ);
	REG_WRITE(SENSITIVE_INTERNAL_SRAM_USAGE_3_REG,
		  (saved & ~SENSITIVE_INTERNAL_SRAM_MAC_DUMP_USAGE_M) | usage);

	ctrl = DUMP_CTRL_RUN | rate_bits(rate) | FIELD_PREP(DUMP_CTRL_COUNT, count);
	REG_WRITE(DUMP_CTRL_REG, ctrl);
	REG_WRITE(DUMP_CTRL_REG, ctrl | DUMP_CTRL_TRIGGER);
	REG_WRITE(DUMP_CTRL_REG, ctrl);
	do {
		done = (REG_READ(DUMP_CTRL_REG) & DUMP_CTRL_DONE) != 0U;
		elapsed = k_cyc_to_us_floor32(k_cycle_get_32() - start);
	} while (!done && elapsed < DUMP_TIMEOUT_US);

	REG_WRITE(DUMP_CTRL_REG, 0);
	REG_WRITE(SENSITIVE_INTERNAL_SRAM_USAGE_3_REG, saved);
	lpf_restore();

	if (!done) {
		return -ETIMEDOUT;
	}
	for (size_t j = 0; j < count; j++) {
		if (words[j] == SENTINEL) {
			return -ETIMEDOUT;
		}
	}
	*elapsed_us = elapsed;
	return 0;
}

int esp_sdr_capture(enum esp_sdr_rate rate, size_t count, struct esp_sdr_burst *burst)
{
	uint32_t usage, elapsed;
	uint64_t t0;
	int ret;

	if (esp_sdr_rate_hz(rate) == 0U || count < ESP_SDR_SAMPLES_MIN ||
	    count > ESP_SDR_SAMPLES_MAX || bank_usage(&usage) != 0) {
		return -EINVAL;
	}

	k_mutex_lock(&sdr_lock, K_FOREVER);
	ret = sdr_ready ? capture_locked(rate, count, usage, &elapsed, &t0) : -EAGAIN;
	if (ret == 0) {
		burst->words = __esp_sdr_bank_start;
		burst->count = count;
		burst->elapsed_us = elapsed;
	}
	k_mutex_unlock(&sdr_lock);
	return ret;
}

#define CIC_STAGES 3

/*
 * Third order CIC decimator over the bank, m input samples per output. Wraps
 * in 32 bits by design (only differences matter); exact while 512 * m^3 fits,
 * hence ESP_SDR_DECIM_MAX. Output scaled so the 10-bit full scale is +-32767.
 */
__attribute__((optimize("O3"))) static size_t cic(const volatile uint32_t *vwords,
						  size_t count, unsigned int m,
						  struct esp_sdr_iq16 *out, size_t max_out)
{
	const uint32_t *words = (const uint32_t *)vwords;
	int32_t ii1 = 0, ii2 = 0, ii3 = 0, qi1 = 0, qi2 = 0, qi3 = 0;
	int32_t ic[CIC_STAGES] = {0}, qc[CIC_STAGES] = {0};
	/* Output gain m^3 down to 32767 / 512: one int64 multiply per output. */
	int64_t scale = ((int64_t)64 << 32) / ((int64_t)m * m * m);
	size_t n = 0, outs = count / m;

	for (size_t k = 0; k < outs; k++) {
		const uint32_t *w = &words[k * m];

		for (unsigned int j = 0; j < m; j++) {
			/* Receive words: I in bits 19:10, Q in bits 9:0. */
			ii1 += (int32_t)(w[j] << 12) >> 22;
			qi1 += (int32_t)(w[j] << 22) >> 22;
			ii2 += ii1;
			qi2 += qi1;
			ii3 += ii2;
			qi3 += qi2;
		}
		int32_t yi = ii3, yq = qi3;

		for (int s = 0; s < CIC_STAGES; s++) {
			int32_t ti = yi - ic[s], tq = yq - qc[s];

			ic[s] = yi;
			qc[s] = yq;
			yi = ti;
			yq = tq;
		}
		/* The first outputs still see the integrators settling. */
		if (k >= CIC_STAGES && n < max_out) {
			out[n].i = (int16_t)CLAMP((yi * scale) >> 32, INT16_MIN, INT16_MAX);
			out[n].q = (int16_t)CLAMP((yq * scale) >> 32, INT16_MIN, INT16_MAX);
			n++;
		}
	}
	return n;
}

int esp_sdr_capture_decimated(enum esp_sdr_rate rate, size_t count, unsigned int m,
			      struct esp_sdr_iq16 *out, size_t max_out, size_t *n_out,
			      uint64_t *first_ns)
{
	uint32_t usage, elapsed;
	uint64_t t0;
	int ret;

	if (esp_sdr_rate_hz(rate) == 0U || count < ESP_SDR_SAMPLES_MIN ||
	    count > ESP_SDR_SAMPLES_MAX || m < ESP_SDR_DECIM_MIN || m > ESP_SDR_DECIM_MAX ||
	    count / m <= CIC_STAGES || bank_usage(&usage) != 0) {
		return -EINVAL;
	}

	k_mutex_lock(&sdr_lock, K_FOREVER);
	ret = sdr_ready ? capture_locked(rate, count, usage, &elapsed, &t0) : -EAGAIN;
	if (ret == 0) {
		/* Still under the lock: a transmit session would overwrite the bank. */
		*n_out = cic(__esp_sdr_bank_start, count, m, out, max_out);
		/* Output k is centred 1.5 (m - 1) samples before input (k + 1) m - 1. */
		*first_ns = k_cyc_to_ns_floor64(t0) +
			    ((uint64_t)(CIC_STAGES + 1U) * m - 1U - (3U * (m - 1U)) / 2U) *
				    NSEC_PER_SEC / esp_sdr_rate_hz(rate);
	}
	k_mutex_unlock(&sdr_lock);
	return ret;
}

/*
 * DAC-side trigger (dactrig() in librftest.a, minus its ramp-fill): plays
 * `words` out of the shared capture bank through the TX DAC. Swaps the
 * analog front end to TX for the duration (prepare_tx()) and always leaves
 * it back in its resting RX-ready state (prepare_rx()) before returning.
 * Caller is still responsible for RF safety (dummy load / attenuated link).
 */
uint32_t esp_sdr_tx_rate_hz(enum esp_sdr_rate rate)
{
	switch (rate) {
	case ESP_SDR_RATE_80MSPS:
		return 80000000U;
	case ESP_SDR_RATE_40MSPS:
		return 40000000U;
	default:
		return 0U;
	}
}

static uint32_t dac_t0;

/* Start one DAC burst over the first `count` words of the bank the engine owns. */
static void dac_trigger(enum esp_sdr_rate rate, size_t count)
{
	uint32_t ctrl = DUMP_CTRL_RUN | (rate == ESP_SDR_RATE_80MSPS ? DAC_CTRL_80MSPS : 0U) |
			FIELD_PREP(DUMP_CTRL_COUNT, count);

	dac_t0 = k_cycle_get_32();
	REG_WRITE(DAC_TRIG_REG, 0);
	REG_WRITE(DAC_TRIG_REG, ctrl);
	REG_WRITE(DAC_TRIG_REG, ctrl | DUMP_CTRL_TRIGGER);
	REG_WRITE(DAC_TRIG_REG, ctrl);
}

/* True once the engine reports the burst done. */
static bool dac_wait_done(void)
{
	bool done;

	do {
		done = (REG_READ(DAC_TRIG_REG) & DUMP_CTRL_DONE) != 0U;
	} while (!done && k_cyc_to_us_floor32(k_cycle_get_32() - dac_t0) < DUMP_TIMEOUT_US);
	return done;
}

static bool dac_burst(enum esp_sdr_rate rate, size_t count)
{
	dac_trigger(rate, count);
	return dac_wait_done();
}

int esp_sdr_play_for(enum esp_sdr_rate rate, const uint32_t *words, size_t count,
		     uint32_t duration_ms, uint32_t *bursts)
{
	volatile uint32_t *bank = __esp_sdr_bank_start;
	uint32_t usage, saved, n = 0;
	size_t len = count;
	int64_t end;
	bool done;
	int ret = 0;

	if (esp_sdr_tx_rate_hz(rate) == 0U || count < ESP_SDR_SAMPLES_MIN ||
	    count > ESP_SDR_SAMPLES_MAX || duration_ms > ESP_SDR_PLAY_MAX_MS ||
	    bank_usage(&usage) != 0) {
		return -EINVAL;
	}

	k_mutex_lock(&sdr_lock, K_FOREVER);
	if (!sdr_ready) {
		ret = -EAGAIN;
		goto out;
	}

	/* Each retrigger leaves a ~1.35 us hole: tile the burst over the bank to
	 * make the holes rare.
	 */
	if (duration_ms > 0U) {
		len = ESP_SDR_SAMPLES_MAX / count * count;
	}
	for (size_t j = 0; j < len; j++) {
		bank[j] = words[j % count];
	}
	count = len;

	/* Front end and bank switch once, then retrigger back to back: the
	 * switch costs ~6 ms, a burst is at most ~1 ms.
	 */
	prepare_tx();
	saved = REG_READ(SENSITIVE_INTERNAL_SRAM_USAGE_3_REG);
	REG_WRITE(SENSITIVE_INTERNAL_SRAM_USAGE_3_REG,
		  (saved & ~SENSITIVE_INTERNAL_SRAM_MAC_DUMP_USAGE_M) | usage);

	end = k_uptime_get() + duration_ms;
	do {
		done = dac_burst(rate, count);
		n++;
	} while (done && k_uptime_get() < end);

	REG_WRITE(DAC_TRIG_REG, 0);
	REG_WRITE(DUMP_CTRL_REG, 0); /* dactrig() also clears the ADC-side register */
	REG_WRITE(SENSITIVE_INTERNAL_SRAM_USAGE_3_REG, saved);
	prepare_rx(); /* back to the module's resting state */

	if (!done) {
		ret = -ETIMEDOUT;
	}
	if (bursts != NULL) {
		*bursts = n;
	}
out:
	k_mutex_unlock(&sdr_lock);
	return ret;
}

static enum esp_sdr_rate dac_rate;
static uint32_t dac_usage, dac_saved;

int esp_sdr_dac_begin(enum esp_sdr_rate rate)
{
	if (esp_sdr_tx_rate_hz(rate) == 0U || bank_usage(&dac_usage) != 0) {
		return -EINVAL;
	}
	k_mutex_lock(&sdr_lock, K_FOREVER);
	if (!sdr_ready) {
		k_mutex_unlock(&sdr_lock);
		return -EAGAIN;
	}
	dac_rate = rate;
	prepare_tx();
	dac_saved = REG_READ(SENSITIVE_INTERNAL_SRAM_USAGE_3_REG);
	return 0;
}

uint32_t *esp_sdr_dac_buf(int idx, size_t *words)
{
	*words = DUMP_BANK_SIZE / sizeof(uint32_t);
#if defined(CONFIG_ESP_SDR_TX_DAC)
	if (idx != 0) {
		return (uint32_t *)__esp_sdr_bank1_start;
	}
#endif
	return (uint32_t *)__esp_sdr_bank_start;
}

void esp_sdr_dac_start(int idx, size_t count)
{
	/* One-hot: the engine gets this bank, the CPUs keep every other one. */
	uint32_t usage = idx == 0 ? dac_usage : BIT(1) << SENSITIVE_INTERNAL_SRAM_MAC_DUMP_USAGE_S;

	REG_WRITE(SENSITIVE_INTERNAL_SRAM_USAGE_3_REG,
		  (dac_saved & ~SENSITIVE_INTERNAL_SRAM_MAC_DUMP_USAGE_M) | usage);
	dac_trigger(dac_rate, count);
}

bool esp_sdr_dac_wait(void)
{
	return dac_wait_done();
}

IRAM_ATTR void esp_sdr_dac_select(int idx)
{
	uint32_t usage = idx == 0 ? dac_usage : BIT(1) << SENSITIVE_INTERNAL_SRAM_MAC_DUMP_USAGE_S;

	REG_WRITE(SENSITIVE_INTERNAL_SRAM_USAGE_3_REG,
		  (dac_saved & ~SENSITIVE_INTERNAL_SRAM_MAC_DUMP_USAGE_M) | usage);
}

void esp_sdr_dac_loop(int idx, size_t count)
{
	uint32_t ctrl = DUMP_CTRL_RUN | (dac_rate == ESP_SDR_RATE_80MSPS ? DAC_CTRL_80MSPS : 0U) |
			FIELD_PREP(DUMP_CTRL_COUNT, count);

	esp_sdr_dac_select(idx);
	REG_WRITE(DAC_TRIG_REG, 0);
	REG_WRITE(DAC_TRIG_REG, ctrl);
	/* Held, not pulsed: the engine wraps instead of stopping after one pass. */
	REG_WRITE(DAC_TRIG_REG, ctrl | DUMP_CTRL_TRIGGER);
}

IRAM_ATTR void esp_sdr_dac_halt(void)
{
	REG_WRITE(DAC_TRIG_REG, 0);
}

void esp_sdr_dac_end(void)
{
	REG_WRITE(DAC_TRIG_REG, 0);
	REG_WRITE(DUMP_CTRL_REG, 0);
	REG_WRITE(SENSITIVE_INTERNAL_SRAM_USAGE_3_REG, dac_saved);
	prepare_rx();
	k_mutex_unlock(&sdr_lock);
}

int esp_sdr_play(enum esp_sdr_rate rate, const uint32_t *words, size_t count)
{
	return esp_sdr_play_for(rate, words, count, 0, NULL);
}

#define SWEEP_TWO_PI 6.28318530717958647692

/*
 * Fill bank[0..n) with a chirp starting at t (s): frequency f0 + k t. Phase
 * is taken from absolute time, so segments line up across the synthesis
 * gaps; per sample only a complex rotation.
 */
static void sweep_fill(volatile uint32_t *bank, size_t n, double t, double fs, double f0,
		       double k, float amp)
{
	double phase = fmod(SWEEP_TWO_PI * (f0 * t + 0.5 * k * t * t), SWEEP_TWO_PI);
	double f = f0 + k * t;
	float zr = cosf((float)phase), zi = sinf((float)phase);
	float wr = cosf((float)(SWEEP_TWO_PI * f / fs)), wi = sinf((float)(SWEEP_TWO_PI * f / fs));
	float cr = cosf((float)(SWEEP_TWO_PI * k / (fs * fs)));
	float ci = sinf((float)(SWEEP_TWO_PI * k / (fs * fs)));

	for (size_t j = 0; j < n; j++) {
		float tr;

		float vi = amp * zr, vq = amp * zi;

		/* Inline rounding: lroundf() is a library call and dominated the loop. */
		bank[j] = esp_sdr_tx_word((int16_t)CLAMP((int)(vi + (vi < 0.0f ? -0.5f : 0.5f)), -511, 511),
					  (int16_t)CLAMP((int)(vq + (vq < 0.0f ? -0.5f : 0.5f)), -511, 511));
		tr = zr * wr - zi * wi;
		zi = zr * wi + zi * wr;
		zr = tr;
		tr = wr * cr - wi * ci;
		wi = wr * ci + wi * cr;
		wr = tr;
	}
}

int esp_sdr_sweep(enum esp_sdr_rate rate, float f0_hz, float f1_hz, uint32_t duration_ms,
		  int amp, struct esp_sdr_sweep_stats *stats)
{
	volatile uint32_t *bank = __esp_sdr_bank_start;
	struct esp_sdr_sweep_stats st = {0};
	double fs = (double)esp_sdr_tx_rate_hz(rate);
	double T = (double)duration_ms / 1e3;
	double k = (double)(f1_hz - f0_hz) / T;
	uint32_t usage, saved;
	uint64_t start;
	int ret = 0;

	if (fs == 0.0 || duration_ms == 0U || duration_ms > ESP_SDR_PLAY_MAX_MS ||
	    (double)fabsf(f0_hz) >= fs / 2 || (double)fabsf(f1_hz) >= fs / 2 || amp < 1 ||
	    amp > 511 || bank_usage(&usage) != 0) {
		return -EINVAL;
	}

	k_mutex_lock(&sdr_lock, K_FOREVER);
	if (!sdr_ready) {
		ret = -EAGAIN;
		goto out;
	}
	prepare_tx();
	saved = REG_READ(SENSITIVE_INTERNAL_SRAM_USAGE_3_REG);

	start = k_cycle_get_64();
	for (;;) {
		double t = (double)k_cyc_to_ns_floor64(k_cycle_get_64() - start) / 1e9;
		size_t n = MIN((size_t)ESP_SDR_SAMPLES_MAX, (size_t)((T - t) * fs));

		if (t >= T || n < ESP_SDR_SAMPLES_MIN) {
			break;
		}
		/* The engine only sees bank writes made while the CPU owns the bank. */
		REG_WRITE(SENSITIVE_INTERNAL_SRAM_USAGE_3_REG, saved);
		sweep_fill(bank, n, t, fs, f0_hz, k, (float)amp);
		REG_WRITE(SENSITIVE_INTERNAL_SRAM_USAGE_3_REG,
			  (saved & ~SENSITIVE_INTERNAL_SRAM_MAC_DUMP_USAGE_M) | usage);
		if (!dac_burst(rate, n)) {
			ret = -ETIMEDOUT;
			break;
		}
		st.segments++;
		st.samples += n;
	}
	st.elapsed_ms = (uint32_t)(k_cyc_to_ns_floor64(k_cycle_get_64() - start) / 1000000U);

	REG_WRITE(DAC_TRIG_REG, 0);
	REG_WRITE(DUMP_CTRL_REG, 0);
	REG_WRITE(SENSITIVE_INTERNAL_SRAM_USAGE_3_REG, saved);
	prepare_rx();
	if (stats != NULL) {
		*stats = st;
	}
out:
	k_mutex_unlock(&sdr_lock);
	return ret;
}

size_t esp_sdr_pack_iq8(const uint32_t *words, size_t count, uint8_t *out)
{
	for (size_t j = 0; j < count; j++) {
		uint32_t w = words[j];

		out[2 * j] = (uint8_t)(w >> 12);
		out[2 * j + 1] = (uint8_t)(w >> 2);
	}
	return 2 * count;
}

size_t esp_sdr_pack_iq10(const uint32_t *words, size_t count, uint8_t *out)
{
	uint8_t *p = out;

	for (size_t j = 0; j < count; j += 2, p += 5) {
		uint32_t a = words[j] & GENMASK(19, 0);
		uint32_t b = j + 1 < count ? words[j + 1] & GENMASK(19, 0) : 0U;

		p[0] = (uint8_t)a;
		p[1] = (uint8_t)(a >> 8);
		p[2] = (uint8_t)((a >> 16) | (b << 4));
		if (j + 1 < count) {
			p[3] = (uint8_t)(b >> 4);
			p[4] = (uint8_t)(b >> 12);
		}
	}
	return (count * 20U + 7U) / 8U;
}
