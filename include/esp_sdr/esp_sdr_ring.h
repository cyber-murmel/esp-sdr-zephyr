/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file
 * @brief ESP-SDR continuous receive: gapless, decimated I/Q from the 16 MS/s ring.
 *
 * The dump engine runs in circular mode and rotates through three SRAM
 * banks; a two-stage decimating FIR (vector unit) filters every finished
 * bank. Needs CONFIG_ESP_SDR_RING, which reserves SRAM banks 0 to 2.
 */

#ifndef ESP_SDR_ESP_SDR_RING_H_
#define ESP_SDR_ESP_SDR_RING_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_sdr/esp_sdr.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Ring sample rate; outputs come at this rate divided by the decimation. */
#define ESP_SDR_RING_RATE_HZ 16000000U

/** Decimation limits (powers of two). */
#define ESP_SDR_RING_DECIM_MIN 64U
#define ESP_SDR_RING_DECIM_MAX 1024U

/** Output full scale: the raw 10-bit full scale times 32. */
#define ESP_SDR_RING_FULL_SCALE 16384

/**
 * Output callback, called from the ring loop with interrupts masked on its
 * CPU: copy the samples and return quickly, never block. @p index is the
 * output sample index of i[0] and q[0] since esp_sdr_ring_run() started;
 * a jump from the previous call's index + n is a gap (lost input).
 */
typedef void (*esp_sdr_ring_sink_t)(void *user, const int16_t *i, const int16_t *q, size_t n,
				    uint64_t index);

struct esp_sdr_ring_cfg {
	/** ESP_SDR_RING_DECIM_MIN to ESP_SDR_RING_DECIM_MAX, a power of two. */
	unsigned int decim;
	/**
	 * Mix by -fs/4 before filtering: the output is centred 4 MHz above
	 * the LO, keeping the LO leakage and the 1/f noise at 0 Hz out of it.
	 */
	bool shift_fs4;
	esp_sdr_ring_sink_t sink;
	void *user;
};

/** Why a run ended early. */
enum esp_sdr_ring_status {
	ESP_SDR_RING_OK = 0,
	/** A bank switch came later than the ring allows (detail: pairs written). */
	ESP_SDR_RING_LATE,
	/** A bank was held longer than the ring allows (detail: cycles). */
	ESP_SDR_RING_AGE,
	/** A unit did not start where the previous one ended. */
	ESP_SDR_RING_START,
	/** A unit's end was not inside its sentinel window. */
	ESP_SDR_RING_END,
	/** A unit of implausible length (detail: pairs). */
	ESP_SDR_RING_LENGTH,
};

struct esp_sdr_ring_stats {
	/** Ring pairs captured, output samples delivered. */
	uint64_t pairs, samples;
	uint64_t elapsed_us;
	/** Bank visits; units dropped unfiltered, and their pairs. */
	uint32_t units, abandoned, lost_pairs;
	/** Worst switch lateness in pairs past the threshold. */
	uint32_t late_max;
	/** Longest filter slice in CPU cycles, and filter cycles per pair x 100. */
	uint32_t slice_max, cycles_x100;
	/** Forced gain re-applied during the run. */
	uint32_t gain_refreshed;
	enum esp_sdr_ring_status status;
	uint32_t detail;
};

/**
 * @brief Stream until esp_sdr_ring_stop() or a ring failure.
 *
 * Runs on the calling thread with interrupts masked on its CPU, and holds
 * the radio for the whole run: call it from a thread pinned to a CPU that
 * nothing else needs meanwhile, and change frequency or gain between runs.
 * No flash writes may happen during a run.
 *
 * @retval 0 after esp_sdr_ring_stop().
 * @retval -EINVAL for a bad configuration.
 * @retval -EAGAIN if esp_sdr_init() has not run.
 * @retval -EIO if the ring failed (@p stats says why).
 */
int esp_sdr_ring_run(const struct esp_sdr_ring_cfg *cfg, struct esp_sdr_ring_stats *stats);

struct esp_sdr_ring_bench_result {
	uint32_t pairs, samples;
	/* Filter cycles per input pair x 100: all, and unpack, stage 1, stage 2, sink. */
	uint32_t cycles_x100, stage_x100[4];
};

/**
 * @brief Time the filter on synthetic data in bank 0, the engine idle.
 *
 * Runs @p units ring units (12288 pairs each) through the filter and @p cfg's
 * sink with interrupts masked on the calling CPU. Not while a run is active.
 */
int esp_sdr_ring_bench(const struct esp_sdr_ring_cfg *cfg, unsigned int units,
		       struct esp_sdr_ring_bench_result *res);

/** @brief Ask a running esp_sdr_ring_run() to return; safe from any thread or ISR. */
void esp_sdr_ring_stop(void);

/** @return Whether esp_sdr_ring_run() is running. */
bool esp_sdr_ring_active(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP_SDR_ESP_SDR_RING_H_ */
