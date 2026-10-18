/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Loop replay through the modem DAC engine on the ESP32-C5 (esp_sdr_replay.h).
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_paced.h>
#include <esp_sdr/esp_sdr_replay.h>

#include "esp_sdr_priv.h"

/* Upstream's scale for replay: the full scale the tone stop leaves clips. */
#define SCALE_DEFAULT 15U

static struct {
	bool on;
	bool endless;
	uint32_t ctrl, saved_usage, saved_scale, t0;
} rp;

uint32_t *esp_sdr_replay_buf(size_t *words)
{
	*words = ESP_SDR_REPLAY_MAX;
	return __esp_sdr_bank_start;
}

int esp_sdr_replay_start(const struct esp_sdr_replay_cfg *cfg)
{
	uint32_t usage, scale;
	unsigned int key;
	int ret;

	if (cfg->count == 0U || cfg->count > ESP_SDR_REPLAY_MAX || cfg->passes > 255U ||
	    (cfg->rate != ESP_SDR_RATE_80MSPS && cfg->rate != ESP_SDR_RATE_40MSPS) ||
	    esp_sdr_bank_usage(&usage) != 0) {
		return -EINVAL;
	}
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
	rp.ctrl = DAC_TRIG_RUN | FIELD_PREP(DAC_TRIG_COUNT, cfg->count) |
		  (cfg->rate == ESP_SDR_RATE_40MSPS ? DAC_TRIG_HALF : 0U) | cfg->ctrl_extra;
	if (cfg->passes != 1U) {
		/* Repeat count 0 with LOOP plays until RUN clears. */
		rp.ctrl |= DAC_TRIG_LOOP | FIELD_PREP(DAC_TRIG_REPEAT, cfg->passes);
	}
	rp.endless = cfg->passes == 0U;
	rp.saved_scale = sys_read32(DAC_SCALE_REG);
	sys_write32((rp.saved_scale & ~DAC_SCALE_M) | (scale << 16) | (scale << 8), DAC_SCALE_REG);
	key = irq_lock();
	/* Dump clocks forced and the bank handed to the engine, as for a capture. */
	rp.saved_usage = esp_sdr_dump_arm(usage);
	sys_write32(0, DAC_TRIG_REG);
	rp.t0 = esp_sdr_paced_cycles();
	sys_write32(rp.ctrl, DAC_TRIG_REG);
	irq_unlock(key);
	rp.on = true;
	return 0;
}

int esp_sdr_replay_wait(uint32_t timeout_us, uint32_t *cycles)
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
		dt = esp_sdr_paced_cycles() - rp.t0;
	} while (!done && dt < limit);
	irq_unlock(key);
	if (cycles != NULL) {
		*cycles = dt;
	}
	return done ? 0 : -ETIMEDOUT;
}

int esp_sdr_replay_restart(void)
{
	if (!rp.on) {
		return -EINVAL;
	}
	sys_write32(rp.ctrl & ~DAC_TRIG_RUN, DAC_TRIG_REG);
	rp.t0 = esp_sdr_paced_cycles();
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
	sys_write32(rp.ctrl & ~DAC_TRIG_RUN, DAC_TRIG_REG);
	sys_write32(0, DAC_TRIG_REG);
	esp_sdr_dump_disarm(rp.saved_usage);
	sys_write32(rp.saved_scale, DAC_SCALE_REG);
	esp_sdr_tx_chain_off();
	rp.on = false;
	k_mutex_unlock(&esp_sdr_lock);
}
