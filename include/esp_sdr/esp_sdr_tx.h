/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file
 * @brief ESP-SDR transmit backend interface.
 *
 * Applications feed complex 16-bit samples through this interface; a backend
 * turns them into RF. The module ships only a stub backend that consumes
 * samples at the requested rate and counts them.
 */

#ifndef ESP_SDR_ESP_SDR_TX_H_
#define ESP_SDR_ESP_SDR_TX_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One complex sample, I then Q, full scale +-32767. */
struct esp_sdr_tx_sample {
	int16_t i;
	int16_t q;
};

/** Transmit backend operations. All may sleep; called from one thread. */
struct esp_sdr_tx_backend {
	/** Backend name for diagnostics. */
	const char *name;
	/** Prepare to transmit at @p freq_hz with @p rate_hz samples per second. */
	int (*start)(uint64_t freq_hz, uint32_t rate_hz);
	/** Accept up to @p count samples; returns how many were taken or a negative errno. */
	int (*write)(const struct esp_sdr_tx_sample *samples, size_t count);
	/** Stop transmitting and drop anything queued. */
	int (*stop)(void);
};

/** Transmit statistics since the last start. */
struct esp_sdr_tx_stats {
	uint64_t samples;
	uint32_t writes;
	uint32_t errors;
};

/**
 * @brief Select the backend; NULL restores the default (DAC if built, else the stub).
 *
 * @retval 0 on success.
 * @retval -EBUSY while transmitting.
 */
int esp_sdr_tx_set_backend(const struct esp_sdr_tx_backend *backend);

/** @return The selected backend. */
const struct esp_sdr_tx_backend *esp_sdr_tx_get_backend(void);

/**
 * @brief Start transmitting.
 *
 * @retval 0 on success.
 * @retval -EALREADY if already started.
 * @retval -EINVAL for a zero rate.
 */
int esp_sdr_tx_start(uint64_t freq_hz, uint32_t rate_hz);

/**
 * @brief Hand samples to the backend.
 *
 * @return Samples taken, or a negative errno (-ENOTCONN when not started).
 */
int esp_sdr_tx_write(const struct esp_sdr_tx_sample *samples, size_t count);

/** @brief Stop transmitting; no-op when stopped. */
int esp_sdr_tx_stop(void);

/** @return Whether transmission is started. */
bool esp_sdr_tx_active(void);

/** Copy the statistics into @p stats. */
void esp_sdr_tx_get_stats(struct esp_sdr_tx_stats *stats);

/** Backend that consumes and counts samples, for tests. */
extern const struct esp_sdr_tx_backend esp_sdr_tx_stub_backend;

/** Statistics of the DAC backend since its last start. */
struct esp_sdr_tx_dac_stats {
	/** Input samples played, and dropped (not there in time, or lost in a restart). */
	uint64_t played, dropped;
	/** Blocks short of input (zero filled), and times the input lapped the ring. */
	uint32_t underruns, overruns;
	/** Bank switches, and blocks not filled in time (the DAC restarted). */
	uint32_t bursts, errors;
	/** Interpolation factor (DAC rate / input rate) and run time in us. */
	uint32_t interp, elapsed_us;
	/** Player time waiting for a filled block, for the DAC, and copying, in us. */
	uint32_t ready_wait_us, dac_wait_us, copy_us;
	/** Per filler: time inside fill(), blocks filled, and the CPU it ran on last. */
	uint32_t fill_us[2], fill_blocks[2], fill_cpu[2];
	/** The vector interpolator passed its self-test and is in use. */
	bool simd;
	/** Start to start of consecutive bursts: sum and maximum in us. */
	uint32_t burst_us_sum, burst_us_max;
	/** Least time left between a finished fill and its bank switch, in us. */
	int32_t slack_us_min;
	/** Restarts after a late fill. */
	uint32_t restarts;
	/** Longest fill, latest wake-up for a switch, longest switch-to-fill gap, in us. */
	uint32_t fill_us_max, wake_late_us_max, idle_us_max;
};

#if defined(CONFIG_ESP_SDR_TX_DAC)
/**
 * DAC backend: interpolates the input linearly to 40 MS/s and plays it. The
 * input rate must divide 40 MS/s by at least 2 and the frequency must be a
 * whole MHz (it retunes the shared LO, so the receiver moves too). Output
 * is gapless while the fill keeps up; input that is not there in time plays
 * as zeros.
 */
extern const struct esp_sdr_tx_backend esp_sdr_tx_dac_backend;

/** Copy the DAC backend statistics into @p stats. */
void esp_sdr_tx_dac_get_stats(struct esp_sdr_tx_dac_stats *stats);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ESP_SDR_ESP_SDR_TX_H_ */
