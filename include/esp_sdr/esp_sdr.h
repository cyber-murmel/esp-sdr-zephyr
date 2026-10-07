/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file
 * @brief ESP-SDR raw I/Q receiver (Zephyr port of ESPARGOS esp-sdr).
 *
 * Captures bursts of raw baseband I/Q samples from the Wi-Fi receiver through
 * an undocumented debug path. One user at a time; all calls may sleep.
 */

#ifndef ESP_SDR_ESP_SDR_H_
#define ESP_SDR_ESP_SDR_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Software tuning limits in MHz; PLL lock is not guaranteed across the range. */
#define ESP_SDR_FREQ_MIN_MHZ 100U
#define ESP_SDR_FREQ_MAX_MHZ 6000U

/** Burst length limits in complex samples. */
#define ESP_SDR_SAMPLES_MIN 256U
#define ESP_SDR_SAMPLES_MAX 16380U

/**
 * Place a static buffer in the internal SRAM above the capture bank (about
 * 20 KiB, DMA capable). It is not zeroed at boot: buffers only.
 */
#define ESP_SDR_HIGH_RAM __attribute__((section(".esp_sdr_high")))

/** Low-pass filter code that restores the PHY-calibrated setting. */
#define ESP_SDR_LPF_AUTO (-1)
/** Largest raw low-pass filter capacitor code. */
#define ESP_SDR_LPF_MAX  63

/** Gain index that selects the hardware AGC. */
#define ESP_SDR_GAIN_AUTO (-1)

/** Native capture rates; values are the upstream protocol rate indices. */
enum esp_sdr_rate {
	ESP_SDR_RATE_80MSPS = 0,
	ESP_SDR_RATE_40MSPS = 1,
	ESP_SDR_RATE_16MSPS = 6,
};

/** Result of one burst capture. */
struct esp_sdr_burst {
	/** Raw words, Q in bits 9:0 and I in bits 19:10 (signed). Valid until the next capture. */
	uint32_t *words;
	/** Number of complex samples. */
	size_t count;
	/** Time from arming to completion in microseconds. */
	uint32_t elapsed_us;
};

/**
 * @brief Bring the receiver up and tune it to the default frequency.
 *
 * Needs the Wi-Fi driver started; safe to call more than once.
 *
 * @retval 0 on success.
 * @retval -ENODEV if the Wi-Fi driver is not ready.
 * @retval -EIO if a Wi-Fi call failed.
 */
int esp_sdr_init(void);

/**
 * @brief Tune the receive LO.
 *
 * @param mhz Frequency in MHz, ESP_SDR_FREQ_MIN_MHZ to ESP_SDR_FREQ_MAX_MHZ.
 * @retval 0 on success.
 * @retval -EINVAL if out of range.
 * @retval -EAGAIN if esp_sdr_init() has not run.
 */
int esp_sdr_set_frequency(uint32_t mhz);

/** @return Current receive LO frequency in MHz. */
uint32_t esp_sdr_get_frequency(void);

/**
 * @brief Set a PLL offset in kHz and retune.
 *
 * A non-zero offset forces direct PLL tuning, also on Wi-Fi channel frequencies.
 *
 * @retval 0 on success.
 * @retval -EAGAIN if esp_sdr_init() has not run.
 */
int esp_sdr_set_freq_offset(int32_t khz);

/**
 * @brief Select hardware AGC or a fixed receive gain.
 *
 * Indices are PHY gain table entries, not dB; higher is more gain.
 *
 * @param index ESP_SDR_GAIN_AUTO, or 0 to esp_sdr_gain_max().
 * @retval 0 on success.
 * @retval -EINVAL if out of range.
 * @retval -ENOTSUP for a fixed gain without CONFIG_ESP_SDR_MANUAL_GAIN.
 * @retval -EAGAIN if esp_sdr_init() has not run.
 */
int esp_sdr_set_gain(int index);

/** @return Selected gain index, or ESP_SDR_GAIN_AUTO. */
int esp_sdr_get_gain(void);

/** @return Largest calibrated gain index, 0 if the PHY reports none. */
int esp_sdr_gain_max(void);

/**
 * @brief Select a transmit power step, picked up by the next esp_sdr_play().
 *
 * Steps follow the vendor PHY's TX power ladder (gain memory index plus
 * baseband gain code per step). TX has no AGC, so there is no "auto" step.
 *
 * @param index 0 (weakest) to esp_sdr_tx_gain_max() (strongest).
 * @retval 0 on success.
 * @retval -EINVAL if out of range.
 * @retval -ENOTSUP without CONFIG_ESP_SDR_MANUAL_GAIN.
 */
int esp_sdr_set_tx_gain(int index);

/** @return Selected transmit power step. */
int esp_sdr_get_tx_gain(void);

