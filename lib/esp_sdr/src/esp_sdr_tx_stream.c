/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Transmit front end: serializes callers of the DAC stream and keeps
 * statistics.
 */

#include <errno.h>
#include <stdbool.h>

#include <zephyr/kernel.h>

#include <esp_sdr/esp_sdr_tx.h>

#include "esp_sdr_priv.h"

static K_MUTEX_DEFINE(tx_lock);
static bool active;
static struct esp_sdr_tx_stats stats;

int esp_sdr_tx_start(uint64_t freq_hz, uint32_t rate_hz)
{
	int ret;

	if (rate_hz == 0U) {
		return -EINVAL;
	}
	k_mutex_lock(&tx_lock, K_FOREVER);
	if (active) {
		ret = -EALREADY;
	} else {
		ret = esp_sdr_dac_stream_start(freq_hz, rate_hz);
		if (ret == 0) {
			active = true;
			stats = (struct esp_sdr_tx_stats){0};
		}
	}
	k_mutex_unlock(&tx_lock);
	return ret;
}

int esp_sdr_tx_write(const struct esp_sdr_iq16 *samples, size_t count)
{
	int ret;

	k_mutex_lock(&tx_lock, K_FOREVER);
	if (!active) {
		ret = -ENOTCONN;
	} else {
		ret = esp_sdr_dac_stream_write(samples, count);
		stats.writes++;
		if (ret < 0) {
			stats.errors++;
		} else {
			stats.samples += (uint64_t)ret;
		}
	}
	k_mutex_unlock(&tx_lock);
	return ret;
}

int esp_sdr_tx_stop(void)
{
	int ret = 0;

	k_mutex_lock(&tx_lock, K_FOREVER);
	if (active) {
		ret = esp_sdr_dac_stream_stop();
		active = false;
	}
	k_mutex_unlock(&tx_lock);
	return ret;
}

bool esp_sdr_tx_active(void)
{
	return active;
}

void esp_sdr_tx_get_stats(struct esp_sdr_tx_stats *out)
{
	k_mutex_lock(&tx_lock, K_FOREVER);
	*out = stats;
	k_mutex_unlock(&tx_lock);
}
