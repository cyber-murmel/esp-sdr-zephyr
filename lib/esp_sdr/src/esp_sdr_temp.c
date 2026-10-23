/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The chip's die temperature (devicetree alias die-temp0) for the crystal calibration.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>

#include <esp_sdr/esp_sdr.h>

/* Tuning reads it on every retune: a reading lasts this long. */
#define TEMP_MAX_AGE_MS 1000

static const struct device *const temp_dev = DEVICE_DT_GET(DT_ALIAS(die_temp0));
static K_MUTEX_DEFINE(temp_lock);
static int64_t temp_at = INT64_MIN;
static int16_t temp_dc;
static int temp_err = -EAGAIN;

int esp_sdr_get_temp_dc(int16_t *dc)
{
	struct sensor_value v;
	int64_t now = k_uptime_get();
	int ret;

	k_mutex_lock(&temp_lock, K_FOREVER);
	if (temp_at == INT64_MIN || now - temp_at >= TEMP_MAX_AGE_MS) {
		if (!device_is_ready(temp_dev)) {
			temp_err = -ENODEV;
		} else {
			temp_err = sensor_sample_fetch(temp_dev);
			if (temp_err == 0) {
				temp_err = sensor_channel_get(temp_dev, SENSOR_CHAN_DIE_TEMP, &v);
			}
			if (temp_err == 0) {
				temp_dc = (int16_t)(v.val1 * 10 + v.val2 / 100000);
			}
		}
		temp_at = now;
	}
	ret = temp_err;
	if (ret == 0) {
		*dc = temp_dc;
	}
	k_mutex_unlock(&temp_lock);
	return ret;
}
