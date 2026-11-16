/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file
 * @brief The receive gain's clip guard: down a level when packets reach full scale, up again
 * after a quiet hold that grows when the source comes in bursts.
 */

#ifndef ESP_SDR_GAIN_GUARD_H_
#define ESP_SDR_GAIN_GUARD_H_

#include <stdbool.h>
#include <stdint.h>

/**
 * @addtogroup esp_sdr_gain_guard
 * @{
 *
 * A receiver at a high gain clips a transmitter close by: its packets come out with bit errors
 * or not at all. The guard watches two counters the receiver keeps, packets at full scale and
 * packets within 10 dB of it (or whatever margin the app counts as loud), and moves through the
 * app's gain levels, the first the highest: down a level after @ref
 * esp_sdr_gain_guard_cfg.down_n packets at full scale within @ref esp_sdr_gain_guard_cfg.down_ms,
 * up a level after a hold without a loud packet. The hold starts at @ref
 * esp_sdr_gain_guard_cfg.up_ms and doubles, up to @ref esp_sdr_gain_guard_cfg.up_max_ms, each time
 * the guard steps down again within @ref esp_sdr_gain_guard_cfg.bounce_ms of stepping up: a source
 * in bursts (a host scanning, then connecting) would otherwise find the gain back up for the first
 * packets of each burst. The longest hold at the first level with nothing loud brings the hold
 * back to the first. The levels' gains and their measured drops (for an RSSI calibration) stay
 * with the app.
 */

/** The guard's thresholds. */
struct esp_sdr_gain_guard_cfg {
	/** Gain levels, the first the highest. */
	uint8_t levels;
	/** Packets at full scale that step down... */
	uint8_t down_n;
	/** ...within this many milliseconds. */
	uint32_t down_ms;
	/** The first hold without a loud packet before a step up. */
	uint32_t up_ms;
	/** The longest hold; this long at the first level without a loud packet resets it. */
	uint32_t up_max_ms;
	/** A step down this soon after a step up doubles the hold. */
	uint32_t bounce_ms;
};

/** A guard's state. */
struct esp_sdr_gain_guard {
	const struct esp_sdr_gain_guard_cfg *cfg;
	/** The level now, 0 the highest gain. */
	uint8_t level;
	/** Packets at full scale counted towards a step down. */
	uint8_t nclip;
	/** The receiver's counters as last seen. */
	uint32_t clipped_seen, loud_seen;
	/** The quiet a level needs before the guard steps up. */
	uint32_t hold_ms;
	/** The first full scale packet of the count, the last loud packet. */
	int64_t t_clip, t_loud;
	/** The last step up. */
	int64_t t_up;
};

/** Start at the first level with the shortest hold, at @p now_ms. */
void esp_sdr_gain_guard_init(struct esp_sdr_gain_guard *g, const struct esp_sdr_gain_guard_cfg *cfg,
			     int64_t now_ms);

/**
 * The receiver's counters of packets at full scale and loud packets at @p now_ms (running totals,
 * wrapping); true when the level changed.
 */
bool esp_sdr_gain_guard_update(struct esp_sdr_gain_guard *g, int64_t now_ms, uint32_t clipped,
			       uint32_t loud);

/** @} */

#endif /* ESP_SDR_GAIN_GUARD_H_ */
