/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The radio arbiter's wake predictor (esp_sdr_arb.c), plain C: the Wi-Fi blob powers the PHY up
 * once every beacon (or listen) interval in modem sleep and once every wake interval with ESP-NOW
 * windows. From the times of the last enables it learns the period (the median of the
 * enable-to-enable intervals, robust against the odd long awake phase) and the phase (the latest
 * enable), and says when the next one comes. tests/unit/arb_pred runs it on native_sim.
 */

#ifndef ESP_SDR_ARB_PRED_H_
#define ESP_SDR_ARB_PRED_H_

#include <stdbool.h>
#include <stdint.h>

/* Enable times kept; intervals a period needs (three: a median that one outlier cannot move). */
#define ARB_PRED_N         8U
#define ARB_PRED_MIN_INTV  3U
/* Past this many periods without an enable the wake is no longer periodic. */
#define ARB_PRED_STALE_PER 4
/* Without a period yet: the first enables of a start are waited for this long (us). */
#define ARB_PRED_LEARN_US  2000000

enum arb_pred_state {
	/* No periodic wake: Wi-Fi absent, never sleeps, or stopped. */
	ARB_PRED_NONE,
	/* Wi-Fi sleeps but the period is not known yet: wait for more enables. */
	ARB_PRED_LEARNING,
	/* Period and phase known. */
	ARB_PRED_PERIODIC,
};

struct arb_pred {
	/* The last ARB_PRED_N enable times (us), oldest first once full. */
	int64_t t[ARB_PRED_N];
	/* Enables ever seen, the next write slot. */
	uint32_t seen, head;
};

static inline void arb_pred_reset(struct arb_pred *p)
{
	p->seen = 0U;
	p->head = 0U;
}

static inline void arb_pred_add(struct arb_pred *p, int64_t t_us)
{
	p->t[p->head] = t_us;
	p->head = (p->head + 1U) % ARB_PRED_N;
	p->seen++;
}

/* Number of enable times held. */
static inline uint32_t arb_pred_count(const struct arb_pred *p)
{
	return p->seen < ARB_PRED_N ? p->seen : ARB_PRED_N;
}

/* The k-th newest enable time, k = 0 the latest. */
static inline int64_t arb_pred_newest(const struct arb_pred *p, uint32_t k)
{
	return p->t[(p->head + ARB_PRED_N - 1U - k) % ARB_PRED_N];
}

/* The median enable-to-enable interval (us), 0 until ARB_PRED_MIN_INTV intervals are known. */
static inline int64_t arb_pred_period(const struct arb_pred *p)
{
	uint32_t n = arb_pred_count(p);
	int64_t d[ARB_PRED_N];
	uint32_t m;

	if (n < ARB_PRED_MIN_INTV + 1U) {
		return 0;
	}
	m = n - 1U;
	for (uint32_t i = 0; i < m; i++) {
		int64_t v = arb_pred_newest(p, i) - arb_pred_newest(p, i + 1U);

		/* Insertion sort: m is at most 7. */
		uint32_t j = i;

		while (j > 0U && d[j - 1U] > v) {
			d[j] = d[j - 1U];
			j--;
		}
		d[j] = v;
	}
	return (m & 1U) != 0U ? d[m / 2U] : (d[m / 2U - 1U] + d[m / 2U]) / 2;
}

static inline enum arb_pred_state arb_pred_state_at(const struct arb_pred *p, int64_t now_us)
{
	int64_t period, age;

	if (p->seen == 0U) {
		return ARB_PRED_NONE;
	}
	period = arb_pred_period(p);
	age = now_us - arb_pred_newest(p, 0U);
	if (period == 0) {
		return age <= ARB_PRED_LEARN_US ? ARB_PRED_LEARNING : ARB_PRED_NONE;
	}
	return age <= ARB_PRED_STALE_PER * period ? ARB_PRED_PERIODIC : ARB_PRED_NONE;
}

/* The first enable after now_us on the learned phase (PERIODIC only). */
static inline int64_t arb_pred_next(const struct arb_pred *p, int64_t now_us)
{
	int64_t period = arb_pred_period(p);
	int64_t last = arb_pred_newest(p, 0U);
	int64_t k = now_us > last ? (now_us - last) / period + 1 : 1;

	return last + k * period;
}

/*
 * A wake seen as an enable at enable_us and the disable after it at disable_us: taught to the
 * predictor only if it lasted min_us (the blob's short pair after the SDR's release is no beacon
 * wake). Returns whether it was.
 */
static inline bool arb_pred_wake(struct arb_pred *p, int64_t enable_us, int64_t disable_us,
				 int64_t min_us)
{
	if (disable_us - enable_us < min_us) {
		return false;
	}
	arb_pred_add(p, enable_us);
	return true;
}

#endif /* ESP_SDR_ARB_PRED_H_ */
