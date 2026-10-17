/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The receive engine behind esdr.rx_*(): the ring on CPU 1 fills a caller's buffer with
 * interleaved int16 I/Q (full scale ESP_SDR_RING_FULL_SCALE).
 */

#ifndef ESDR_RX_H_
#define ESDR_RX_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct esdr_rx_stats {
	/* Samples stored (or counted, without a buffer); ring runs, restarts after a ring error. */
	uint64_t samples;
	uint32_t runs, errors;
	/* Holes in the output index within a run (lost input), and their samples. */
	uint32_t gaps;
	uint64_t gap_samples;
	/* The last ring error: enum esp_sdr_ring_status and its detail. */
	int last_status;
	uint32_t last_detail;
};

/* Brings the radio up and starts the engine's thread (once). */
int esdr_rx_init(void);

/*
 * Starts a capture of @p count samples (0: until esdr_rx_stop() or the buffer is full) at
 * 16 MS/s / @p decim. @p buf holds @p cap samples (int16 I, Q), or is NULL to only count.
 */
int esdr_rx_start(unsigned int decim, bool shift_fs4, int16_t *buf, size_t cap, uint64_t count,
		  bool monitor);

/*
 * With monitor set, the capture also keeps its latest samples in a ring for displays:
 * copies the latest  n of them (int16 I, Q; zeros before the first) to  dst and returns
 * how many samples the capture has produced so far.
 */
uint64_t esdr_rx_latest(int16_t *dst, size_t n);

/* Monitor ring length in samples. */
#define ESDR_MON_SAMPLES 8192U

/* Asks a running capture to end; esdr_rx_busy() turns false once it has. */
void esdr_rx_stop(void);

bool esdr_rx_busy(void);

/* Waits up to @p timeout_ms for the capture to end; true when it has. */
bool esdr_rx_wait(int32_t timeout_ms);

void esdr_rx_get_stats(struct esdr_rx_stats *stats);

#endif /* ESDR_RX_H_ */
