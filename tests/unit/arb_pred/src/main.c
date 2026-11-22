/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit test of the radio arbiter's wake predictor (lib/esp_sdr/src/esp_sdr_arb_pred.h): the
 * period as the median of the enable-to-enable intervals and the phase from the last enable,
 * for the wake patterns measured on the S3 (coexistence.md): modem sleep at DTIM 1 (102.4 ms),
 * MAX modem with a listen interval of 3 (307.2 ms) and ESP-NOW windows (about 103 ms), with
 * jitter, the odd long awake phase, a change of power save and a stop.
 */

#include <zephyr/ztest.h>

#include "esp_sdr_arb_pred.h"

#define MS(x) ((int64_t)(x) * 1000)

static struct arb_pred p;

static void before(void *f)
{
	arb_pred_reset(&p);
}

/* n enables from t0, period apart, jitter[k % 4] added. */
static int64_t feed(int64_t t0, int64_t period, int n, const int64_t *jit)
{
	int64_t t = t0;

	for (int k = 0; k < n; k++) {
		arb_pred_add(&p, t + (jit != NULL ? jit[k % 4] : 0));
		t += period;
	}
	return t;
}

ZTEST(arb_pred, test_nothing_seen_is_none)
{
	zassert_equal(arb_pred_state_at(&p, 0), ARB_PRED_NONE);
	zassert_equal(arb_pred_period(&p), 0);
}

ZTEST(arb_pred, test_learning_until_three_intervals)
{
	int64_t t = MS(1000);

	for (int k = 0; k < 3; k++) {
		arb_pred_add(&p, t + k * 102400);
		zassert_equal(arb_pred_period(&p), 0, "%d enables", k + 1);
		zassert_equal(arb_pred_state_at(&p, t + k * 102400 + MS(5)), ARB_PRED_LEARNING);
	}
	arb_pred_add(&p, t + 3 * 102400);
	zassert_equal(arb_pred_period(&p), 102400);
	zassert_equal(arb_pred_state_at(&p, t + 3 * 102400 + MS(5)), ARB_PRED_PERIODIC);
}

ZTEST(arb_pred, test_few_enables_then_silence_is_none)
{
	/* A start (one or two enables) that never goes periodic: not waited for forever. */
	arb_pred_add(&p, MS(1000));
	arb_pred_add(&p, MS(1100));
	zassert_equal(arb_pred_state_at(&p, MS(1100) + ARB_PRED_LEARN_US - 1), ARB_PRED_LEARNING);
	zassert_equal(arb_pred_state_at(&p, MS(1100) + ARB_PRED_LEARN_US + 1), ARB_PRED_NONE);
}

ZTEST(arb_pred, test_dtim1_period_and_phase)
{
	static const int64_t jit[4] = {0, 1500, -900, 400};
	int64_t last, next_t;

	(void)feed(MS(500), 102400, 8, jit);
	last = MS(500) + 7 * 102400 + jit[7 % 4];
	zassert_within(arb_pred_period(&p), 102400, 2000);
	/* Wi-Fi went to sleep 9 ms after its enable: the next wake is one period after it. */
	next_t = arb_pred_next(&p, last + MS(9));
	zassert_within(next_t - last, 102400, 2000);
	/* A little before the wake it still is that wake; just after, the one after. */
	zassert_equal(arb_pred_next(&p, next_t - 1), next_t);
	zassert_equal(arb_pred_next(&p, next_t + 1), next_t + arb_pred_period(&p));
}

ZTEST(arb_pred, test_max_modem_307ms)
{
	int64_t last = feed(MS(2000), 307200, 6, NULL) - 307200;

	zassert_equal(arb_pred_period(&p), 307200);
	zassert_equal(arb_pred_next(&p, last + MS(10)), last + 307200);
	zassert_equal(arb_pred_state_at(&p, last + MS(200)), ARB_PRED_PERIODIC);
}

ZTEST(arb_pred, test_espnow_window_103ms)
{
	int64_t last = feed(MS(100), 103000, 8, NULL) - 103000;

	zassert_equal(arb_pred_period(&p), 103000);
	zassert_equal(arb_pred_next(&p, last + MS(16)), last + 103000);
}

