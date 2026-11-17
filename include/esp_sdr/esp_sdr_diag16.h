/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file
 * @brief ESP-SDR gapless receive on the ESP32-S31 through the modem diagnostic bus.
 *
 * The bus carries I and Q, 10 bits each. The GPIO matrix loops the upper 8 of each through 16 pads
 * into the PARLIO receiver, a GDMA ring collects them without the CPU. The method is ESPARGOS
 * esp-sdr's S31 streaming firmware (main/targets/esp32s31/streaming).
 *
 * Bytes: signed I[9:2], then signed Q[9:2], usable as I + jQ. A lower rate takes every n-th sample
 * of the 80 MS/s bus: there is no digital filter, and the analog low-pass does not go below some
 * 13 MHz, so 10 and 5 MS/s alias.
 *
 * The 16 pads are driven as outputs that toggle at up to 40 MHz: nothing may drive them, whatever
 * they reach gets all that, and a slow pad spoils its bits (doc/samples/rings.md).
 *
 * The PARLIO clock comes from 160 MHz, the bus runs at 80 MHz: a start samples the bus either in the
 * middle of its bit or on its edge, about one start in two, whatever the rate or clock edge. On the
 * edge some lines take the next sample's bits: a value crossing zero comes out far from zero. Every
 * run looks first with the receiver off, where such values cannot be real, and slips the clock until
 * there are none and Q crossed zero often enough to tell; I's crossings do not count. Where the LO's
 * DC keeps Q off zero, it then looks with the receiver on, in moments with nothing on the air.
 */

#ifndef ESP_SDR_ESP_SDR_DIAG16_H_
#define ESP_SDR_ESP_SDR_DIAG16_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup esp_sdr_diag16
 * @{
 */

/** Pads, and data lines of the receiver. */
#define ESP_SDR_DIAG16_PINS 16U

/** A ring node. A sink gets the whole node: the samples start ESP_SDR_DIAG16_HOLE into it. */
#define ESP_SDR_DIAG16_NODE 4096U

/**
 * The DMA leaves this many bytes at the start of every node, for a header in front of the samples:
 * nodes side by side are then a run of blocks that needs no copying.
 */
#define ESP_SDR_DIAG16_HOLE 16U

/** Sample bytes in a node. */
#define ESP_SDR_DIAG16_PAYLOAD (ESP_SDR_DIAG16_NODE - ESP_SDR_DIAG16_HOLE)

/**
 * Receives one node.
 *
 * @param user cfg.user.
 * @param node ESP_SDR_DIAG16_NODE bytes in the ring. Valid until the DMA comes round again, which
 *             esp_sdr_diag16_node_valid() tells, and the DMA does not wait; the hole is the sink's.
 * @param index Sample bytes before this node since the start; a jump from the previous index plus
 *              ESP_SDR_DIAG16_PAYLOAD means the consumer fell behind and nodes were skipped.
 */
typedef void (*esp_sdr_diag16_sink_t)(void *user, uint8_t *node, uint64_t index);

struct esp_sdr_diag16_cfg {
	/**
	 * Sixteen GPIO numbers, or NULL for the defaults: the devicetree's zephyr,user property
	 * esp-sdr-diag-pads, else the ESP32-S31-Function-CoreBoard-1's free header pins.
	 */
	const int8_t *pins;
	/**
	 * Samples per second, exactly 160 MHz over an even whole number: 40, 20, 16, 10, 8, 5, 4, 3.2,
	 * 2.5, 2, 1.6, 1.25, 1, 0.8 or 0.64 MS/s. Anything else is -EINVAL.
	 */
	uint32_t rate_hz;
	/** Sample on the falling PARLIO clock edge instead of the rising one. */
	bool falling_edge;
	/** Run time in seconds, 0 to run until esp_sdr_diag16_stop(). */
	uint32_t seconds;
	/** Wake the consumer every this many nodes, at most half the ring; 0 means 1. */
	uint8_t wake_nodes;
	/** Take the DC off with the baseband's DC DACs before the first node (a fixed gain only). */
	bool dc_correct;
	/** Return -EIO rather than run on with stats.phase_bad set. */
	bool phase_required;
	/** Called from the thread that called esp_sdr_diag16_run(). */
	esp_sdr_diag16_sink_t sink;
	void *user;
};

struct esp_sdr_diag16_stats {
	/** Sample bytes the ring received. */
	uint64_t bytes;
	/** Sample bytes handed to the sink. */
	uint64_t consumed;
	/** Times the consumer fell a ring behind and skipped ahead. */
	uint32_t skips;
	/** Nodes skipped by those. */
	uint32_t skipped_nodes;
	/** The interrupt fell a whole ring behind the hardware (data lost, run aborted). */
	bool overrun;
	/** Ring byte count against the system timer, in 0.01 ppm; 0 until some time has run. */
	int32_t ppm_x100;
	/** CPU cycles per sample byte spent in the sink, in 0.01 cycles. */
	uint32_t sink_cycles_x100;
	/** Bytes per second the PARLIO clock delivers. */
	uint32_t byte_rate;
	/** Clock slips until the sampling phase was clean. */
	uint8_t phase_tries;
	/**
	 * No clean phase was found: no look, receiver off or on at any gain, had Q crossing zero often
	 * enough with next to no large value.
	 */
	bool phase_bad;
	/** The servo's error (cfg.dc_correct); its numbers in stats. */
	int16_t dc_ret;
	/** DC before and after it, in 1/256 of an 8-bit step, I then Q. */
	int32_t dc_before[2], dc_after[2];
	/** Debugging: ring interrupts, the raw DMA status. */
	uint32_t isrs, dma_raw;
};

/**
 * @brief Run the ring until the time is up, esp_sdr_diag16_stop() or an overrun.
 *
 * Takes the radio for the whole run; the receiver keeps its frequency, gain and filter, but not an
 * earlier DC calibration: cfg.dc_correct makes a new one.
 *
 * A sink slower than the DMA is no error: nodes get skipped (stats->skips), and the run still ends
 * on time or on a stop.
 *
 * @return 0, -EINVAL for a bad configuration, -EAGAIN before esp_sdr_init(), -EBUSY when already
 *         running, -ETIMEDOUT if the ring got no data or stopped getting it, -EOVERFLOW when the
 *         interrupt could not keep up (stats->overrun), -EIO for no clean sampling phase and
 *         cfg.phase_required.
 */
int esp_sdr_diag16_run(const struct esp_sdr_diag16_cfg *cfg, struct esp_sdr_diag16_stats *stats);

/**
 * @brief Ask a running esp_sdr_diag16_run() to return.
 *
 * With no run in progress it does nothing: a run that starts just after has not seen it, so a
 * caller with a reason to stop must look at that reason again from its sink.
 */
void esp_sdr_diag16_stop(void);

/** @return true while esp_sdr_diag16_run() is in progress. */
bool esp_sdr_diag16_active(void);

/** @return Sample bytes the DMA has finished, counted as the sink's index: compare the two. */
uint64_t esp_sdr_diag16_produced(void);

/**
 * @brief Is the node a sink got at @p index still as it was?
 *
 * For a consumer that keeps or copies nodes: copy first, ask after.
 *
 * @param index The index the sink got.
 * @return false when the DMA may have started on that node again.
 */
bool esp_sdr_diag16_node_valid(uint64_t index);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ESP_SDR_ESP_SDR_DIAG16_H_ */
