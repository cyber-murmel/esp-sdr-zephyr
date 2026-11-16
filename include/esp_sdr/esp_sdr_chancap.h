/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file
 * @brief Channelized burst capture: one I/Q capture per packet per channel of a uniform grid.
 *
 * The receive ring (esp_sdr_ring.h, 16 MS/s, no decimation) feeds a hand-over ring in SRAM bank 2
 * from CPU 1; a thread on the other CPU runs a wideband power gate, a channelizer (a polyphase
 * filter bank for grids that divide the ring rate, else one mixer and decimator per channel), a
 * power detector per channel, and assembles the captures in a PSRAM slab. A callback thread
 * hands every finished capture to the consumer. Design: doc/chancap.md. Needs
 * CONFIG_ESP_SDR_CHANCAP.
 */

#ifndef ESP_SDR_ESP_SDR_CHANCAP_H_
#define ESP_SDR_ESP_SDR_CHANCAP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_sdr/esp_sdr_ring.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup esp_sdr_chancap
 * @{
 */

/** Channels of one instance at most. */
#define ESP_SDR_CHANCAP_CHANNELS_MAX 16U

/** Output full scale: a full scale input tone on a channel's centre gives this amplitude. */
#define ESP_SDR_CHANCAP_FULL_SCALE ESP_SDR_RING_RAW_FULL_SCALE

/** One finished capture: a packet on one channel, or a chunk of its stream. */
struct esp_sdr_chancap {
	/** Channel index: its centre is first_freq_hz + channel * spacing_hz. */
	uint16_t channel;
	/**
	 * Ring pair index of the first sample, as esp_sdr_ring_now() counts (the centre of the
	 * channel filter's window, so captures of different channels line up).
	 */
	uint64_t index;
	/** Per-channel sample rate in Hz. */
	uint32_t sample_rate;
	/** Interleaved I, Q; full scale ESP_SDR_CHANCAP_FULL_SCALE. */
	const int16_t *iq;
	/** Complex samples. */
	size_t n;
	/** Peak block power of the burst and the channel's noise floor, in dBFS. */
	int16_t peak_db, noise_db;
	/** Cut at max_capture_us while the detector still saw power. */
	bool truncated;
	/** A fixed-length piece of a continuous stream (the per-channel gate off). */
	bool chunk;
};

/** Called on the callback thread for every finished capture; the consumer releases it. */
typedef void (*esp_sdr_chancap_cb_t)(void *user, const struct esp_sdr_chancap *cap);

struct esp_sdr_chancap_cfg {
	/** Centre of channel 0 in Hz. */
	uint32_t first_freq_hz;
	/** Channel spacing in Hz (2 MHz BLE, 5 MHz IEEE 802.15.4). */
	uint32_t spacing_hz;
	/** Channels, 1 to ESP_SDR_CHANCAP_CHANNELS_MAX; the whole grid must fit the ring's band. */
	uint16_t n_channels;
	/**
	 * Per-channel sample rate in Hz. A grid whose spacing divides the ring rate runs the
	 * filter bank at the spacing or twice it; any other grid runs a decimator per channel at
	 * the ring rate divided by an integer up to 32 (4 MS/s for 802.15.4).
	 */
	uint32_t channel_rate_hz;
	/** Wideband gate on at start (the channelizer runs only while the band carries power). */
	bool wb_gate;
	/** Per-channel gate on at start (one capture per packet; off: back-to-back chunks). */
	bool ch_gate;
	/** Wideband gate threshold in dB above its tracked noise floor. */
	int16_t wb_threshold_db;
	/** Per-channel threshold in dB above that channel's noise floor. */
	int16_t ch_threshold_db;
	/** Samples before a trigger kept in a capture, in us (at most about 200). */
	uint32_t lookback_us;
	/** Hold after the power drops, in us. */
	uint32_t hang_us;
	/** Longest capture, and the chunk length with the per-channel gate off, in us. */
	uint32_t max_capture_us;
	esp_sdr_chancap_cb_t cb;
	void *user;
};

struct esp_sdr_chancap_stats {
	/** Ring pairs handed over, and of them channelized. */
	uint64_t pairs, pairs_run;
	/** Captures delivered, dropped for want of a buffer, cut at max_capture_us. */
	uint32_t captures, dropped, truncated;
	/** Hand-over ring overruns (the channelizer fell behind) and ring gaps seen. */
	uint32_t overruns, gaps;
	/** Channelizer cycles per input pair x 100 while running, and the longest block. */
	uint32_t cycles_x100, block_max;
	/** Most input pairs the channelizer was behind the newest one (it overruns past 6656). */
	uint32_t lag_max;
	/**
	 * Diagnostics, cycles: the longest sub-block run, capture open and capture finish
	 * (preemption included, as in block_max).
	 */
	uint32_t run_max, open_max, finish_max;
	/** The receive ring's own statistics of the current or last run. */
	struct esp_sdr_ring_stats ring;
	/** Capture buffers in use right now, and at most. */
	uint16_t bufs_used, bufs_peak;
};

/**
 * @brief Tune to the grid and start capturing.
 *
 * Picks the LO so that the LO leakage falls between channels where it can, starts the ring on
 * CPU 1 and the channelizer and callback threads on the other CPU.
 *
 * @retval 0 on success.
 * @retval -EINVAL for a grid that does not fit the band, an unsupported rate, or no callback.
 * @retval -EBUSY if an instance is running.
 * @retval -EAGAIN if esp_sdr_init() has not run.
 * @retval -ERANGE if the LO cannot be tuned.
 */
int esp_sdr_chancap_start(const struct esp_sdr_chancap_cfg *cfg);

/** @brief Switch the gates; takes effect at the next hand-over block. */
int esp_sdr_chancap_set_gates(bool wideband, bool per_channel);

/** @brief Change the thresholds; takes effect at the next hand-over block. */
int esp_sdr_chancap_set_thresholds(int16_t wb_db, int16_t ch_db);

/** @brief Give a capture's buffer back. */
void esp_sdr_chancap_release(const struct esp_sdr_chancap *cap);

/** @brief Stop the run and join its threads; buffers still held stay valid until released. */
void esp_sdr_chancap_stop(void);

/** @return Whether an instance is running. */
bool esp_sdr_chancap_active(void);

void esp_sdr_chancap_get_stats(struct esp_sdr_chancap_stats *stats);

/** The LO the run tuned to, in Hz, and the engine it chose. */
struct esp_sdr_chancap_plan {
	uint64_t lo_hz;
	/** Filter bank (m channels, decimation d) or decimator bank (d). */
	bool pfb;
	unsigned int m, d;
	/** Taps of the channel filter and its group delay in ring pairs. */
	unsigned int taps, delay_pairs;
};

/** Copy the running or last run's plan into @p plan. */
void esp_sdr_chancap_get_plan(struct esp_sdr_chancap_plan *plan);

/** Timing of the channelizer on synthetic data (esp_sdr_chancap_bench()). */
struct esp_sdr_chancap_bench_result {
	/** Whether the grid runs the fast path: 8 channels at decimation 8 over both CPUs. */
	bool fast;
	/**
	 * Cycles per input pair x 100 (15 are available per CPU at 16 MS/s): the ring's CPU
	 * (unpack and sink, the branch sums included on the fast path), the channelizer's CPU
	 * for every sub-block (block), and for the C engines alone: a filter bank frame, or one
	 * channel of the decimator bank.
	 */
	uint32_t ring_x100, block_x100, pfb_x100, ddc_x100;
	/**
	 * The ring CPU's share: the unpack, the sink, and the ring keeping the sink's history
	 * (the rest is the ring's own work).
	 */
	uint32_t ring_unpack_x100, ring_sink_x100, ring_hist_x100;
	/** The sink alone on the same blocks, and the channelizer CPU's split (fast path): the
	 * kernels (post, power, transpose) and the per-channel work. */
	uint32_t sink_x100, kern_x100, chan_x100;
	/**
	 * Fast path, cycles per kernel call: branch sums per frame, post per 8 frames, power of a
	 * detector block (32 frames, every 4 runs).
	 */
	uint32_t sums_cyc, post_cyc, power_cyc;
	/** Whether the vector kernels matched the C reference on the same data. */
	bool kernels_match;
	uint32_t mismatches;
};

/**
 * @brief Time the channelizer on noise-like data on the calling CPU, not while a run is active.
 *
 * @param cfg The grid and rate to time; the gates are ignored.
 * @param pairs Input pairs to process (a multiple of 512).
 * @param res The timings.
 *
 * @retval 0 on success.
 * @retval -EBUSY while a run is active.
 * @retval -EINVAL for a pair count that is not a multiple of 512, or a grid esp_sdr_chancap_start() refuses.
 * @retval -ERANGE for an LO below the band.
 */
int esp_sdr_chancap_bench(const struct esp_sdr_chancap_cfg *cfg, uint32_t pairs,
			  struct esp_sdr_chancap_bench_result *res);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ESP_SDR_ESP_SDR_CHANCAP_H_ */