ZTEST(arb_pred, test_an_outlier_does_not_move_the_median)
{
	/* The station stayed awake once: that interval is 76 ms longer, and one is shorter. */
	int64_t t = MS(300);

	arb_pred_add(&p, t);
	for (int k = 0; k < 7; k++) {
		t += 102400 + (k == 3 ? MS(76) : 0) - (k == 5 ? MS(40) : 0);
		arb_pred_add(&p, t);
	}
	zassert_equal(arb_pred_period(&p), 102400);
}

ZTEST(arb_pred, test_even_count_averages_the_middle)
{
	/* Four enables: three intervals, odd. Five: four intervals, the two middle ones averaged. */
	arb_pred_add(&p, 0);
	arb_pred_add(&p, 100000);
	arb_pred_add(&p, 202000);
	arb_pred_add(&p, 304000);
	zassert_equal(arb_pred_period(&p), 102000);
	arb_pred_add(&p, 410000);
	/* Intervals 100000, 102000, 102000, 106000: the middle two. */
	zassert_equal(arb_pred_period(&p), 102000);
}

ZTEST(arb_pred, test_the_ring_buffer_keeps_the_newest)
{
	int64_t t = 0;

	/* 40 intervals of 50 ms, then 8 of 102.4 ms: only the latter remain in the window. */
	for (int k = 0; k < 40; k++, t += MS(50)) {
		arb_pred_add(&p, t);
	}
	for (int k = 0; k < 8; k++, t += 102400) {
		arb_pred_add(&p, t);
	}
	zassert_equal(arb_pred_count(&p), ARB_PRED_N);
	zassert_equal(arb_pred_period(&p), 102400);
}

ZTEST(arb_pred, test_power_save_change_adapts_in_four_intervals)
{
	/* MAX modem (307.2 ms), then MIN (102.4 ms): the median follows after four intervals. */
	int64_t t = feed(MS(100), 307200, 8, NULL);

	zassert_equal(arb_pred_period(&p), 307200);
	for (int k = 0; k < 3; k++, t += 102400) {
		arb_pred_add(&p, t);
	}
	zassert_equal(arb_pred_period(&p), 307200, "two short intervals of seven");
	for (int k = 0; k < 2; k++, t += 102400) {
		arb_pred_add(&p, t);
	}
	zassert_equal(arb_pred_period(&p), 102400, "four short intervals of seven");
}

ZTEST(arb_pred, test_staleness_ends_the_periodic_wake)
{
	int64_t last = feed(MS(100), 102400, 8, NULL) - 102400;

	zassert_equal(arb_pred_state_at(&p, last + 4 * 102400), ARB_PRED_PERIODIC);
	/* Power save off, or Wi-Fi stopped: no enable for more than four periods. */
	zassert_equal(arb_pred_state_at(&p, last + 4 * 102400 + 1), ARB_PRED_NONE);
}

ZTEST(arb_pred, test_next_after_a_long_gap_stays_on_the_phase)
{
	int64_t last = feed(MS(100), 102400, 8, NULL) - 102400;

	/* A caller that looks again 2.5 periods later gets the third wake, not a new phase. */
	zassert_equal(arb_pred_next(&p, last + 2 * 102400 + 51200), last + 3 * 102400);
}

ZTEST(arb_pred, test_a_short_pair_after_a_release_does_not_move_the_phase)
{
	int64_t last = feed(MS(100), 102400, 8, NULL) - 102400;
	int64_t next_before = arb_pred_next(&p, last + MS(20));

	/* The SDR released at its deadline; the blob enables 330 us later and disables 350 us on. */
	zassert_false(arb_pred_wake(&p, last + MS(90), last + MS(90) + 350, MS(1)));
	zassert_equal(arb_pred_next(&p, last + MS(91)), next_before);
	zassert_equal(arb_pred_period(&p), 102400);
	/* The beacon wake 12 ms long is learned. */
	zassert_true(arb_pred_wake(&p, last + 102400, last + 102400 + MS(12), MS(1)));
	zassert_equal(arb_pred_newest(&p, 0U), last + 102400);
}

ZTEST_SUITE(arb_pred, NULL, NULL, before, NULL, NULL);
