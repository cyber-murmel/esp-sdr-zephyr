/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit boundaries of a dump ring by marker words (h0m3us3r/eSpDR): the CPU fills the windows
 * where a unit may start and end with markers, the engine overwrites them with samples, and a
 * binary search finds where the samples begin or end. Shared by the S3 ring and the C6
 * gapless ring; each fills its markers its own way.
 */

#ifndef ESP_SDR_MARK_H_
#define ESP_SDR_MARK_H_

#include <stdint.h>

#include <zephyr/toolchain.h>

#define ESP_SDR_MARK           0xa5c33c5aU
#define ESP_SDR_MARK_NOT_FOUND 0xffffffffU

/*
 * The searches read the ring with plain loads (a volatile pointer costs the S3 a memw per
 * probe); the barrier keeps the compiler from reusing loads of an earlier search.
 */

/* The first written word in [origin, origin + guard) of ring @p p: markers up to it, data on. */
static ALWAYS_INLINE uint32_t esp_sdr_mark_first(const uint32_t *p, uint32_t mask,
						 uint32_t origin, uint32_t guard)
{
	uint32_t lo = 0, hi = guard - 1U;

	__asm__ volatile("" ::: "memory");
	if (p[origin & mask] != ESP_SDR_MARK || p[(origin + guard - 1U) & mask] == ESP_SDR_MARK) {
		return ESP_SDR_MARK_NOT_FOUND;
	}
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2U;

		if (p[(origin + mid) & mask] == ESP_SDR_MARK) {
			lo = mid + 1U;
		} else {
			hi = mid;
		}
	}
	return (origin + lo) & mask;
}

/* One past the last written word in [origin, origin + guard), from the last index seen, @p wi. */
static ALWAYS_INLINE uint32_t esp_sdr_mark_end(const uint32_t *p, uint32_t mask, uint32_t origin,
					       uint32_t wi, uint32_t guard)
{
	uint32_t lo = (wi - origin) & mask, hi = guard - 1U;

	__asm__ volatile("" ::: "memory");
	if (lo >= guard || p[(origin + guard - 1U) & mask] != ESP_SDR_MARK) {
		return ESP_SDR_MARK_NOT_FOUND;
	}
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2U;

		if (p[(origin + mid) & mask] == ESP_SDR_MARK) {
			hi = mid;
		} else {
			lo = mid + 1U;
		}
	}
	return (origin + lo) & mask;
}

#endif /* ESP_SDR_MARK_H_ */
