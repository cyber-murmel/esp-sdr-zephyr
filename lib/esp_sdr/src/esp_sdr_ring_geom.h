/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The S3 ring's unit geometry from its unit length: plain C, unit tested on native_sim
 * (tests/unit/ring_geom).
 */

#ifndef ESP_SDR_RING_GEOM_H_
#define ESP_SDR_RING_GEOM_H_

#include <errno.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include <esp_sdr/esp_sdr_ring.h>

#define RING_PAIRS     16384U
#define RING_MASK      (RING_PAIRS - 1U)
/* Unit 0: its whole bank holds sentinels; stay clear of its start. */
#define UNIT0_END_WIN  1024U

/* Filter work per slice at most, and the input chunk through both stages. */
#define SLICE_PAIRS 1024U
#define FIR_CHUNK   512U

/*
 * The unit geometry of a run, from its unit length (esp_sdr_ring_cfg):
 * the engine moves on after thr pairs, at most late pairs late; sentinels
 * cover sg pairs from a unit's earliest start and eg from its earliest end.
 * The default unit: thr 12288, sg 3072, eg 4096, late 2000.
 */
struct geom {
	uint32_t thr, sg, eg, late;
	/* Unit 0: start window, shortest length. */
	uint32_t sg0, min0;
	uint32_t slice;
};

/*
 * @p late_cfg 0: the lateness allowed is as much as the windows can hold (sg - 128, 2000 at
 * most, and sg is the unit length up to 3072). Else it caps that (CONFIG_ESP_SDR_RING_LATE_PAIRS):
 * a unit starts within late of its earliest start and ends within 2 late of its earliest end, so
 * the windows shrink with it (sg late + 128, eg 2 late + 128) and the sentinel words a unit drop
 * from sg + eg. A switch later than late is the LATE error and a restart, as before.
 */
static inline int geom_init(struct geom *g, uint32_t unit, uint32_t late_cfg)
{
	if (unit == 0U) {
		unit = ESP_SDR_RING_UNIT_DEFAULT;
	}
	if (unit < ESP_SDR_RING_UNIT_MIN || unit > ESP_SDR_RING_UNIT_DEFAULT || unit % 64U != 0U) {
		return -EINVAL;
	}
	g->thr = unit;
	/* A start window longer than the unit would overlap the end window. */
	g->sg = MIN(3072U, unit);
	g->late = MIN(2000U, g->sg - 128U);
	if (late_cfg != 0U) {
		if (late_cfg < 64U) {
			return -EINVAL;
		}
		g->late = MIN(g->late, late_cfg);
		g->sg = g->late + 128U;
	}
	g->eg = MIN(4096U, 2U * g->late + 128U);
	g->sg0 = 1024U + MIN(2048U, unit / 2U);
	g->min0 = unit - MIN(1024U, unit / 2U);
	g->slice = MIN(SLICE_PAIRS, unit / 4U);
	if (g->late + 64U >= g->sg || 2U * g->late + 64U >= g->eg ||
	    g->sg > g->thr || g->sg + g->thr > RING_PAIRS || g->eg + g->thr > RING_PAIRS) {
		return -EINVAL;
	}
	return 0;
}

#endif /* ESP_SDR_RING_GEOM_H_ */
