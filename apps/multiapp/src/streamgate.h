/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Whether a stream that runs is one to protect (src/netsvc.c). No Zephyr here: tests/host/test_streamgate.py
 * builds it for the PC.
 */

#ifndef MULTIAPP_STREAMGATE_H_
#define MULTIAPP_STREAMGATE_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * A ring that runs is a stream while its consumer, the PC reading the SDR's USB class, keeps up: it finished an
 * IN transfer less than @p window_ms ago, or, as long as it has not finished one since the ring was first seen
 * (an earlier stream's last transfer is no news), the ring was first seen less than @p window_ms ago.
 *
 * All in milliseconds of a 32 bit uptime that wraps: ages are unsigned differences. @p last_in was read before
 * @p now (so it is never after it); 0 means no transfer yet.
 */
static inline bool stream_consumer_live(uint32_t now, uint32_t last_in, uint32_t ring_since, uint32_t window_ms)
{
	uint32_t age_ring = now - ring_since;
	uint32_t age_in = now - last_in;
	uint32_t age;

	if ((int32_t)age_in < 0 && age_in > UINT32_MAX - window_ms) {
		/* a transfer stamped after "now" (a later millisecond, read in between): it is the newest there is */
		age_in = 0U;
	}
	age = (last_in != 0U && age_in < age_ring) ? age_in : age_ring;

	return age < window_ms;
}

#endif /* MULTIAPP_STREAMGATE_H_ */