/** @return Strongest transmit power step. */
int esp_sdr_tx_gain_max(void);

/**
 * @return The vendor's target power for @p index in quarter dB (relative, not
 * dBm; measured 4.24 units per dB over the ladder), INT_MIN if out of range.
 */
int esp_sdr_tx_gain_power(int index);

/**
 * @brief Select the analog low-pass filter by approximate bandwidth.
 *
 * @param mhz Two-sided bandwidth in MHz, 0 selects the widest setting.
 * @retval 0 on success.
 * @retval -EINVAL if outside the characterized range.
 */
int esp_sdr_set_bandwidth(uint32_t mhz);

/** @return Supported bandwidth range in MHz through @p min and @p max. */
void esp_sdr_bandwidth_range(uint32_t *min, uint32_t *max);

/**
 * @brief Select a raw low-pass filter capacitor code.
 *
 * @param code 0 (widest) to ESP_SDR_LPF_MAX, or ESP_SDR_LPF_AUTO.
 * @retval 0 on success.
 * @retval -EINVAL if out of range.
 */
int esp_sdr_set_lpf(int code);

/** @return Selected low-pass filter code, or ESP_SDR_LPF_AUTO. */
int esp_sdr_get_lpf(void);

/**
 * @brief Capture one burst of complex samples.
 *
 * The samples live in a reserved SRAM bank and stay valid until the next
 * capture. The caller may rewrite them in place, for example to pack them.
 *
 * @param rate Native sample rate.
 * @param count Complex samples, ESP_SDR_SAMPLES_MIN to ESP_SDR_SAMPLES_MAX.
 * @param burst Filled with the result.
 * @retval 0 on success.
 * @retval -EINVAL for an unsupported rate or count.
 * @retval -EAGAIN if esp_sdr_init() has not run.
 * @retval -ETIMEDOUT if the capture engine did not finish.
 */
int esp_sdr_capture(enum esp_sdr_rate rate, size_t count, struct esp_sdr_burst *burst);

/**
 * @brief Play one burst of complex samples out through the TX DAC.
 *
 * Reverse direction of esp_sdr_capture(): drives the dump engine's DAC-side
 * trigger instead of its ADC-side one, through the same shared SRAM bank.
 * Swaps the analog front end to TX for the duration and restores RX
 * afterwards. The caller must still have RF safety in place (dummy load /
 * attenuated link); this function does not limit transmit power beyond
 * TX_GAIN_DEFAULT_INDEX's conservative default.
 *
 * The DAC side has its own clock: only ESP_SDR_RATE_40MSPS and
 * ESP_SDR_RATE_80MSPS exist, see esp_sdr_tx_rate_hz().
 *
 * @param rate Transmit sample rate.
 * @param words Complex samples built with esp_sdr_tx_word().
 * @param count Complex samples, ESP_SDR_SAMPLES_MIN to ESP_SDR_SAMPLES_MAX.
 * @retval 0 on success.
 * @retval -EINVAL for an unsupported rate or count.
 * @retval -EAGAIN if esp_sdr_init() has not run.
 * @retval -ETIMEDOUT if the dump engine did not finish.
 */
int esp_sdr_play(enum esp_sdr_rate rate, const uint32_t *words, size_t count);

/** Longest esp_sdr_play_for() duration; the caller's thread busy-waits throughout. */
#define ESP_SDR_PLAY_MAX_MS 5000U

/**
 * @brief Replay one burst back to back for a while, like esp_sdr_play().
 *
 * Switches the front end once and retriggers the DAC until @p duration_ms has
 * passed (0: exactly one burst). With a duration, @p words is tiled over as
 * much of the bank as whole copies fit, so each retrigger (~1.35 us gap) comes
 * after up to ESP_SDR_SAMPLES_MAX samples. Seamless only if @p words holds a
 * whole number of signal periods. Holds the receiver lock throughout, so
 * captures wait.
 *
 * @param bursts If not NULL, set to the number of bursts played.
 * @retval 0 on success.
 * @retval -EINVAL for an unsupported rate, count or duration.
 * @retval -EAGAIN if esp_sdr_init() has not run.
 * @retval -ETIMEDOUT if the dump engine did not finish a burst.
 */
int esp_sdr_play_for(enum esp_sdr_rate rate, const uint32_t *words, size_t count,
		     uint32_t duration_ms, uint32_t *bursts);

/** What an esp_sdr_sweep() run did. */
struct esp_sdr_sweep_stats {
	/** DAC bursts played. */
	uint32_t segments;
	/** Samples played; divide by the rate and elapsed_ms for the duty cycle. */
	uint32_t samples;
	/** Wall-clock run time in ms. */
	uint32_t elapsed_ms;
};

