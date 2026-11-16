/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file
 * @brief ESP-SDR crystal calibration: measured against an LTE cell, kept in flash.
 *
 * esp_sdr_lte_calibrate() measures the crystal's error from a base station's carrier
 * (include/esp_sdr/esp_sdr_lte.h does the signal processing) and applies it with
 * esp_sdr_set_freq_corr_ppb(). With CONFIG_ESP_SDR_CALSTORE the result lives in the
 * storage partition and esp_sdr_init() applies it at every start.
 */

#ifndef ESP_SDR_ESP_SDR_CAL_H_
#define ESP_SDR_ESP_SDR_CAL_H_

#include <errno.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup esp_sdr_cal
 * @{
 */

/** No temperature known for a record. */
#define ESP_SDR_CAL_TEMP_UNKNOWN INT16_MIN

/** Calibration points (temperature, error) a record keeps for the temperature model. */
#define ESP_SDR_CAL_POINTS 8

/** A measurement replaces a point this close in temperature, 0.1 degC. */
#define ESP_SDR_CAL_POINT_MERGE_DC 20

/** The model is not extrapolated further than this beyond its points, 0.1 degC. */
#define ESP_SDR_CAL_EXTRAPOLATE_DC 100

/** The points must span this much for a quadratic fit (with 3 or more), 0.1 degC. */
#define ESP_SDR_CAL_QUADRATIC_SPAN_DC 100

/** The crystal's error measured at one temperature. */
struct esp_sdr_cal_point {
	/** Chip temperature in 0.1 degC. */
	int16_t temp_dc;
	/** Reserved, 0. */
	int16_t reserved;
	/** The crystal's error in ppb. */
	int32_t ppb;
};

/** One calibration, as stored, with the points of the temperature model. */
struct esp_sdr_cal_record {
	/** The crystal's error in ppb, positive when the LO runs high. */
	int32_t ppb;
	/** Spread of the estimates it was made of, ppb (0 when set by hand). */
	int32_t spread_ppb;
	/** Carrier it was measured on, kHz (0 when set by hand). */
	uint32_t carrier_khz;
	/** Physical cell id of that carrier, or -1. */
	int16_t cell_id;
	/** Estimates combined. */
	uint16_t estimates;
	/** Chip temperature in 0.1 degC, or ESP_SDR_CAL_TEMP_UNKNOWN. */
	int16_t temp_dc;
	/** Reserved, 0. */
	uint16_t reserved;
	/** Valid entries of @ref point, oldest first. */
	uint8_t points;
	/** Reserved, 0. */
	uint8_t reserved2[3];
	/** Measurements at different temperatures: the model esp_sdr_cal_eval() fits. */
	struct esp_sdr_cal_point point[ESP_SDR_CAL_POINTS];
};

/**
 * @brief Add a measurement to a record's points.
 *
 * A point within ESP_SDR_CAL_POINT_MERGE_DC of @p temp_dc is replaced; with all points in
 * use the oldest goes. The new point is the newest.
 *
 * @param rec Record.
 * @param temp_dc Chip temperature in 0.1 degC.
 * @param ppb The crystal's error measured there.
 */
static inline void esp_sdr_cal_add_point(struct esp_sdr_cal_record *rec, int16_t temp_dc,
					 int32_t ppb)
{
	uint8_t drop = ESP_SDR_CAL_POINTS;

	for (uint8_t i = 0; i < rec->points; i++) {
		int32_t d = (int32_t)rec->point[i].temp_dc - temp_dc;

		if (d <= ESP_SDR_CAL_POINT_MERGE_DC && d >= -ESP_SDR_CAL_POINT_MERGE_DC) {
			drop = i;
			break;
		}
	}
	if (drop == ESP_SDR_CAL_POINTS && rec->points == ESP_SDR_CAL_POINTS) {
		drop = 0;
	}
	if (drop < ESP_SDR_CAL_POINTS) {
		for (uint8_t i = drop; i + 1U < rec->points; i++) {
			rec->point[i] = rec->point[i + 1U];
		}
		rec->points--;
	}
	rec->point[rec->points].temp_dc = temp_dc;
	rec->point[rec->points].reserved = 0;
	rec->point[rec->points].ppb = ppb;
	rec->points++;
}

/**
 * @brief The crystal's error at a temperature, from a record's points.
 *
 * One point gives a constant, two the line through them, three or more a least-squares
 * quadratic when they span ESP_SDR_CAL_QUADRATIC_SPAN_DC (else a least-squares line). The
 * temperature is clamped to ESP_SDR_CAL_EXTRAPOLATE_DC beyond the points.
 *
 * @param rec Record.
 * @param temp_dc Chip temperature in 0.1 degC.
 * @param ppb Set to the error there.
 * @retval 0 on success.
 * @retval -ENOENT if the record has no points.
 */
static inline int esp_sdr_cal_eval(const struct esp_sdr_cal_record *rec, int16_t temp_dc,
				   int32_t *ppb)
{
	uint8_t n = rec->points > ESP_SDR_CAL_POINTS ? ESP_SDR_CAL_POINTS : rec->points;
	int32_t lo = INT32_MAX, hi = INT32_MIN;
	double mean_t = 0.0, mean_y = 0.0, t, y;

	if (n == 0U) {
		return -ENOENT;
	}
	for (uint8_t i = 0; i < n; i++) {
		lo = rec->point[i].temp_dc < lo ? rec->point[i].temp_dc : lo;
		hi = rec->point[i].temp_dc > hi ? rec->point[i].temp_dc : hi;
		mean_t += rec->point[i].temp_dc;
		mean_y += rec->point[i].ppb;
	}
	mean_t /= n;
	mean_y /= n;
	t = temp_dc;
	if (t < (double)lo - ESP_SDR_CAL_EXTRAPOLATE_DC) {
		t = (double)lo - ESP_SDR_CAL_EXTRAPOLATE_DC;
	} else if (t > (double)hi + ESP_SDR_CAL_EXTRAPOLATE_DC) {
		t = (double)hi + ESP_SDR_CAL_EXTRAPOLATE_DC;
	}
	/* Centred and in units of 10 degC, for a well conditioned fit. */
	double x = (t - mean_t) / 100.0;
	double s2 = 0.0, s3 = 0.0, s4 = 0.0, sy = 0.0, sxy = 0.0, sx2y = 0.0;

	for (uint8_t i = 0; i < n; i++) {
		double xi = (rec->point[i].temp_dc - mean_t) / 100.0;
		double yi = rec->point[i].ppb - mean_y;

		s2 += xi * xi;
		s3 += xi * xi * xi;
		s4 += xi * xi * xi * xi;
		sy += yi;
		sxy += xi * yi;
		sx2y += xi * xi * yi;
	}
	y = mean_y;
	if (n >= 3U && hi - lo >= ESP_SDR_CAL_QUADRATIC_SPAN_DC) {
		/* y - mean = a + b x + c x^2 with sum x = 0: the normal equations by Cramer's rule. */
		double m[3][3] = {{n, 0.0, s2}, {0.0, s2, s3}, {s2, s3, s4}};
		double r[3] = {sy, sxy, sx2y};
		double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
			     m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
			     m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);

		if (det > 1e-9 || det < -1e-9) {
			double c[3];

			for (int k = 0; k < 3; k++) {
				double a[3][3];

				for (int i = 0; i < 3; i++) {
					for (int j = 0; j < 3; j++) {
						a[i][j] = j == k ? r[i] : m[i][j];
					}
				}
				c[k] = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
					a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
					a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0])) /
				       det;
			}
			y += c[0] + c[1] * x + c[2] * x * x;
			*ppb = (int32_t)(y + (y < 0.0 ? -0.5 : 0.5));
			return 0;
		}
	}
	if (n >= 2U && s2 > 0.0) {
		y += sxy / s2 * x;
	}
	*ppb = (int32_t)(y + (y < 0.0 ? -0.5 : 0.5));
	return 0;
}

