/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A hot-swap kernel for rfprobe's stage ring_sink (apps/rfprobe/src/main.c, "probe ring"): the
 * same sink with one node in 16 scanned, not one in 64, so that `scanned` of the ring's line is about
 * four times as large for the same run, a count that tells at a glance which sink ran. Built
 * INTERNAL: its function runs from internal RAM (multiapp_kernel() in apps/multiapp/CMakeLists.txt).
 *
 *   multiapp stage load ring_sink sink_scan16     (the file on the volume), or
 *   python3 apps/multiapp/scripts/kswap.py --port <port> --stage ring_sink sink_scan16.llext
 *
 * It imports only what the host exports for rfprobe already (memcpy) and the app's own ring_dump,
 * which the multiapp build of rfprobe makes global for it.
 */

#include <string.h>

#include <esp_sdr/esp_sdr_diag16.h>
#include <esp_sdr/esp_sdr_place.h>

#include <multiapp/stage.h>

#include "ring_stage.h"

#define SCAN_MASK 15U

ESP_SDR_CODE(PACED_SAMPLE, sink_scan16)
static void sink_scan16(void *user, uint8_t *node, uint64_t index)
{
	struct ring_check *c = user;

	if (index != c->expect) {
		c->gaps++;
	}
	c->expect = index + ESP_SDR_DIAG16_PAYLOAD;
	if (((index / ESP_SDR_DIAG16_PAYLOAD) & SCAN_MASK) == 0U) {
		const int8_t *s = (const int8_t *)(node + ESP_SDR_DIAG16_HOLE);

		for (size_t j = 0; j < ESP_SDR_DIAG16_PAYLOAD; j++) {
			if (s[j] >= RING_OUTLIER || s[j] <= -RING_OUTLIER) {
				c->outliers++;
			}
		}
		c->scanned++;
	}
	if (index >= c->dump_from && c->dumped < c->nodes) {
		memcpy(&ring_dump[c->dumped * ESP_SDR_DIAG16_PAYLOAD], node + ESP_SDR_DIAG16_HOLE,
		       ESP_SDR_DIAG16_PAYLOAD);
		c->dumped++;
	}
}

MULTIAPP_KERNEL_DEFINE(ring_sink, esp_sdr_diag16_sink_t, sink_scan16);