/**
 * @brief Transmit a linear frequency sweep at fixed power.
 *
 * Frequency goes linearly from @p f0_hz to @p f1_hz (constant Hz per second)
 * over @p duration_ms of wall-clock time, at the selected transmit step
 * (esp_sdr_set_tx_gain()) and digital amplitude @p amp. Each segment of up to
 * ESP_SDR_SAMPLES_MAX samples is synthesized into the bank while the DAC is
 * idle, so transmission pauses between segments; phase follows absolute time,
 * so the chirp stays continuous across the pauses. Holds the receiver lock
 * throughout.
 *
 * @param rate Transmit sample rate, see esp_sdr_tx_rate_hz().
 * @param f0_hz,f1_hz Baseband offsets from the LO, below half the rate.
 * @param duration_ms 1 to ESP_SDR_PLAY_MAX_MS.
 * @param amp Digital amplitude 1 to 511; the TX baseband compresses near 511.
 * @param stats If not NULL, filled with what was played.
 * @retval 0 on success.
 * @retval -EINVAL for an unsupported rate, frequency, duration or amplitude.
 * @retval -EAGAIN if esp_sdr_init() has not run.
 * @retval -ETIMEDOUT if the dump engine did not finish a burst.
 */
int esp_sdr_sweep(enum esp_sdr_rate rate, float f0_hz, float f1_hz, uint32_t duration_ms,
		  int amp, struct esp_sdr_sweep_stats *stats);

/** Decimated complex sample, full scale +-32767. */
struct esp_sdr_iq16 {
	int16_t i;
	int16_t q;
};

/** Decimation factor limits for esp_sdr_capture_decimated(). */
#define ESP_SDR_DECIM_MIN 2U
#define ESP_SDR_DECIM_MAX 160U

/**
 * @brief Capture a burst and decimate it with a third order CIC filter.
 *
 * Captures @p count samples like esp_sdr_capture(), then (still holding the
 * receiver lock) decimates by @p m into @p out, I and Q in the usual order.
 * The first 3 outputs (filter settling) are dropped, so up to count / m - 3
 * samples come out. The CIC needs no multiplies per input sample but droops
 * towards the band edge: -2.7 dB at a quarter of the output rate, -9.3 dB at
 * 0.45 of it.
 *
 * @param out Output samples, full scale +-32767.
 * @param max_out Capacity of @p out.
 * @param n_out Set to the number of samples written.
 * @param first_ns Set to the time of the first output sample (k_cycle_get_64() clock, ns).
 * @retval 0 on success.
 * @retval -EINVAL for an unsupported rate, count or factor.
 * @retval -EAGAIN if esp_sdr_init() has not run.
 * @retval -ETIMEDOUT if the capture engine did not finish.
 */
int esp_sdr_capture_decimated(enum esp_sdr_rate rate, size_t count, unsigned int m,
			      struct esp_sdr_iq16 *out, size_t max_out, size_t *n_out,
			      uint64_t *first_ns);

/** @return Sample rate in Hz, or 0 for an unknown rate. */
uint32_t esp_sdr_rate_hz(enum esp_sdr_rate rate);

/** @return Transmit sample rate in Hz, or 0 if the DAC cannot run at @p rate. */
uint32_t esp_sdr_tx_rate_hz(enum esp_sdr_rate rate);

/*
 * Receive words carry Q in the low field: with I low, a tone below the LO
 * showed up above it (checked by detuning the receiver against a known
 * carrier). Transmit words are the other way round, see esp_sdr_tx_word().
 */

/** @return In-phase component of a raw receive word, -512 to 511. */
static inline int16_t esp_sdr_i(uint32_t word)
{
	return (int16_t)((int32_t)(word << 12) >> 22);
}

/** @return Quadrature component of a raw receive word, -512 to 511. */
static inline int16_t esp_sdr_q(uint32_t word)
{
	return (int16_t)((int32_t)(word << 22) >> 22);
}

/** @return Raw transmit word for esp_sdr_play(): I in bits 9:0, Q in bits 19:10. */
static inline uint32_t esp_sdr_tx_word(int16_t i, int16_t q)
{
	return ((uint32_t)i & 0x3ffU) | (((uint32_t)q & 0x3ffU) << 10);
}

/**
 * @brief Pack raw words to signed 8-bit I then Q (upper 8 of 10 bits).
 *
 * @p out may alias @p words.
 *
 * @return Bytes written, 2 * count.
 */
size_t esp_sdr_pack_iq8(const uint32_t *words, size_t count, uint8_t *out);

/**
 * @brief Pack raw words to the upstream packed 10-bit format.
 *
 * Little-endian 20-bit I/Q pairs, two samples per five bytes. @p out may alias @p words.
 *
 * @return Bytes written, ceil(20 * count / 8).
 */
size_t esp_sdr_pack_iq10(const uint32_t *words, size_t count, uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ESP_SDR_ESP_SDR_H_ */
