/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The crystal calibration in Zephyr settings, key "esp_sdr/cal", with a version word. The
 * app picks the backend; the apps here use the file backend on a FAT volume in the
 * storage partition (apps/common/calstore.conf and calstore.overlay).
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_cal.h>

#include "esp_sdr_priv.h"

LOG_MODULE_DECLARE(esp_sdr, CONFIG_ESP_SDR_LOG_LEVEL);

#define CAL_KEY     "esp_sdr/cal"
#define CAL_VERSION 1U

struct cal_entry {
	uint32_t version;
	struct esp_sdr_cal_record rec;
};

static K_MUTEX_DEFINE(cal_lock);
static struct cal_entry loaded;
static bool have;

static int cal_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	struct cal_entry e;
	ssize_t n;

	if (strcmp(name, "cal") != 0) {
		return -ENOENT;
	}
	if (len != sizeof(e)) {
		return 0;
	}
	n = read_cb(cb_arg, &e, sizeof(e));
	if (n == (ssize_t)sizeof(e) && e.version == CAL_VERSION) {
		loaded = e;
		have = true;
	}
	return n < 0 ? (int)n : 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(esp_sdr, "esp_sdr", NULL, cal_set, NULL, NULL);

int esp_sdr_cal_load(struct esp_sdr_cal_record *rec)
{
	int ret;

	k_mutex_lock(&cal_lock, K_FOREVER);
	have = false;
	ret = settings_subsys_init();
	if (ret == 0) {
		ret = settings_load_subtree("esp_sdr");
	}
	if (ret == 0) {
		if (have) {
			*rec = loaded.rec;
		} else {
			ret = -ENOENT;
		}
	}
	k_mutex_unlock(&cal_lock);
	return ret;
}

int esp_sdr_cal_save(const struct esp_sdr_cal_record *rec)
{
	struct cal_entry e = {.version = CAL_VERSION, .rec = *rec};
	int ret;

	k_mutex_lock(&cal_lock, K_FOREVER);
	ret = settings_subsys_init();
	if (ret == 0) {
		ret = settings_save_one(CAL_KEY, &e, sizeof(e));
	}
	k_mutex_unlock(&cal_lock);
	return ret;
}

int esp_sdr_cal_clear(void)
{
	int ret;

	k_mutex_lock(&cal_lock, K_FOREVER);
	ret = settings_subsys_init();
	if (ret == 0) {
		ret = settings_delete(CAL_KEY);
	}
	k_mutex_unlock(&cal_lock);
	return ret;
}

void esp_sdr_cal_apply_stored(void)
{
	struct esp_sdr_cal_record rec;
	int ret = esp_sdr_cal_load(&rec);

	if (ret == 0 && rec.ppb >= -ESP_SDR_FREQ_CORR_MAX_PPB &&
	    rec.ppb <= ESP_SDR_FREQ_CORR_MAX_PPB) {
		/* Called from esp_sdr_init() with the lock held (k_mutex nests): no retune yet. */
		(void)esp_sdr_set_freq_corr_model(&rec);
		LOG_INF("crystal correction %d ppb (stored, cell %d, %u kHz, %u points)",
			esp_sdr_corr_ppb, rec.cell_id, rec.carrier_khz, rec.points);
	} else if (ret != -ENOENT) {
		LOG_WRN("no stored crystal correction: %d", ret);
	}
}
