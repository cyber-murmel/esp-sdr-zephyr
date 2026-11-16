/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Triggered snapshots on the ESP32-C6 (esp_sdr_trig.h): the dump engine as a
 * circular ring in SRAM block 2, the IQ estimator's power sums as the trigger.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_paced.h>
#include <esp_sdr/esp_sdr_trig.h>

#include "esp_sdr_priv.h"

extern uint32_t __esp_sdr_bank_end[];

/*
 * In circular mode (DUMP_CTRL_CIRCULAR) DUMP_MODE_REG's low bits give the write index (bit 14
 * toggles per lap) and read 0 once the engine stops.
 */
#define DUMP_INDEX_M GENMASK(14, 0)

#define WINDOW_CYC (ESP_SDR_TRIG_WINDOW_US * (ESP_SDR_CPU_HZ / 1000000U))
/*
 * Restart the estimator every 8000 inputs (100 us): its sums then stay below
 * 2^31 at full scale, whatever they do past that.
 */
#define RESTART_CYC (WINDOW_CYC * 10U)
/* 2 CPU cycles per sample at 160 MHz. */
#define CYC_PER_SAMPLE (ESP_SDR_CPU_HZ / ESP_SDR_TRIG_RATE_HZ)
#define LEARN_WINDOWS 64U
/*
 * No quiet window for this long (10 ms) after learning: learn again. A window of a receiver
 * that held its output (a gain change) has no power about its mean, and a baseline learnt
 * from it is so low that no window is quiet again; nor would a band that is busy throughout.
 */
#define RELEARN_WINDOWS 1000U
#define BASE_FOLLOW   256

static uint32_t baseline;
static uint32_t learnt;
/* The windows' powers while learning. */
static uint32_t learn_pw[LEARN_WINDOWS];

/*
 * One reading of the sums, and the cycle count just before it. With the sums of I and Q (times
 * 64) the power sums give a window's power about its mean: the receiver's DC is larger than
 * its noise and drifts after a retune.
 */
struct est_sums {
	uint32_t t, p, si, sq;
};

static inline void est_read(struct est_sums *e)
{
	e->t = esp_sdr_paced_cycles();
	e->si = sys_read32(IQE_SUM_I_REG);
	e->sq = sys_read32(IQE_SUM_Q_REG);
	e->p = sys_read32(IQE_POW_REG);
}

/*
 * The power of the inputs between two readings about their mean: the sum of I^2 + Q^2 less
 * (sum I)^2 + (sum Q)^2 over their count. The count comes from the readings' own cycle counts:
 * each input off adds the DC's square, many times the noise's. One window's sums fit 32 bits,
 * so the differences are exact across a wrap.
 */
static uint32_t est_power(const struct est_sums *a, const struct est_sums *b)
{
	uint32_t n = (b->t - a->t + CYC_PER_SAMPLE / 2U) / CYC_PER_SAMPLE;
	int64_t si = (int32_t)(b->si - a->si) / 64;
	int64_t sq = (int32_t)(b->sq - a->sq) / 64;
	int64_t p = (int64_t)(uint32_t)(b->p - a->p) * 8;

	p -= (si * si + sq * sq) / (int64_t)MAX(n, 1U);
	return p > 0 ? (uint32_t)p : 0U;
}

/*
 * The lower quartile of the learning windows: neither bursts in up to three quarters of them
 * nor windows of a held output (no power about the mean) in up to a quarter move it much.
 * Sorts @p v.
 */
static uint32_t lower_quartile(uint32_t *v)
{
	for (uint32_t i = 1; i < LEARN_WINDOWS; i++) {
		uint32_t x = v[i], j = i;

		for (; j > 0U && v[j - 1U] > x; j--) {
			v[j] = v[j - 1U];
		}
		v[j] = x;
	}
	return v[LEARN_WINDOWS / 4U];
}

void esp_sdr_trig_reset(void)
{
	baseline = 0;
	learnt = 0;
}

