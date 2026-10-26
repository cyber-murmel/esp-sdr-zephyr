/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Crystal calibration against an LTE cell: scan, find a cell, collect estimates, apply.
 * The signal processing is include/esp_sdr/esp_sdr_lte.h.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_cal.h>
#include <esp_sdr/esp_sdr_lte.h>
#include <esp_sdr/esp_sdr_rx.h>

#include "esp_sdr_priv.h"

LOG_MODULE_DECLARE(esp_sdr, CONFIG_ESP_SDR_LOG_LEVEL);

/*
 * The estimator runs at 2 MS/s on captures of up to LTE_SAMPLES, which with a symbol to spare
 * fit the largest transform (CONFIG_ESP_SDR_LTE_CAL_FFT); a shorter capture (the C6's 0.2 ms)
 * takes a smaller one.
 */
#define LTE_FS        2000000U
#define LTE_NFFT_MAX  ((uint32_t)CONFIG_ESP_SDR_LTE_CAL_FFT)
#define LTE_SAMPLES   (LTE_NFFT_MAX - 140U)
/* With 1 MHz LO steps a carrier is at most 500 kHz from one of them. */
#define LTE_MAX_OFS   520e3
#define LTE_MIN_RATIO 20.0f
/* Power captures per LO step for the survey (the downlink's power comes in bursts). */
#define SURVEY_CAPS   3
#define MAX_STEPS     512U
/* Estimates further apart than this mean the cell or the capture is not to be trusted. */
#define MAX_SPREAD    500

#if defined(CONFIG_SOC_SERIES_ESP32C6)
/*
 * The C6 captures 0.2 ms (80 MS/s only): a 1024 point transform does. Its work memory sits in
 * the SRAM above the capture bank, as the image leaves little DRAM below it.
 */
#define LTE_STATIC_NFFT 1024U
static float lte_mem[ESP_SDR_LTE_WORK_FLOATS(LTE_STATIC_NFFT)] __aligned(4) ESP_SDR_HIGH_RAM;
static struct esp_sdr_iq16 lte_buf[LTE_STATIC_NFFT] __aligned(4) ESP_SDR_HIGH_RAM;
#endif

struct lte_ctx {
	enum esp_sdr_rate rate;
	unsigned int m;
	size_t count;
	struct esp_sdr_iq16 *buf;
	size_t max_n;
	size_t n;
	uint32_t n_fft;
	struct esp_sdr_lte_work w;
	float *mem;
};

/* The slowest native rate that 2 MS/s divides: the longest capture. */
static int pick_rate(struct lte_ctx *c)
{
	static const enum esp_sdr_rate rates[] = {
		ESP_SDR_RATE_4MSPS,  ESP_SDR_RATE_8MSPS,  ESP_SDR_RATE_10MSPS, ESP_SDR_RATE_16MSPS,
		ESP_SDR_RATE_20MSPS, ESP_SDR_RATE_40MSPS, ESP_SDR_RATE_80MSPS,
	};

	for (size_t i = 0; i < ARRAY_SIZE(rates); i++) {
		uint32_t hz = esp_sdr_rx_rate_hz(rates[i]);

		if (hz == 0U || hz % LTE_FS != 0U) {
			continue;
		}
		c->rate = rates[i];
		c->m = hz / LTE_FS;
		c->count = MIN((size_t)ESP_SDR_SAMPLES_MAX, (LTE_SAMPLES + 3U) * c->m);
		/* The CIC drops its first 3 outputs. */
		c->max_n = c->count / c->m - 3U;
		c->n_fft = 256U;
		while (c->n_fft < c->max_n + esp_sdr_lte_symbol_len(LTE_FS) + 1U) {
			c->n_fft <<= 1;
		}
		return 0;
	}
	return -ENOTSUP;
}

static int capture(struct lte_ctx *c)
{
	uint64_t t0;

	return esp_sdr_rx_capture_decimated(c->rate, c->count, c->m, c->buf, c->max_n, &c->n,
					    &t0);
}

static float power(const struct lte_ctx *c)
{
	float p = 0.0f;

	for (size_t i = 0; i < c->n; i++) {
		p += (float)c->buf[i].i * c->buf[i].i + (float)c->buf[i].q * c->buf[i].q;
	}
	return c->n > 0U ? p / (float)c->n : 0.0f;
}

static int cmp_i32(const void *a, const void *b)
{
	int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;

	return (x > y) - (x < y);
}

/* A step the LO does not reach (esp_sdr_set_freq() -ERANGE): no power measured. */
#define PW_UNREACHABLE (-1.0f)

/*
 * The order to search the LO steps in: a downlink carrier is 5 to 20 MHz wide and its
 * synchronization signals sit at its centre, so take runs of steps well above the median
 * power of the reachable ones, strongest run first, and each run from its centre outwards.
 */
