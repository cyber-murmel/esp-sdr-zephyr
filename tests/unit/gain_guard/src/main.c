/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The clip guard (esp_sdr_gain_guard.h) with apps/ubertooth's thresholds and gains (ubt_rssi.h),
 * the cases its own suite had.
 */

#include <zephyr/ztest.h>

#include <esp_sdr/esp_sdr_gain_guard.h>

#define LEVELS 4U

static const struct esp_sdr_gain_guard_cfg cfg = {
	.levels = LEVELS,
	.down_n = 2,
	.down_ms = 2000,
	.up_ms = 10000,
	.up_max_ms = 160000,
	.bounce_ms = 60000,
};
static const uint8_t gain[LEVELS] = {60, 52, 44, 36};

/*
 * Down a level after two full scale packets within 2 s (not one, not two 3 s apart), never past
 * the last level; up again after 10 s without a packet within 10 dB of full scale, and not while
 * such packets keep coming.
 */
ZTEST(gain_guard, test_steps)
{
	struct esp_sdr_gain_guard g;
	uint32_t clipped = 0, loud = 0;
	int64_t t = 0;

	esp_sdr_gain_guard_init(&g, &cfg, t);
	zassert_equal(g.level, 0U);
	/* One, then another 3 s later: no step. */
	clipped++, loud++;
	zassert_false(esp_sdr_gain_guard_update(&g, t += 100, clipped, loud));
	clipped++, loud++;
	zassert_false(esp_sdr_gain_guard_update(&g, t += 3000, clipped, loud));
	/* The second within 2 s of that one: down. */
	clipped++, loud++;
	zassert_true(esp_sdr_gain_guard_update(&g, t += 500, clipped, loud));
	zassert_equal(g.level, 1U);
	zassert_equal(gain[g.level], 52U);
	/* Two at once: down again, and again, then the last level holds. */
	for (unsigned int l = 2; l < LEVELS + 2U; l++) {
		bool moved;

		clipped += 2, loud += 2;
		moved = esp_sdr_gain_guard_update(&g, t += 100, clipped, loud);
		zassert_equal(moved, l < LEVELS, "level %u", l);
	}
	zassert_equal(g.level, LEVELS - 1U);
	/* Loud packets (within 10 dB) every 5 s keep the level. */
	for (int k = 0; k < 6; k++) {
		loud++;
		zassert_false(esp_sdr_gain_guard_update(&g, t += 5000, clipped, loud));
	}
	/* Then quiet: up one level per 10 s, and no further than the first. */
	zassert_false(esp_sdr_gain_guard_update(&g, t += 9000, clipped, loud));
	for (unsigned int l = LEVELS - 1U; l > 0U; l--) {
		t += 1000 + (l == LEVELS - 1U ? 0 : 9000);
		zassert_true(esp_sdr_gain_guard_update(&g, t, clipped, loud));
		zassert_equal(g.level, l - 1U);
	}
	zassert_false(esp_sdr_gain_guard_update(&g, t += 60000, clipped, loud));
	zassert_equal(g.level, 0U);
}

/*
 * Against a source in bursts, 3 dB past full scale (clipped at the first level, within 10 dB of it
 * at the second): 2 s of packets every 30 s, the host scanning and connecting. The guard climbs
 * back up in the first gaps; coming back down within a minute of each step up doubles the hold
 * (10, 20, 40 s), and from the third burst on it keeps the level over the gaps. After the last
 * burst it climbs back once the hold has passed, and 160 s of quiet bring the hold back to 10 s.
 */
static int guard_poll(struct esp_sdr_gain_guard *g, int64_t t, uint32_t *clipped, uint32_t *loud,
		      bool packet)
{
	uint8_t before = g->level;

	if (packet) {
		(*loud)++;
		if (g->level == 0U) {
			(*clipped)++;
		}
	}
	if (!esp_sdr_gain_guard_update(g, t, *clipped, *loud)) {
		return 0;
	}
	return g->level > before ? 1 : -1;
}

ZTEST(gain_guard, test_bursts)
{
	struct esp_sdr_gain_guard g;
	uint32_t clipped = 0, loud = 0;
	unsigned int downs = 0, ups = 0, ups_late = 0;
	int64_t t;

	esp_sdr_gain_guard_init(&g, &cfg, 0);
	for (int b = 0; b < 10; b++) {
		int64_t t0 = (int64_t)b * 30000;

		for (t = t0; t < t0 + 2000; t += 250) {
			int m = guard_poll(&g, t, &clipped, &loud, true);

			downs += m > 0 ? 1U : 0U;
			ups += m < 0 ? 1U : 0U;
		}
		for (; t < t0 + 30000; t += 100) {
			int m = guard_poll(&g, t, &clipped, &loud, false);

			zassert_true(m <= 0, "down in a gap at %lld", (long long)t);
			ups += m < 0 ? 1U : 0U;
			ups_late += m < 0 && b >= 2 ? 1U : 0U;
		}
		if (b >= 2) {
			zassert_equal(g.level, 1U, "burst %d", b);
		}
	}
	zassert_equal(downs, 3U);
	zassert_equal(ups, 2U);
	zassert_equal(ups_late, 0U);
	zassert_equal(g.hold_ms, 40000U);
	/* The last packet at 271.75 s: back up 40 s later, not before. */
	zassert_false(esp_sdr_gain_guard_update(&g, 311700, clipped, loud));
	zassert_true(esp_sdr_gain_guard_update(&g, 311800, clipped, loud));
	zassert_equal(g.level, 0U);
	/* 160 s of quiet at the first level: the hold is back to 10 s. */
	zassert_false(esp_sdr_gain_guard_update(&g, 311800 + 159000, clipped, loud));
	zassert_equal(g.hold_ms, 40000U);
	zassert_false(esp_sdr_gain_guard_update(&g, 311800 + 160000, clipped, loud));
	zassert_equal(g.hold_ms, 10000U);
}

ZTEST_SUITE(gain_guard, NULL, NULL, NULL, NULL, NULL);