/**
 * @brief Correct tuning with a record's temperature model.
 *
 * With points, the correction follows the chip's temperature: every retune evaluates the
 * model at the die temperature (esp_sdr_get_temp_dc()). Without points, or without a
 * temperature, the record's ppb is the correction. esp_sdr_set_freq_corr_ppb() drops the
 * model again. Retunes now.
 *
 * @param rec Record (copied).
 * @retval 0 on success, also before esp_sdr_init().
 * @retval -ERANGE as for esp_sdr_set_freq().
 */
int esp_sdr_set_freq_corr_model(const struct esp_sdr_cal_record *rec);

/**
 * @brief The calibration in force: the record esp_sdr_set_freq_corr_model() took, or for a
 * constant correction (esp_sdr_set_freq_corr_ppb()) a record without points of that ppb.
 *
 * @param rec Filled with it.
 */
void esp_sdr_get_freq_corr_model(struct esp_sdr_cal_record *rec);

/**
 * @brief Take a measured crystal error into the correction in force, and store it.
 *
 * With a temperature the measurement becomes a point of the model in force
 * (esp_sdr_cal_add_point()), without one the constant correction (the points go). The result
 * is applied (esp_sdr_set_freq_corr_model()) and, with CONFIG_ESP_SDR_CALSTORE, stored.
 * esp_sdr_lte_calibrate() ends with it; a host that measures the error itself (the WHY2025
 * badge's P4 for its C6) hands its measurement over the same way.
 *
 * @param meas The measurement: ppb, spread_ppb, carrier_khz, cell_id, estimates and temp_dc
 *	       (ESP_SDR_CAL_TEMP_UNKNOWN for none); its points are not used.
 * @param out Set to the record now in force, or NULL.
 * @retval 0 if applied and stored.
 * @retval -ENOTSUP if applied, without CONFIG_ESP_SDR_CALSTORE.
 * @retval -EINVAL beyond ESP_SDR_FREQ_CORR_MAX_PPB: nothing changes.
 * @retval other negative errno if applied but not stored.
 */
int esp_sdr_cal_update(const struct esp_sdr_cal_record *meas, struct esp_sdr_cal_record *out);