int esp_sdr_trig_wait(const struct esp_sdr_trig_cfg *cfg, struct esp_sdr_trig_snap *snap)
{
	uint32_t usage, saved, ctrl1;
	struct est_sums e0, e1;
	uint32_t t_win, t_restart, t_arm, t_trig = 0;
	uint32_t trig_idx = 0, stop_idx = 0, power = 0, thr_base = 0, run = 0, loud = 0;
	bool quiet = false, trig = false;
	unsigned int key;
	int ret = 0;

	if (cfg == NULL || snap == NULL || cfg->post == 0U || cfg->post >= ESP_SDR_TRIG_RING ||
	    (cfg->level == 0U && cfg->ratio_x16 <= 16U)) {
		return -EINVAL;
	}
	if (!esp_sdr_ready) {
		return -EAGAIN;
	}
	if (esp_sdr_bank_usage(&usage) != 0) {
		return -EINVAL;
	}
	k_mutex_lock(&esp_sdr_lock, K_FOREVER);
	(void)esp_sdr_rx_gain_refresh();
	esp_sdr_paced_cycles_enable();
	ctrl1 = iqe_window_mode();

	key = irq_lock();
	saved = esp_sdr_dump_arm(usage);
	sys_write32(DUMP_CTRL_CIRCULAR, DUMP_CTRL_REG);
	sys_write32(DUMP_CTRL_CIRCULAR | DUMP_CTRL_RUN, DUMP_CTRL_REG);
	iqe_restart(ctrl1);
	t_arm = t_win = t_restart = esp_sdr_paced_cycles();
	est_read(&e0);

	for (;;) {
		uint32_t now = esp_sdr_paced_cycles(), dt, pw;

		if (now - t_win < WINDOW_CYC) {
			continue;
		}
		est_read(&e1);
		dt = now - t_win;
		t_win = now;
		pw = est_power(&e0, &e1);
		if (dt > WINDOW_CYC) {
			/* A late window (the stop() poll ran long): scaled to the nominal length. */
			pw = (uint32_t)((uint64_t)pw * WINDOW_CYC / dt);
		}
		if (now - t_restart >= RESTART_CYC) {
			iqe_restart(ctrl1);
			t_restart = now;
			est_read(&e1);
		}
		e0 = e1;

		if (trig) {
			if (now - t_trig >= cfg->post * CYC_PER_SAMPLE) {
				stop_idx = sys_read32(DUMP_MODE_REG) & DUMP_INDEX_M;
				sys_write32(DUMP_CTRL_CIRCULAR, DUMP_CTRL_REG);
				break;
			}
			continue;
		}

		if (cfg->level == 0U && learnt < LEARN_WINDOWS) {
			learn_pw[learnt++] = pw;
			if (learnt == LEARN_WINDOWS) {
				baseline = MAX(lower_quartile(learn_pw), 1U);
			}
		} else {
			uint32_t thr = cfg->level != 0U
					       ? cfg->level
					       : (uint32_t)MIN((uint64_t)baseline * cfg->ratio_x16 / 16U,
							       UINT32_MAX);

			if (pw > thr) {
				if (run > 0U) {
					run++;
				} else if (quiet &&
					   now - t_arm >= ESP_SDR_TRIG_RING * CYC_PER_SAMPLE) {
					/*
					 * A rising edge, once the ring holds only samples taken since
					 * arming: the burst starts here if it lasts.
					 */
					run = 1U;
					t_trig = now;
					trig_idx = sys_read32(DUMP_MODE_REG) & DUMP_INDEX_M;
					power = pw;
					thr_base = baseline;
				}
				trig = run >= ESP_SDR_TRIG_WINDOWS;
				if (!quiet && cfg->level == 0U && ++loud >= RELEARN_WINDOWS) {
					learnt = 0;
					loud = 0;
				}
			} else {
				run = 0U;
				quiet = true;
				if (cfg->level == 0U) {
					baseline = (uint32_t)((int64_t)baseline +
							      ((int64_t)pw - (int64_t)baseline) /
								      BASE_FOLLOW);
				}
			}
		}
		if (cfg->stop != NULL && !trig) {
			irq_unlock(key);
			bool end = cfg->stop(cfg->user);

			key = irq_lock();
			if (end) {
				sys_write32(DUMP_CTRL_CIRCULAR, DUMP_CTRL_REG);
				ret = -EINTR;
				break;
			}
		}
	}
	esp_sdr_dump_disarm(saved);
	irq_unlock(key);
	k_mutex_unlock(&esp_sdr_lock);
	if (ret != 0) {
		return ret;
	}

	snap->words = (const uint32_t *)__esp_sdr_bank_start;
	snap->first = stop_idx & (ESP_SDR_TRIG_RING - 1U);
	uint32_t after = (stop_idx - trig_idx) & (ESP_SDR_TRIG_RING - 1U);

	/* The window that triggered began one window before its reading. */
	snap->trig = ESP_SDR_TRIG_RING - after -
		     MIN(ESP_SDR_TRIG_WINDOW_US * (ESP_SDR_TRIG_RATE_HZ / 1000000U),
			 ESP_SDR_TRIG_RING - after);
	snap->power = power;
	snap->baseline = thr_base;
	snap->scratch = (uint8_t *)__esp_sdr_bank_start + ESP_SDR_TRIG_RING * 4U;
	snap->scratch_len = (size_t)((uintptr_t)__esp_sdr_bank_end -
				     (uintptr_t)__esp_sdr_bank_start) -
			    ESP_SDR_TRIG_RING * 4U;
	return 0;
}

void esp_sdr_trig_pack_iq8(const struct esp_sdr_trig_snap *snap, uint32_t from, uint32_t n,
			   int8_t *out)
{
	uint16_t *o = (uint16_t *)out;
	uint32_t at = (snap->first + from) & (ESP_SDR_TRIG_RING - 1U);

	while (n > 0U) {
		/* Up to the ring's end, then from its start. */
		uint32_t run = MIN(n, ESP_SDR_TRIG_RING - at);
		const uint32_t *w = snap->words + at;

		n -= run;
		at = 0;
		for (; run >= 4U; run -= 4U, w += 4, o += 4) {
			uint32_t a = w[0], b = w[1], c = w[2], d = w[3];

			o[0] = (uint16_t)esp_sdr_rx_iq8(a);
			o[1] = (uint16_t)esp_sdr_rx_iq8(b);
			o[2] = (uint16_t)esp_sdr_rx_iq8(c);
			o[3] = (uint16_t)esp_sdr_rx_iq8(d);
		}
		for (; run > 0U; run--, w++, o++) {
			o[0] = (uint16_t)esp_sdr_rx_iq8(w[0]);
		}
	}
}