static size_t search_order(const float *pw, size_t steps, uint16_t *order, size_t max)
{
	float *sorted = k_malloc(steps * sizeof(float));
	float thr;
	size_t n = 0, measured = 0;

	if (sorted == NULL) {
		return 0;
	}
	for (size_t i = 0; i < steps; i++) {
		float v = pw[i];
		size_t j = measured;

		if (v < 0.0f) {
			continue;
		}
		for (; j > 0 && sorted[j - 1] > v; j--) {
			sorted[j] = sorted[j - 1];
		}
		sorted[j] = v;
		measured++;
	}
	if (measured == 0U) {
		k_free(sorted);
		return 0;
	}
	/* 6 dB over the median; an unreachable step (below 0) never passes it. */
	thr = MAX(4.0f * sorted[measured / 2], 0.0f);
	k_free(sorted);

	bool *used = k_calloc(steps, sizeof(bool));

	if (used == NULL) {
		return 0;
	}
	while (n < max) {
		/* The strongest run not taken yet. */
		size_t best_lo = 0, best_hi = 0;
		float best_mean = 0.0f;

		for (size_t i = 0; i < steps;) {
			if (pw[i] < thr || used[i]) {
				i++;
				continue;
			}
			size_t j = i;
			float sum = 0.0f;

			while (j < steps && pw[j] >= thr && !used[j]) {
				sum += pw[j++];
			}
			if (sum / (float)(j - i) > best_mean) {
				best_mean = sum / (float)(j - i);
				best_lo = i;
				best_hi = j;
			}
			i = j;
		}
		if (best_mean == 0.0f) {
			break;
		}
		size_t mid = (best_lo + best_hi - 1U) / 2U;

		for (size_t d = 0; n < max && d <= best_hi - best_lo; d++) {
			if (mid + d < best_hi) {
				order[n++] = (uint16_t)(mid + d);
			}
			if (d > 0U && mid >= best_lo + d && n < max) {
				order[n++] = (uint16_t)(mid - d);
			}
		}
		for (size_t i = best_lo; i < best_hi; i++) {
			used[i] = true;
		}
	}
	k_free(used);
	return n;
}

