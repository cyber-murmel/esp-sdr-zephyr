/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What the shell may set (txs_limits.h): the RF band with the tone's offset, and the knobs' limits.
 */

#include <zephyr/ztest.h>

#include "txs_limits.h"

ZTEST_SUITE(txs_limits, NULL, NULL, NULL, NULL, NULL);

ZTEST(txs_limits, test_tone_khz)
{
	/* 18 cycles per 351 samples at 20 MS/s: 1025.6 kHz, rounded up */
	zassert_equal(txs_tone_khz(18, 20), 1026U);
	/* k = 175 at 40 MS/s: 19943.02 kHz, rounded up */
	zassert_equal(txs_tone_khz(175, 40), 19944U);
	zassert_equal(txs_tone_khz(351, 20), 20000U);
}

ZTEST(txs_limits, test_rf_centre_and_offset)
{
	/* the default: 2437 with the default tone */
	zassert_true(txs_rf_ok(2437, 0, 18, 20));
	/* not tuned at all */
	zassert_false(txs_rf_ok(0, 0, 18, 20));
	/* outside the band, as `tx init 2890` leaves the hardware */
	zassert_false(txs_rf_ok(2890, 0, 18, 20));
	zassert_false(txs_rf_ok(2200, 0, 18, 20));
	/* the band is 2400 to 2483.5 with a guard of 1 MHz: the tone's image counts as well */
	zassert_false(txs_rf_ok(2400, 0, 1, 20), "carrier at the edge");
	zassert_false(txs_rf_ok(2401, 0, 18, 20), "tone 1.03 MHz down: below the guard");
	zassert_true(txs_rf_ok(2403, 0, 18, 20));
	zassert_false(txs_rf_ok(2483, 0, 1, 20), "carrier at the upper edge");
	zassert_true(txs_rf_ok(2481, 0, 18, 20));
	zassert_false(txs_rf_ok(2482, 0, 18, 20), "up by the tone: 2483.03");
}

ZTEST(txs_limits, test_rf_wide_tone)
{
	/* k = 175 at 40 MS/s is 19.9 MHz either side of the carrier */
	zassert_true(txs_rf_ok(2437, 0, 175, 40), "2417 to 2457");
	zassert_false(txs_rf_ok(2410, 0, 175, 40), "the image at 2390");
	zassert_false(txs_rf_ok(2470, 0, 175, 40), "the tone at 2490");
	zassert_false(txs_rf_ok(2410, 0, 175, 20), "9.97 MHz at 20 MS/s: 2400.03, inside the guard");
	zassert_true(txs_rf_ok(2412, 0, 175, 20), "2402.03");
}

ZTEST(txs_limits, test_rf_lo_offset)
{
	/* the LO offset moves the carrier */
	zassert_true(txs_rf_ok(2437, 5000, 18, 20));
	zassert_false(txs_rf_ok(2437, 45000, 18, 20), "carrier at 2482: the tone passes the edge");
	zassert_false(txs_rf_ok(2437, -40000, 18, 20), "carrier at 2397");
	zassert_true(txs_rf_ok(2437, 20000, 18, 20));
}

ZTEST(txs_limits, test_knobs)
{
	zassert_true(txs_lat_ok(0), "0 measures");
	zassert_false(txs_lat_ok(1));
	zassert_false(txs_lat_ok(19));
	zassert_true(txs_lat_ok(20));
	zassert_true(txs_lat_ok(200));
	zassert_false(txs_lat_ok(201));
	zassert_true(txs_ovh_ok(0));
	zassert_true(txs_ovh_ok(2000));
	zassert_false(txs_ovh_ok(2001));
	zassert_true(txs_tol_ok(1024));
	zassert_false(txs_tol_ok(1025));
	zassert_true(txs_stall_ok(50));
	zassert_false(txs_stall_ok(51));
}
