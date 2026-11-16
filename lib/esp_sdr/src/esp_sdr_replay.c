/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Loop replay through the modem DAC engine on the ESP32-C5 and the ESP32-S31
 * (esp_sdr_replay.h).
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_replay.h>

#include "esp_sdr_priv.h"

#if defined(CONFIG_SOC_SERIES_ESP32S31)
#include <esp_cpu.h>

/* The scale the tone stop leaves (0xff) is kept: the S31's is no plain gain (hardware-quirks). */
#define SCALE_DEFAULT 0U

/* The reader's rate codes in DUMP_MODE: 80, 40 and 20 MS/s; others were not measured. */
static int rate_code(enum esp_sdr_rate rate)
{
	switch (rate) {
	case ESP_SDR_RATE_80MSPS:
		return 0;
	case ESP_SDR_RATE_40MSPS:
		return 1;
	case ESP_SDR_RATE_20MSPS:
		return 3;
	default:
		return -1;
	}
}

static inline uint32_t cycles(void)
{
	return (uint32_t)esp_cpu_get_cycle_count();
}
#else
#include <esp_sdr/esp_sdr_paced.h>

/* Upstream's scale for replay: the full scale the tone stop leaves clips. */
#define SCALE_DEFAULT 15U

/* The trigger's rate bit: none is 80 MS/s, HALF 40. */
static int rate_code(enum esp_sdr_rate rate)
{
	switch (rate) {
	case ESP_SDR_RATE_80MSPS:
		return 0;
	case ESP_SDR_RATE_40MSPS:
		return 1;
	default:
		return -1;
	}
}

static inline uint32_t cycles(void)
{
	return esp_sdr_paced_cycles();
}
#endif

/* The power step's range, as esp_sdr_tx_paced_start(). */
#define POWER_MAX 15U

static struct {
	bool on;
	bool endless;
	uint32_t ctrl, saved_usage, saved_scale, t0;
} rp;

uint32_t *esp_sdr_replay_buf(size_t *words)
{
	*words = ESP_SDR_REPLAY_MAX;
#if defined(CONFIG_SOC_SERIES_ESP32S31)
	return __esp_sdr_group2_start;
#else
	return __esp_sdr_bank_start;
#endif
}

int esp_sdr_replay_start(const struct esp_sdr_replay_cfg *cfg)
{
	int code = rate_code(cfg->rate);
	uint32_t scale;
#if !defined(CONFIG_SOC_SERIES_ESP32S31)
	uint32_t usage;
	unsigned int key;
#endif
	int ret;

	if (cfg->count == 0U || cfg->count > ESP_SDR_REPLAY_MAX || cfg->passes > 255U ||
	    code < 0 || cfg->power > POWER_MAX) {
		return -EINVAL;
	}
#if !defined(CONFIG_SOC_SERIES_ESP32S31)
	if (esp_sdr_bank_usage(&usage) != 0) {
		return -EINVAL;
	}
#endif
	if (!esp_sdr_ready) {
		return -EAGAIN;
	}
	k_mutex_lock(&esp_sdr_lock, K_FOREVER);
	if (rp.on) {
		k_mutex_unlock(&esp_sdr_lock);
		return -EBUSY;
	}
	ret = esp_sdr_tx_chain_on(cfg->power);
	if (ret != 0) {
		k_mutex_unlock(&esp_sdr_lock);
		return ret;
	}
	scale = cfg->scale != 0U ? cfg->scale : SCALE_DEFAULT;
	rp.ctrl = DAC_TRIG_RUN | FIELD_PREP(DAC_TRIG_COUNT, cfg->count) | cfg->ctrl_extra;
	if (cfg->passes != 1U) {
		/* Repeat count 0 with LOOP plays until RUN clears. */
		rp.ctrl |= DAC_TRIG_LOOP | FIELD_PREP(DAC_TRIG_REPEAT, cfg->passes);
	}
	rp.endless = cfg->passes == 0U;
	rp.saved_scale = sys_read32(DAC_SCALE_REG);
	if (scale != 0U) {
		sys_write32((rp.saved_scale & ~DAC_SCALE_M) | (scale << 16) | (scale << 8),
			    DAC_SCALE_REG);
	}
#if defined(CONFIG_SOC_SERIES_ESP32S31)
	ret = esp_sdr_s31_dac_start((unsigned int)code, rp.ctrl, &rp.t0);
	if (ret != 0) {
		sys_write32(rp.saved_scale, DAC_SCALE_REG);
		esp_sdr_tx_chain_off();
		k_mutex_unlock(&esp_sdr_lock);
		return ret;
	}
#else
	if (code != 0) {
		rp.ctrl |= DAC_TRIG_HALF;
	}
	key = irq_lock();
	/* Dump clocks forced and the bank handed to the engine, as for a capture. */
	rp.saved_usage = esp_sdr_dump_arm(usage);
	sys_write32(0, DAC_TRIG_REG);
	rp.t0 = cycles();
	sys_write32(rp.ctrl, DAC_TRIG_REG);
	irq_unlock(key);
#endif
	rp.on = true;
	return 0;
}

int esp_sdr_replay_wait(uint32_t timeout_us, uint32_t *cycles_out)
{
	uint32_t limit = timeout_us * (ESP_SDR_CPU_HZ / 1000000U), dt;
	unsigned int key;
	bool done;

	if (!rp.on || rp.endless) {
		return -EINVAL;
	}
	/* Polled with interrupts locked so the cycle count is the engine's. */
	key = irq_lock();
	do {
		done = (sys_read32(DAC_TRIG_REG) & DAC_TRIG_DONE) != 0U;
		dt = cycles() - rp.t0;
	} while (!done && dt < limit);
	irq_unlock(key);
	if (cycles_out != NULL) {
		*cycles_out = dt;
	}
	return done ? 0 : -ETIMEDOUT;
}

int esp_sdr_replay_restart(void)
{
	if (!rp.on) {
		return -EINVAL;
	}
	sys_write32(rp.ctrl & ~DAC_TRIG_RUN, DAC_TRIG_REG);
	rp.t0 = cycles();
	sys_write32(rp.ctrl, DAC_TRIG_REG);
	return 0;
}

uint32_t esp_sdr_replay_ctrl(void)
{
	return sys_read32(DAC_TRIG_REG);
}

bool esp_sdr_replay_active(void)
{
	return rp.on;
}

void esp_sdr_replay_stop(void)
{
	if (!rp.on) {
		return;
	}
#if defined(CONFIG_SOC_SERIES_ESP32S31)
	esp_sdr_s31_dac_stop();
#else
	sys_write32(rp.ctrl & ~DAC_TRIG_RUN, DAC_TRIG_REG);
	sys_write32(0, DAC_TRIG_REG);
	esp_sdr_dump_disarm(rp.saved_usage);
#endif
	sys_write32(rp.saved_scale, DAC_SCALE_REG);
	esp_sdr_tx_chain_off();
	rp.on = false;
	k_mutex_unlock(&esp_sdr_lock);
}
