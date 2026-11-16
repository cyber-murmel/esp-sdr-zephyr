/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The threshold servo's decision: the expected steps come from the rules in Kconfig's
 * ESP_SDR_GAPLESS_THR_LINK_PCT and _LOST_PCT, on a budget of 27500 bytes (11 MB/s for the
 * servo's 2.5 ms).
 */

#include <stdint.h>

#include <zephyr/ztest.h>

#include "esp_sdr_gapless_servo.h"

#define BUDGET 27500U

/* Headroom 5 and 10 %, the link's limit at 95 %. */
static enum thr_step step(uint32_t sent, uint32_t lost, uint32_t lost_pct)
{
	return thr_servo_step(sent, sent, lost, BUDGET, 5U, 10U, lost_pct);
}

ZTEST(gapless_servo, test_no_tolerance_any_loss_raises_fast)
{
	zassert_equal(step(10000U, 1U, 0U), THR_STEP_UP_FAST);
	zassert_equal(step(0U, 1U, 0U), THR_STEP_UP_FAST);
	zassert_equal(step(10000U, 0U, 0U), THR_STEP_DOWN);
}

ZTEST(gapless_servo, test_tolerance_holds_up_to_the_share)
{
	/* 5 % of 10000 bytes is 500. */
	zassert_equal(step(10000U, 500U, 5U), THR_STEP_DOWN);
	zassert_equal(step(10000U, 501U, 5U), THR_STEP_UP_FAST);
}

ZTEST(gapless_servo, test_loss_with_nothing_kept_raises_fast)
{
	/* A window kept in the period before and lost in this one. */
	zassert_equal(step(0U, 1U, 5U), THR_STEP_UP_FAST);
}

ZTEST(gapless_servo, test_over_the_budget_raises_fast_whatever_the_tolerance)
{
	zassert_equal(step(BUDGET + 1U, 0U, 0U), THR_STEP_UP_FAST);
	zassert_equal(step(BUDGET + 1U, 0U, 50U), THR_STEP_UP_FAST);
	zassert_equal(step(BUDGET, 0U, 0U), THR_STEP_UP);
}

ZTEST(gapless_servo, test_link_band_at_95_percent)
{
	/* 95 % of the budget is 26125, 90 % is 24750. */
	zassert_equal(step(26126U, 0U, 0U), THR_STEP_UP);
	zassert_equal(step(26125U, 0U, 0U), THR_STEP_HOLD);
	zassert_equal(step(24750U, 0U, 0U), THR_STEP_HOLD);
	zassert_equal(step(24749U, 0U, 0U), THR_STEP_DOWN);
}

ZTEST(gapless_servo, test_no_headroom_fills_the_link)
{
	/* The limit at 100 %: only passing the budget raises it. */
	zassert_equal(thr_servo_step(BUDGET, BUDGET, 0U, BUDGET, 0U, 5U, 0U), THR_STEP_HOLD);
	zassert_equal(thr_servo_step(BUDGET + 1U, BUDGET + 1U, 0U, BUDGET, 0U, 5U, 0U),
		      THR_STEP_UP_FAST);
}

ZTEST(gapless_servo, test_width_records_count_as_what_they_sent)
{
	/* 40000 bytes kept went out as 10000: well under the budget, and 2000 lost is 5 %. */
	zassert_equal(thr_servo_step(40000U, 10000U, 2000U, BUDGET, 5U, 10U, 5U), THR_STEP_DOWN);
	zassert_equal(thr_servo_step(40000U, 10000U, 2001U, BUDGET, 5U, 10U, 5U),
		      THR_STEP_UP_FAST);
}

ZTEST(gapless_servo, test_largest_inputs_do_not_wrap)
{
	const uint32_t big = UINT32_MAX / 100U;

	/* Half of it lost, none tolerated beyond 50 %: exactly at the share. */
	zassert_equal(thr_servo_step(big, 0U, big / 2U, BUDGET, 5U, 10U, 50U), THR_STEP_DOWN);
	zassert_equal(thr_servo_step(big, 0U, big, BUDGET, 5U, 10U, 50U), THR_STEP_UP_FAST);
}

ZTEST_SUITE(gapless_servo, NULL, NULL, NULL, NULL, NULL);