#if defined(CONFIG_ESP_SDR_CALSTORE) || defined(__DOXYGEN__)
/**
 * @brief Read the stored calibration (settings key "esp_sdr/cal").
 *
 * @param rec Filled with it.
 * @retval 0 on success.
 * @retval -ENOENT if none is stored.
 * @retval -ENOTSUP without CONFIG_ESP_SDR_CALSTORE.
 * @retval other negative errno from the settings backend.
 */
int esp_sdr_cal_load(struct esp_sdr_cal_record *rec);

/**
 * @brief Store a calibration (the radio's correction is not changed).
 *
 * @retval 0 on success, negative errno as for esp_sdr_cal_load().
 */
int esp_sdr_cal_save(const struct esp_sdr_cal_record *rec);

/**
 * @brief Remove the stored calibration.
 *
 * @retval 0 on success (also when none was stored), negative errno otherwise.
 */
int esp_sdr_cal_clear(void);
#else
static inline int esp_sdr_cal_load(struct esp_sdr_cal_record *rec)
{
	(void)rec;
	return -ENOTSUP;
}

static inline int esp_sdr_cal_save(const struct esp_sdr_cal_record *rec)
{
	(void)rec;
	return -ENOTSUP;
}

static inline int esp_sdr_cal_clear(void)
{
	return -ENOTSUP;
}
#endif

/** What esp_sdr_lte_calibrate() searches and how long. */
struct esp_sdr_lte_scan {
	/** First LO frequency in MHz. */
	uint32_t start_mhz;
	/** Last LO frequency in MHz. */
	uint32_t stop_mhz;
	/** LO frequencies whose power ranks highest that get searched for a cell. */
	uint8_t candidates;
	/** Captures tried at each of those before moving on. */
	uint8_t tries;
	/** Estimates to collect from one cell. */
	uint8_t estimates;
	/** Receive gain index, or ESP_SDR_RX_GAIN_AUTO. */
	int8_t gain;
};

/** A calibration's outcome. */
struct esp_sdr_lte_result {
	/** Correction now applied, ppb. */
	int32_t ppb;
	/** What this run measured on top of the correction it started with, ppb. */
	int32_t delta_ppb;
	/** Spread (half the range) of the estimates, ppb. */
	int32_t spread_ppb;
	/** Raster frequency of the carrier, kHz. */
	uint32_t carrier_khz;
	/** Physical cell id. */
	int cell_id;
	/** Estimates combined. */
	int estimates;
	/** Captures taken. */
	int captures;
	/** Captures in which a cell decoded. */
	int decoded;
	/** Scan steps the LO does not reach (the VCO's window, through the 5/6 divider below 2210 MHz). */
	int unreachable;
	/** Chip temperature during the estimates, 0.1 degC, or ESP_SDR_CAL_TEMP_UNKNOWN. */
	int temp_dc;
	/** Its change from the first estimate to the last, 0.1 degC. */
	int temp_drift_dc;
	/** Points of the temperature model now. */
	int points;
};

/** Scan defaults: LTE band 1 downlink (2110 to 2170 MHz). */
#define ESP_SDR_LTE_SCAN_DEFAULT                                                                   \
	{                                                                                          \
		.start_mhz = 2110, .stop_mhz = 2170, .candidates = 6, .tries = 12, .estimates = 5, \
		.gain = -1,                                                                        \
	}

/**
 * @brief Measure the crystal's error against an LTE cell and apply it.
 *
 * Steps the LO across the scan range by 1 MHz (steps the LO does not reach are left out: the
 * S3 reaches down to about 1865 MHz, the C6 to about 1775), ranks the steps by received
 * power, then captures at the strongest ones until a cell decodes (esp_sdr_lte_detect()), and collects
 * estimates from that cell, each against where the LO is (esp_sdr_get_lo_residual_hz()).
 * Their median, added to the correction in force, goes to esp_sdr_cal_update(): a point of the
 * temperature model, applied and, with CONFIG_ESP_SDR_CALSTORE, stored.
 * Takes the radio for seconds to minutes and gives it back with the frequency and gain it had.
 * The S3 and the C5 capture 1 to 2 ms at a time, the C6 0.2 ms (80 MS/s only), so the C6
 * needs many more captures to catch the synchronization signals (every 5 ms).
 * Needs CONFIG_ESP_SDR_LTE_CAL, and about 9 KiB of stack in the calling thread (rfprobe's shell
 * thread peaks at 11.6 KiB with it on the S3).
 *
 * @param scan What to search; NULL for ESP_SDR_LTE_SCAN_DEFAULT.
 * @param res Filled with the outcome.
 * @retval 0 on success.
 * @retval -ENOENT if no cell decoded, -ERANGE if the estimates disagree.
 * @retval -ENOMEM without the heap for the work memory.
 * @retval other negative errno from the radio.
 */
int esp_sdr_lte_calibrate(const struct esp_sdr_lte_scan *scan, struct esp_sdr_lte_result *res);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ESP_SDR_ESP_SDR_CAL_H_ */
