/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The protocol's crystal calibration requests on the library (esdr_cal.h).
 */

#include <errno.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_cal.h>

#include "esdr_cal.h"
#include "esdr_proto.h"

BUILD_ASSERT(ESDR_CAL_POINTS == ESP_SDR_CAL_POINTS);
BUILD_ASSERT(ESDR_TEMP_UNKNOWN == ESP_SDR_CAL_TEMP_UNKNOWN);

/* What storing the last ESDR_REQ_SET_CAL gave. */
static int16_t last_store;

static void rec_out(const struct esp_sdr_cal_record *rec, struct esdr_cal_rec *out)
{
	*out = (struct esdr_cal_rec){
		.ppb = rec->ppb,
		.spread_ppb = rec->spread_ppb,
		.carrier_khz = rec->carrier_khz,
		.cell_id = rec->cell_id,
		.estimates = rec->estimates,
		.temp_dc = rec->temp_dc,
		.points = MIN(rec->points, (uint8_t)ESDR_CAL_POINTS),
	};
	for (uint8_t i = 0; i < out->points; i++) {
		out->point[i].temp_dc = rec->point[i].temp_dc;
		out->point[i].ppb = rec->point[i].ppb;
	}
}

static void rec_in(const struct esdr_cal_rec *in, struct esp_sdr_cal_record *rec)
{
	*rec = (struct esp_sdr_cal_record){
		.ppb = in->ppb,
		.spread_ppb = in->spread_ppb,
		.carrier_khz = in->carrier_khz,
		.cell_id = in->cell_id,
		.estimates = in->estimates,
		.temp_dc = in->temp_dc,
		.points = MIN(in->points, (uint8_t)ESP_SDR_CAL_POINTS),
	};
	for (uint8_t i = 0; i < rec->points; i++) {
		rec->point[i].temp_dc = in->point[i].temp_dc;
		rec->point[i].ppb = in->point[i].ppb;
	}
}

int esdr_cal_get(uint8_t *buf, size_t max)
{
	struct esp_sdr_cal_record rec, stored;
	struct esdr_cal c = {0};
	int16_t dc;

	esp_sdr_get_freq_corr_model(&rec);
	c.corr_ppb = esp_sdr_get_freq_corr_ppb();
	c.lo_residual_hz = esp_sdr_get_lo_residual_hz();
	c.temp_dc = esp_sdr_get_temp_dc(&dc) == 0 ? dc : ESDR_TEMP_UNKNOWN;
	c.stored = (int16_t)esp_sdr_cal_load(&stored);
	c.last_store = last_store;
	rec_out(&rec, &c.rec);
	memcpy(buf, &c, MIN(max, sizeof(c)));
	return (int)MIN(max, sizeof(c));
}

/* -ERANGE from the library is applied, the VCO outside its window (esp_sdr_set_freq()). */
static int applied(int ret)
{
	return ret == -ERANGE ? 0 : ret;
}

int esdr_cal_set(uint16_t op, const uint8_t *data, size_t len)
{
	struct esp_sdr_cal_record rec = {.cell_id = -1, .temp_dc = ESP_SDR_CAL_TEMP_UNKNOWN};
	int ret;

	switch (op) {
	case ESDR_CAL_ADD: {
		struct esdr_cal_meas m;

		if (len != sizeof(m)) {
			return -EINVAL;
		}
		memcpy(&m, data, sizeof(m));
		rec.ppb = m.ppb;
		rec.spread_ppb = m.spread_ppb;
		rec.carrier_khz = m.carrier_khz;
		rec.cell_id = m.cell_id;
		rec.estimates = m.estimates;
		rec.temp_dc = m.temp_dc;
		break;
	}
	case ESDR_CAL_SET:
		if (len != 4U) {
			return -EINVAL;
		}
		rec.ppb = (int32_t)sys_get_le32(data);
		break;
	case ESDR_CAL_CLEAR:
		if (len != 0U) {
			return -EINVAL;
		}
		ret = esp_sdr_set_freq_corr_ppb(0);
		last_store = (int16_t)esp_sdr_cal_clear();
		return applied(ret);
	case ESDR_CAL_LOAD: {
		struct esdr_cal_rec in;

		if (len != sizeof(in)) {
			return -EINVAL;
		}
		memcpy(&in, data, sizeof(in));
		if (in.ppb < -ESP_SDR_FREQ_CORR_MAX_PPB || in.ppb > ESP_SDR_FREQ_CORR_MAX_PPB) {
			return -EINVAL;
		}
		rec_in(&in, &rec);
		ret = esp_sdr_set_freq_corr_model(&rec);
		if (ret != 0 && ret != -ERANGE) {
			return ret;
		}
		last_store = (int16_t)esp_sdr_cal_save(&rec);
		return 0;
	}
	default:
		return -EINVAL;
	}
	ret = esp_sdr_cal_update(&rec, NULL);
	if (ret == -EINVAL) {
		return ret;
	}
	last_store = (int16_t)ret;
	return 0;
}
