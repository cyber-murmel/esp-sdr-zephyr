/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Device side of the osmosdr protocol (esdr_proto.h): the request checks and reply fields
 * every device shares, the osmosdr app on the S3 and osmosdr_serial. Stateless: each device
 * keeps its own request switch, settings, locking and change tracking. Header-only.
 */

#ifndef ESDR_DEV_H_
#define ESDR_DEV_H_

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <esp_sdr/esp_sdr.h>

#include "esdr_proto.h"

/* Nearest of the rates max_hz >> k, k = 0 .. count - 1, on a log scale. */
static inline uint32_t esdr_rate_round(uint32_t max_hz, uint32_t count, uint32_t hz)
{
	uint32_t min_hz = max_hz >> (count - 1U);

	/* Also keeps 2 hz^2 below 2^64. */
	if (hz >= max_hz) {
		return max_hz;
	}
	for (uint32_t r = max_hz; r > min_hz; r /= 2U) {
		/* Above the geometric mean of r and r / 2. */
		if (2U * (uint64_t)hz * hz >= (uint64_t)r * r) {
			return r;
		}
	}
	return min_hz;
}

/* ESDR_REQ_SET_FREQ: within the software tuning limits. */
static inline bool esdr_freq_ok(uint64_t hz)
{
	return hz >= (uint64_t)ESP_SDR_FREQ_MIN_MHZ * 1000000U &&
	       hz <= (uint64_t)ESP_SDR_FREQ_MAX_MHZ * 1000000U;
}

/* ESDR_REQ_SET_DIGITAL_GAIN: the default, or whole 6 dB steps up to the maximum. */
static inline bool esdr_dgain_ok(uint16_t value)
{
	return value == ESDR_DGAIN_DEFAULT || (value <= ESDR_DGAIN_MAX_DB && value % 6U == 0U);
}

/* ESDR_REQ_GET_INFO: the reply, receive rates as for esdr_rate_round(). */
static inline void esdr_info_init(struct esdr_info *info, uint32_t rate_max_hz,
				  uint16_t rate_count, uint16_t rx_gain_max, uint16_t tx_gain_max,
				  const char *firmware)
{
	size_t n;

	memset(info, 0, sizeof(*info));
	info->magic = ESDR_INFO_MAGIC;
	info->proto_version = ESDR_PROTO_VERSION;
	info->info_size = sizeof(*info);
	info->freq_min_hz = (uint64_t)ESP_SDR_FREQ_MIN_MHZ * 1000000U;
	info->freq_max_hz = (uint64_t)ESP_SDR_FREQ_MAX_MHZ * 1000000U;
	info->rx_rate_max_hz = rate_max_hz;
	info->rx_rate_count = rate_count;
	info->rx_gain_max = rx_gain_max;
	info->tx_gain_max = tx_gain_max;
	info->block_bytes = ESDR_BLOCK_BYTES;
	info->full_scale16 = 32767;
	/* Cut to the field, which keeps a terminating zero. */
	n = strlen(firmware);
	memcpy(info->firmware, firmware, n < sizeof(info->firmware) ? n : sizeof(info->firmware) - 1U);
}

#endif /* ESDR_DEV_H_ */
