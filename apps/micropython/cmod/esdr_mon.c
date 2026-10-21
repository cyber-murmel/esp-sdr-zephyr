/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The monitor of the radios behind esdr_remote.h (esdr_mon.h).
 */

#include <string.h>

#include <zephyr/sys/util.h>

#include "esdr_defs.h"
#include "esdr_mon.h"

uint32_t esdr_mon[ESDR_MON_SAMPLES] ESDR_PSRAM;
volatile uint64_t esdr_mon_total;
volatile bool esdr_mon_read;

/* The last ESDR_MON_SAMPLES of @p n pairs, up to the monitor's wrap at a time. */
static void put(const void *iq, uint32_t n, bool narrow)
{
	uint32_t skip = n > ESDR_MON_SAMPLES ? n - ESDR_MON_SAMPLES : 0U;

	for (uint32_t k = skip; k < n;) {
		uint32_t at = (uint32_t)((esdr_mon_total + k) % ESDR_MON_SAMPLES);
		uint32_t m = MIN(n - k, ESDR_MON_SAMPLES - at);

		if (narrow) {
			esdr_widen((int16_t *)&esdr_mon[at], (const int8_t *)iq + 2U * k, m);
		} else {
			memcpy(&esdr_mon[at], (const int16_t *)iq + 2U * k,
			       m * sizeof(esdr_mon[0]));
		}
		k += m;
	}
	esdr_mon_total += n;
}

void esdr_mon_put_s16(const int16_t *iq, uint32_t n)
{
	put(iq, n, false);
}

void esdr_mon_put_s8(const int8_t *iq, uint32_t n)
{
	put(iq, n, true);
}

uint64_t esdr_rx_latest(int16_t *dst, size_t n)
{
	uint32_t *out = (uint32_t *)dst;
	uint64_t total = esdr_mon_total;
	size_t have = (size_t)MIN(total, (uint64_t)ESDR_MON_SAMPLES);
	size_t lead = 0;

	n = MIN(n, (size_t)ESDR_MON_SAMPLES);
	if (have < n) {
		lead = n - have;
		memset(out, 0, lead * sizeof(*out));
	}
	for (size_t k = lead; k < n; k++) {
		out[k] = esdr_mon[(total - (n - k)) % ESDR_MON_SAMPLES];
	}
	esdr_mon_read = true;
	return total;
}
