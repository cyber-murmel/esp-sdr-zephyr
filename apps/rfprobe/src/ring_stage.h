/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What "probe ring" (main.c) shares with its hot-swap kernels (kernels/sink_scan16.c): the sink's
 * state and constants. The ring sink is the stage ring_sink of the multiapp build
 * (include/multiapp/stage.h); standalone it is a plain function.
 */

#ifndef RFPROBE_RING_STAGE_H_
#define RFPROBE_RING_STAGE_H_

#include <stdint.h>

#include <esp_sdr/esp_sdr_diag16.h>

#define RING_DUMP_NODES 4U

struct ring_check {
	uint64_t expect;
	uint64_t dump_from;
	uint32_t gaps;
	uint32_t dumped;
	uint32_t nodes;
	/* Nodes looked at, and large samples in them: on a quiet band at a mid gain, about none. */
	uint32_t scanned;
	uint32_t outliers;
};

/* One node in 64 of the run: enough to see a phase that goes bad, cheap at any rate. */
#define RING_SCAN_MASK 63U
#define RING_OUTLIER   32

#if defined(MULTIAPP_NAME)
/* The first nodes after 100 ms, which "probe ring" dumps: global in the multiapp build only. */
extern uint8_t ring_dump[RING_DUMP_NODES * ESP_SDR_DIAG16_PAYLOAD];
#endif

#endif /* RFPROBE_RING_STAGE_H_ */
