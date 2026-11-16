/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file
 * @brief ESP-SDR triggered snapshots (ESP32-C6): the full 80 MS/s, kept from before a burst.
 *
 * The dump engine writes the receive I/Q into SRAM block 2 as a ring of
 * ESP_SDR_TRIG_RING samples (205 us at 80 MS/s), while the CPU watches the
 * power the IQ estimator sums over the same 80 MHz input (sums of I^2, Q^2,
 * I and Q, in hardware) in windows of ESP_SDR_TRIG_WINDOW_US, about each
 * window's mean: the receiver's DC, larger than its noise and drifting after
 * a retune, does not count. When ESP_SDR_TRIG_WINDOWS windows in a row rise
 * past the threshold the engine keeps writing for the requested number of
 * samples from the first of them, then stops: the ring holds the burst's
 * start and what came before it.
 *
 * The CPUs cannot reach block 2 while the engine owns it, so the ring is
 * read only once it has stopped, and the engine writes only its lower half:
 * the upper 64 KiB is the caller's scratch until the next
 * esp_sdr_trig_wait(), for packing and sending the snapshot from.
 */

#ifndef ESP_SDR_ESP_SDR_TRIG_H_
#define ESP_SDR_ESP_SDR_TRIG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup esp_sdr_trig
 * @{
 */

/** Samples in the ring: 205 us at 80 MS/s. */
#define ESP_SDR_TRIG_RING 16384U
/** Sample rate of a snapshot. */
#define ESP_SDR_TRIG_RATE_HZ 80000000U
/** Power window: one estimator reading each. */
#define ESP_SDR_TRIG_WINDOW_US 10U
/**
 * Windows in a row over the threshold that make a trigger: bursts of 20 to 30 us and longer.
 * Once the Wi-Fi driver is in some state the C6's receiver shows a glitch of 1.75 us every
 * 139 ms, 30 dB over the noise at any frequency; it touches two windows at most.
 */
#define ESP_SDR_TRIG_WINDOWS 3U

struct esp_sdr_trig_cfg {
	/**
	 * Threshold as a multiple of the quiet baseline, in sixteenths (128: 9 dB). The
	 * baseline is the lower quartile of the first 64 windows after esp_sdr_trig_reset() and
	 * follows quiet windows slowly after that; without a quiet window for 10 ms it is
	 * learnt again.
	 */
	uint32_t ratio_x16;
	/** Absolute threshold (window power, see esp_sdr_trig_snap::power) instead, if not 0. */
	uint32_t level;
	/** Samples kept after the trigger, below ESP_SDR_TRIG_RING. */
	uint32_t post;
	/** Polled between windows; true ends the wait with -EINTR. */
	bool (*stop)(void *user);
	void *user;
};

struct esp_sdr_trig_snap {
	/** The ring: one IQ word per sample (esp_sdr_rx_i(), esp_sdr_rx_q()). */
	const uint32_t *words;
	/** Ring index of the oldest sample. */
	uint32_t first;
	/** Samples from the oldest to the first window over the threshold (its first sample, +- 1 window). */
	uint32_t trig;
	/**
	 * The first window's power over the threshold and the baseline then: the sum of
	 * I^2 + Q^2 over a window's 800 inputs, less their mean's part.
	 */
	uint32_t power, baseline;
	/** Free until the next esp_sdr_trig_wait(): 64 KiB, 4-byte aligned. */
	uint8_t *scratch;
	size_t scratch_len;
};

/**
 * @brief Run the ring until a burst, then freeze it.
 *
 * Takes the radio meanwhile, with interrupts locked between the stop()
 * polls (every ESP_SDR_TRIG_WINDOW_US). A trigger needs a quiet window
 * first (a rising edge) and a ring full of samples taken since the call.
 *
 * @return 0 with @p snap filled, -EINTR when stop() said so, -EINVAL for a
 *         bad @p cfg, -EAGAIN before esp_sdr_init().
 */
int esp_sdr_trig_wait(const struct esp_sdr_trig_cfg *cfg, struct esp_sdr_trig_snap *snap);

/** @brief Forget the baseline (after a change of frequency or gain). */
void esp_sdr_trig_reset(void);

/**
 * @brief Pack @p n samples of a snapshot, from the @p from-th oldest, to int8
 * I, Q pairs (the top 8 of each 10-bit value) at @p out, 2-byte aligned.
 */
void esp_sdr_trig_pack_iq8(const struct esp_sdr_trig_snap *snap, uint32_t from, uint32_t n,
			   int8_t *out);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ESP_SDR_ESP_SDR_TRIG_H_ */
