/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit test of the S3 ring's geometry (esp_sdr_ring_geom.h): for every unit length the API
 * allows (a multiple of 64 from 1024 to 12288) and several lateness limits, the windows hold
 * the lateness, do not overlap and fit the ring; 0 is the old formula; the histogram bins.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include "esp_sdr_ring_geom.h"

static const uint32_t lates[] = {0, 64, 100, 256, 512, 896, 1000, 2000};

ZTEST(ring_geom, test_invariants)
{
	for (uint32_t unit = ESP_SDR_RING_UNIT_MIN; unit <= ESP_SDR_RING_UNIT_DEFAULT; unit += 64U) {
		for (size_t i = 0; i < ARRAY_SIZE(lates); i++) {
			struct geom g, old;

			zassert_equal(geom_init(&g, unit, lates[i]), 0, "unit %u late %u", unit,
				      lates[i]);
			zassert_equal(g.thr, unit);
			/* A unit starts up to late after its earliest start, ends up to 2 late. */
			zassert_true(g.late + 64U < g.sg, "start window %u unit %u", g.sg, unit);
			zassert_true(2U * g.late + 64U < g.eg, "end window %u unit %u", g.eg, unit);
			/* The start window ends inside the unit: no overlap with the end window. */
			zassert_true(g.sg <= g.thr);
			zassert_true(g.sg + g.thr <= RING_PAIRS && g.eg + g.thr <= RING_PAIRS);
			/* The longest accepted unit fits the end window. */
			zassert_true(g.thr + g.late + 64U <= g.thr + g.eg);
			zassert_true(g.late >= 64U && g.late <= 2000U);
			zassert_true(g.slice >= 1U && g.slice <= SLICE_PAIRS);
			zassert_true(g.min0 <= g.thr && g.sg0 <= RING_PAIRS);
			/* A setting never widens the old geometry. */
			zassert_equal(geom_init(&old, unit, 0), 0);
			zassert_true(g.late <= old.late && g.sg <= old.sg && g.eg <= old.eg);
			if (lates[i] == 0U) {
				zassert_equal(g.late, MIN(2000U, MIN(3072U, unit) - 128U));
				zassert_equal(g.sg, MIN(3072U, unit));
				zassert_equal(g.eg, MIN(4096U, 2U * g.late + 128U));
			} else {
				zassert_equal(g.late, MIN(old.late, lates[i]));
				zassert_equal(g.sg, g.late + 128U);
				zassert_equal(g.eg, MIN(4096U, 2U * g.late + 128U));
			}
		}
	}
}

ZTEST(ring_geom, test_sentinel_words)
{
	struct geom g, old;

	zassert_equal(geom_init(&old, 1024U, 0), 0);
	zassert_equal(old.sg + old.eg, 2944U);
	zassert_equal(geom_init(&old, 12288U, 0), 0);
	zassert_equal(old.sg + old.eg, 7168U);
	zassert_equal(geom_init(&g, 1024U, 256U), 0);
	zassert_equal(g.sg + g.eg, 384U + 640U);
	zassert_equal(geom_init(&g, 12288U, 256U), 0);
	zassert_equal(g.sg + g.eg, 384U + 640U);
}

ZTEST(ring_geom, test_bad)
{
	struct geom g;

	zassert_equal(geom_init(&g, 1000U, 0), -EINVAL);
	zassert_equal(geom_init(&g, 960U, 0), -EINVAL);
	zassert_equal(geom_init(&g, 12288U + 64U, 0), -EINVAL);
	zassert_equal(geom_init(&g, 4096U, 63U), -EINVAL);
	zassert_equal(geom_init(&g, 0, 0), 0);
	zassert_equal(g.thr, ESP_SDR_RING_UNIT_DEFAULT);
	zassert_equal(g.sg, 3072U);
	zassert_equal(g.eg, 4096U);
	zassert_equal(g.late, 2000U);
}

ZTEST(ring_geom, test_hist_bins)
{
	zassert_equal(esp_sdr_ring_late_bin(0), 0U);
	zassert_equal(esp_sdr_ring_late_bin(1), 1U);
	zassert_equal(esp_sdr_ring_late_bin(2), 2U);
	zassert_equal(esp_sdr_ring_late_bin(3), 2U);
	zassert_equal(esp_sdr_ring_late_bin(4), 3U);
	zassert_equal(esp_sdr_ring_late_bin(137), 8U);
	zassert_equal(esp_sdr_ring_late_bin(2000), 11U);
	zassert_equal(esp_sdr_ring_late_bin(2048), 12U);
	zassert_equal(esp_sdr_ring_late_bin(0xffffffffU), 12U);
}

ZTEST(ring_geom, test_hist_str)
{
	uint32_t h[ESP_SDR_RING_LATE_BINS] = {5, 0, 2, 0, 0, 0, 0, 0, 1, 0, 0, 0, 3};
	char b[64];

	esp_sdr_ring_late_hist_str(h, b, sizeof(b));
	zassert_str_equal(b, "0:5 2-3:2 128-255:1 2048+:3");
	memset(h, 0, sizeof(h));
	esp_sdr_ring_late_hist_str(h, b, sizeof(b));
	zassert_str_equal(b, "-");
}

ZTEST_SUITE(ring_geom, NULL, NULL, NULL, NULL, NULL);