int esp_sdr_lte_calibrate(const struct esp_sdr_lte_scan *scan, struct esp_sdr_lte_result *res)
{
	static const struct esp_sdr_lte_scan defaults = ESP_SDR_LTE_SCAN_DEFAULT;
	struct lte_ctx c = {0};
	uint32_t old_mhz = esp_sdr_get_freq();
	int32_t old_khz = esp_sdr_fofs_khz;
	int old_gain = esp_sdr_rx_get_gain();
	int32_t old_ppb = 0;
	int16_t t_first = ESP_SDR_CAL_TEMP_UNKNOWN, t_last = ESP_SDR_CAL_TEMP_UNKNOWN;
	struct esp_sdr_cal_record rec;
	int32_t est[32];
	size_t steps, n_order;
	float *pw = NULL;
	uint16_t *order = NULL;
	int ret, found_cell = -1;
	uint32_t found_raster = 0;
	int n_est = 0;

	if (scan == NULL) {
		scan = &defaults;
	}
	*res = (struct esp_sdr_lte_result){.cell_id = -1, .temp_dc = ESP_SDR_CAL_TEMP_UNKNOWN};
	if (scan->stop_mhz < scan->start_mhz || scan->stop_mhz - scan->start_mhz + 1U > MAX_STEPS ||
	    scan->estimates == 0U || scan->estimates > ARRAY_SIZE(est)) {
		return -EINVAL;
	}
	ret = pick_rate(&c);
	if (ret != 0) {
		return ret;
	}
	steps = scan->stop_mhz - scan->start_mhz + 1U;
#if defined(LTE_STATIC_NFFT)
	if (c.n_fft > LTE_STATIC_NFFT) {
		return -ENOMEM;
	}
	c.buf = lte_buf;
	c.mem = lte_mem;
#else
	c.buf = k_malloc(c.max_n * sizeof(*c.buf));
	c.mem = k_malloc(ESP_SDR_LTE_WORK_FLOATS(c.n_fft) * sizeof(float));
#endif
	pw = k_calloc(steps, sizeof(float));
	order = k_malloc(MAX(scan->candidates, 1U) * sizeof(uint16_t));
	if (c.buf == NULL || c.mem == NULL || pw == NULL || order == NULL) {
		ret = -ENOMEM;
		goto out;
	}
	esp_sdr_lte_work_init(&c.w, c.mem, c.n_fft);
	/* Every estimate is against the correction in force now, held for the run. */
	k_mutex_lock(&esp_sdr_lock, K_FOREVER);
	esp_sdr_corr_refresh();
	old_ppb = esp_sdr_corr_ppb;
	esp_sdr_corr_frozen = true;
	k_mutex_unlock(&esp_sdr_lock);
	ret = esp_sdr_rx_set_gain(scan->gain);
	if (ret != 0) {
		goto out;
	}
	ret = esp_sdr_set_freq_offset(0);
	if (ret != 0 && ret != -ERANGE) {
		goto out;
	}

	/* Survey: received power at each LO step the chip reaches. */
	for (size_t s = 0; s < steps; s++) {
		ret = esp_sdr_set_freq(scan->start_mhz + s);
		if (ret == -ERANGE) {
			pw[s] = PW_UNREACHABLE;
			res->unreachable++;
			continue;
		}
		if (ret != 0) {
			goto out;
		}
		for (int k = 0; k < SURVEY_CAPS; k++) {
			ret = capture(&c);
			if (ret != 0) {
				goto out;
			}
			pw[s] += power(&c) / SURVEY_CAPS;
			res->captures++;
		}
	}
	n_order = search_order(pw, steps, order, MAX(scan->candidates, 1U));
	LOG_INF("LTE survey %u to %u MHz: %u steps out of the LO's reach, %u places to search",
		scan->start_mhz, scan->stop_mhz, (unsigned int)res->unreachable,
		(unsigned int)n_order);

	/* Search: captures at each place until a cell decodes, then estimates from that cell. */
	for (size_t o = 0; o < n_order && n_est < scan->estimates; o++) {
		uint32_t mhz = scan->start_mhz + order[o];
		int budget = scan->tries;
		/* Where the LO is: the correction in force, rounded to whole kHz, leaves a rest. */
		double lo_hz;

		ret = esp_sdr_set_freq(mhz);
		if (ret != 0) {
			continue;
		}
		lo_hz = (double)mhz * 1e6 + (double)esp_sdr_get_lo_residual_hz();
		for (int t = 0; t < budget && n_est < scan->estimates; t++) {
			struct esp_sdr_lte_cell cell;
			double raster;

			ret = capture(&c);
			if (ret != 0) {
				goto out;
			}
			res->captures++;
			/* A cell decoded with margin whose reference signals cohere: an SSS alone
			 * decodes "cells" on noise now and then. */
			if (esp_sdr_lte_detect((const struct esp_sdr_lte_iq *)c.buf, c.n, LTE_FS,
					       LTE_MAX_OFS, LTE_MIN_RATIO, &c.w, &cell) != 0 ||
			    !cell.valid || cell.crs_symbols == 0) {
				continue;
			}
			res->decoded++;
			double ppb = esp_sdr_lte_ppb(lo_hz, cell.offset_hz, &raster);

			if (found_cell < 0) {
				found_cell = cell.cell_id;
				found_raster = (uint32_t)(raster / 1e3);
				/* A cell here: give it the captures the estimates need. */
				budget = scan->tries * scan->estimates;
				LOG_INF("cell %d at %u kHz (LO %u MHz %+d Hz)", found_cell,
					found_raster, mhz, (int)cell.offset_hz);
			}
			if (cell.cell_id != found_cell || (uint32_t)(raster / 1e3) != found_raster) {
				continue;
			}
			est[n_est++] = (int32_t)ppb;
			if (esp_sdr_get_temp_dc(&t_last) != 0) {
				t_last = ESP_SDR_CAL_TEMP_UNKNOWN;
			} else if (t_first == ESP_SDR_CAL_TEMP_UNKNOWN) {
				t_first = t_last;
			}
			LOG_DBG("estimate %d: %d ppb (sss %d %%)", n_est, (int32_t)ppb,
				(int)(cell.sss_metric * 100.0f));
		}
		if (found_cell >= 0) {
			break;
		}
	}
	if (n_est == 0) {
		ret = -ENOENT;
		goto out;
	}
	qsort(est, n_est, sizeof(est[0]), cmp_i32);
	res->delta_ppb = est[n_est / 2];
	res->spread_ppb = (est[n_est - 1] - est[0]) / 2;
	res->estimates = n_est;
	res->cell_id = found_cell;
	res->carrier_khz = found_raster;
	if (res->spread_ppb > MAX_SPREAD) {
		ret = -ERANGE;
		goto out;
	}
	res->ppb = old_ppb + res->delta_ppb;
	if (t_first != ESP_SDR_CAL_TEMP_UNKNOWN && t_last != ESP_SDR_CAL_TEMP_UNKNOWN) {
		res->temp_dc = (t_first + t_last) / 2;
		res->temp_drift_dc = t_last - t_first;
	}

	/* The model in force plus this point (without a temperature: a constant). */
	rec = (struct esp_sdr_cal_record){
		.ppb = res->ppb,
		.spread_ppb = res->spread_ppb,
		.carrier_khz = res->carrier_khz,
		.cell_id = (int16_t)res->cell_id,
		.estimates = (uint16_t)res->estimates,
		.temp_dc = (int16_t)res->temp_dc,
	};
	esp_sdr_corr_frozen = false;
	ret = esp_sdr_cal_update(&rec, &rec);
	if (ret == -EINVAL) {
		goto out;
	}
	if (ret != 0 && ret != -ENOTSUP) {
		LOG_WRN("calibration not stored: %d", ret);
	}
	res->points = rec.points;
	ret = 0;
out:
	esp_sdr_corr_frozen = false;
	(void)esp_sdr_rx_set_gain(old_gain);
	(void)esp_sdr_set_freq(old_mhz);
	(void)esp_sdr_set_freq_offset(old_khz);
	k_free(order);
	k_free(pw);
#if !defined(LTE_STATIC_NFFT)
	k_free(c.mem);
	k_free(c.buf);
#endif
	return ret;
}
