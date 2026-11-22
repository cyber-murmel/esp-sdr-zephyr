/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file
 * @brief ESP-SDR receive: gain, analog low-pass, burst and decimated capture.
 *
 * Captures bursts of raw baseband I/Q samples from the Wi-Fi receiver through
 * an undocumented debug path.
 */

#ifndef ESP_SDR_ESP_SDR_RX_H_
#define ESP_SDR_ESP_SDR_RX_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_sdr/esp_sdr.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup esp_sdr_rx
 * @{
 */

/** Gain index that selects the hardware AGC. */
#define ESP_SDR_RX_GAIN_AUTO (-1)

/** Low-pass filter code that restores the PHY-calibrated setting. */
#define ESP_SDR_RX_LPF_AUTO (-1)
/** Largest raw low-pass filter capacitor code: seven bits on the ESP32, six on the others. */
#if defined(CONFIG_SOC_SERIES_ESP32)
#define ESP_SDR_RX_LPF_MAX 127
#else
#define ESP_SDR_RX_LPF_MAX 63
#endif

/** Decimation factor limits for esp_sdr_rx_capture_decimated(). */
#define ESP_SDR_RX_DECIM_MIN 2U
#define ESP_SDR_RX_DECIM_MAX 160U
/** Outputs esp_sdr_rx_capture_decimated() drops while its filter settles: it gives count / m less these. */
#define ESP_SDR_RX_DECIM_SETTLE 3U

/** Fold factor limits for esp_sdr_rx_capture_folded(). */
#define ESP_SDR_RX_FOLD_MIN 2U
#define ESP_SDR_RX_FOLD_MAX 160U

/** Result of one burst capture. */
struct esp_sdr_rx_burst {
	/** Raw words, Q in bits 9:0 and I in bits 19:10 (signed). Valid until the next capture. */
	uint32_t *words;
	/** Number of complex samples. */
	size_t count;
	/** Time from arming to completion in microseconds. */
	uint32_t elapsed_us;
};

/**
 * @brief Select hardware AGC or a fixed receive gain.
 *
 * Indices are PHY gain table entries, not dB; higher is more gain.
 *
 * @param index ESP_SDR_RX_GAIN_AUTO, or 0 to esp_sdr_rx_gain_max().
 * @retval 0 on success.
 * @retval -EINVAL if out of range.
 * @retval -ENOTSUP for a fixed gain without CONFIG_ESP_SDR_RFTEST.
 * @retval -EAGAIN if esp_sdr_init() has not run.
 */
int esp_sdr_rx_set_gain(int index);

/** @return Selected gain index, or ESP_SDR_RX_GAIN_AUTO. */
int esp_sdr_rx_get_gain(void);

/** @return Largest calibrated gain index, 0 if the PHY reports none. */
int esp_sdr_rx_gain_max(void);

/**
 * @brief Passive receive: sample what the MAC and AGC have set up, force nothing.
 *
 * While on, retunes, front end forcing, gain forcing and low-pass writes are skipped, so a
 * Wi-Fi link on the same radio keeps running (the frequency is then whatever Wi-Fi tuned).
 * Switching on gives the gain back to the AGC and restores the PHY's low-pass codes; fixed
 * gain and low-pass codes are refused (-EBUSY) until it is off again. May be called before
 * esp_sdr_init().
 *
 * @param on true for passive.
 * @retval 0 on success.
 * @retval -ENOTSUP without CONFIG_ESP_SDR_RFTEST.
 */
int esp_sdr_rx_set_passive(bool on);

/** @return true while esp_sdr_rx_set_passive() is on. */
bool esp_sdr_rx_is_passive(void);

/**
 * @brief Gain index field of the Wi-Fi AGC register (AGC_FORCE_REG [31:24]).
 *
 * Under the AGC this is the index the AGC last wrote only if the PHY mirrors it there: not
 * verified, treat as a probe. @retval -ENOTSUP except on the ESP32-S3.
 */
int esp_sdr_rx_agc_index(void);

/**
 * @brief Select the analog low-pass filter by approximate bandwidth.
 *
 * @param mhz Two-sided bandwidth in MHz, 0 selects the widest setting.
 * @retval 0 on success.
 * @retval -EINVAL if outside the characterized range.
 */
int esp_sdr_rx_set_bandwidth(uint32_t mhz);

/**
 * @brief Supported bandwidth range in MHz.
 *
 * @param min Set to the narrowest setting.
 * @param max Set to the widest setting.
 */
void esp_sdr_rx_bandwidth_range(uint32_t *min, uint32_t *max);

/**
 * @brief Select a raw low-pass filter capacitor code.
 *
 * @param code 0 (widest) to ESP_SDR_RX_LPF_MAX, or ESP_SDR_RX_LPF_AUTO.
 * @retval 0 on success.
 * @retval -EINVAL if out of range.
 */
int esp_sdr_rx_set_lpf(int code);

/** @return Selected low-pass filter code, or ESP_SDR_RX_LPF_AUTO. */
int esp_sdr_rx_get_lpf(void);

/**
 * @brief Capture one burst of complex samples.
 *
 * The samples live in a reserved SRAM bank and stay valid until the next
 * capture or transmit session by any thread: with other radio users, copy
 * them with esp_sdr_rx_capture_iq() instead. The caller may rewrite them in
 * place, for example to pack them.
 *
 * @param rate Native sample rate.
 * @param count Complex samples, ESP_SDR_SAMPLES_MIN to ESP_SDR_SAMPLES_MAX.
 * @param burst Filled with the result.
 * @retval 0 on success.
 * @retval -EINVAL for an unsupported rate or count.
 * @retval -EAGAIN if esp_sdr_init() has not run.
 * @retval -ETIMEDOUT if the capture engine did not finish.
 */
int esp_sdr_rx_capture(enum esp_sdr_rate rate, size_t count, struct esp_sdr_rx_burst *burst);

/**
 * @brief Capture one burst and convert it to I/Q (raw units, -512 to 511) under the radio lock.
 *
 * @param rate Native sample rate.
 * @param count Complex samples, ESP_SDR_SAMPLES_MIN to ESP_SDR_SAMPLES_MAX.
 * @param out Output samples, @p count of them.
 * @param first_ns Set to the time of the first sample returned (k_cycle_get_64() clock, ns): the
 *                 arming time, but on the ESP32-S31 256 settling words after it.
 * @return Like esp_sdr_rx_capture().
 */
int esp_sdr_rx_capture_iq(enum esp_sdr_rate rate, size_t count, struct esp_sdr_iq16 *out,
			  uint64_t *first_ns);

/** Takes a burst's raw words (esp_sdr_rx_burst.words) for esp_sdr_rx_capture_with(). */
typedef void (*esp_sdr_rx_words_fn_t)(void *user, const uint32_t *words, size_t count);

/**
 * @brief Capture one burst and hand its raw words to @p fn under the radio lock.
 *
 * For work straight on the bank's words that other radio users must not overwrite meanwhile, as
 * esp_sdr_rx_capture_iq() converts them. @p fn runs in the calling thread, after the capture; it
 * must not call into esp_sdr, and every other thread that wants the radio waits for it.
 *
 * @param rate Native sample rate.
 * @param count Complex samples, ESP_SDR_SAMPLES_MIN to ESP_SDR_SAMPLES_MAX.
 * @param fn Called once with @\p user, the words and @p count; the words are good for the call.
 * @param user Passed to @p fn.
 * @param first_ns As for esp_sdr_rx_capture_iq().
 * @return Like esp_sdr_rx_capture(); @p fn runs only on success.
 */
int esp_sdr_rx_capture_with(enum esp_sdr_rate rate, size_t count, esp_sdr_rx_words_fn_t fn,
			    void *user, uint64_t *first_ns);

#if defined(CONFIG_SOC_SERIES_ESP32S31) || defined(__DOXYGEN__)
/** What a DC calibration measured; means in 1/256 of an 8-bit step, I then Q. */
struct esp_sdr_rx_dc {
	/** Before it. */
	int32_t before[2];
	/** After it, the best it reached. */
	int32_t after[2];
	/** Settings it tried. */
	uint32_t steps;
};

/**
 * @brief Take the receive DC off with the baseband's DC DACs (ESP32-S31).
 *
 * For the fixed gain set: its setting stays until the next retune, gain change or
 * esp_sdr_diag16_run(), whose phase check passes through gain 0 (cfg.dc_correct finds it anew).
 * Takes the receiver for some 100 ms.
 *
 * @param result What it measured.
 * @return 0 on success (or nothing to do), -ENOTSUP under the AGC or without a calibrated
 *         gain word, -ERANGE if the DACs do not move the DC, -EIO if a capture failed,
 *         -EAGAIN before esp_sdr_init().
 */
int esp_sdr_rx_dc_calibrate(struct esp_sdr_rx_dc *result);
#endif

/**
 * @brief Capture one burst into a chosen bank, like esp_sdr_rx_capture().
 *
 * With two banks (CONFIG_ESP_SDR_BANK1), one burst can be processed while
 * the next is captured into the other bank. Bank 0 is the one
 * esp_sdr_rx_capture() uses.
 *
 * @param rate Native sample rate.
 * @param count Complex samples, ESP_SDR_SAMPLES_MIN to ESP_SDR_SAMPLES_MAX.
 * @param bank 0 to ESP_SDR_BANKS - 1.
 * @param burst Filled with the result.
 * @return Like esp_sdr_rx_capture(); -EINVAL also for a bank that does not exist.
 */
int esp_sdr_rx_capture_bank(enum esp_sdr_rate rate, size_t count, int bank,
			    struct esp_sdr_rx_burst *burst);

/**
 * @brief Have work done while a capture runs.
 *
 * The engine needs no CPU while it fills the bank, so @p fn is called once in each capture from
 * its start, in the capturing thread, instead of spinning all that time. It must not start a
 * capture or read the bank. It may take longer than the capture, which then only ends late.
 *
 * On the ESP32-S31 the burst lands in TCM group 2 where the image leaves it free
 * (CONFIG_ESP_SDR_S31_GROUP2): @p fn runs during it, interrupts on, and the engine runs on past
 * the burst until @p fn returns. It must return before the engine reaches the group's end, some
 * 16000 words after the burst's (0.8 ms at 20 MS/s, 0.2 ms at 80), or the capture fails with
 * -EOVERFLOW. Without group 2 nothing can run while the engine has the memory: it is called just
 * before the burst, which then starts late.
 *
 * @param fn Called with @p user; NULL for none.
 * @param user Passed to @p fn.
 */
void esp_sdr_rx_set_capture_hook(void (*fn)(void *user), void *user);

/**
 * @brief Capture a burst and decimate it with a third order CIC filter.
 *
 * Captures @p count samples like esp_sdr_rx_capture(), then (still holding
 * the radio lock) decimates by @p m into @p out, I and Q in the usual order.
 * The first 3 outputs (filter settling) are dropped, so up to count / m - 3
 * samples come out. The CIC needs no multiplies per input sample but droops
 * towards the band edge: -2.7 dB at a quarter of the output rate, -9.3 dB at
 * 0.45 of it. Needs CONFIG_ESP_SDR_RX_DECIM.
 *
 * @param rate Native sample rate.
 * @param count Complex samples, ESP_SDR_SAMPLES_MIN to ESP_SDR_SAMPLES_MAX.
 * @param m Decimation factor, ESP_SDR_RX_DECIM_MIN to ESP_SDR_RX_DECIM_MAX.
 * @param out Output samples, full scale +-32767.
 * @param max_out Capacity of @p out.
 * @param n_out Set to the number of samples written.
 * @param first_ns Set to the time of the first output sample (k_cycle_get_64() clock, ns).
 * @retval 0 on success.
 * @retval -EINVAL for an unsupported rate, count or factor.
 * @retval -EAGAIN if esp_sdr_init() has not run.
 * @retval -ETIMEDOUT if the capture engine did not finish.
 */
int esp_sdr_rx_capture_decimated(enum esp_sdr_rate rate, size_t count, unsigned int m,
				 struct esp_sdr_iq16 *out, size_t max_out, size_t *n_out,
				 uint64_t *first_ns);

/**
 * @brief Capture a burst and fold its spectrum by @p n: a power estimation mode.
 *
 * Captures @p count samples like esp_sdr_rx_capture(), then (still holding
 * the radio lock) sums @p n time-shifted blocks of count / n samples each,
 * averaged back to the raw 10-bit scale, into @p out. This is decimation in
 * frequency rather than in time: a tone originally at offset f reappears at
 * f mod (rate / n), summed with every other tone an integer multiple of
 * rate / n away, instead of being low-pass filtered out. The whole original
 * band's power is represented at 1/n the data rate, so it suits a wideband
 * power/occupancy survey; it does not suit recovering an individual signal,
 * since two sources that land in the same fold group add instead of one of
 * them surviving cleanly. Needs CONFIG_ESP_SDR_RX_DECIM.
 *
 * @param rate Native sample rate.
 * @param count Complex samples, ESP_SDR_SAMPLES_MIN to ESP_SDR_SAMPLES_MAX.
 * @param n Fold factor, ESP_SDR_RX_FOLD_MIN to ESP_SDR_RX_FOLD_MAX.
 * @param out Output samples, full scale +-511 (raw 10-bit, averaged not summed).
 * @param max_out Capacity of @p out.
 * @param n_out Set to the number of samples written (count / n).
 * @param first_ns Set to the time of the first input sample in the burst.
 * @retval 0 on success.
 * @retval -EINVAL for an unsupported rate, count or factor.
 * @retval -EAGAIN if esp_sdr_init() has not run.
 * @retval -ETIMEDOUT if the capture engine did not finish.
 */
int esp_sdr_rx_capture_folded(enum esp_sdr_rate rate, size_t count, unsigned int n,
			      struct esp_sdr_iq16 *out, size_t max_out, size_t *n_out,
			      uint64_t *first_ns);

/** @return Capture sample rate in Hz, or 0 for an unknown rate. */
uint32_t esp_sdr_rx_rate_hz(enum esp_sdr_rate rate);

/*
 * Receive words carry Q in the low field: with I low, a tone below the LO
 * showed up above it (checked by detuning the receiver against a known
 * carrier). Transmit words are the other way round, see esp_sdr_tx_word().
 */

/** @return In-phase component of a raw receive word, -512 to 511. */
static inline int16_t esp_sdr_rx_i(uint32_t word)
{
	return (int16_t)((int32_t)(word << 12) >> 22);
}

/** @return Quadrature component of a raw receive word, -512 to 511. */
static inline int16_t esp_sdr_rx_q(uint32_t word)
{
	return (int16_t)((int32_t)(word << 22) >> 22);
}

/**
 * @return The top 8 bits of each component of a raw receive word as an int8 I/Q pair: I
 * (bits 19:12) in the low byte, Q (bits 9:2) in the high one.
 */
static inline __attribute__((always_inline)) uint32_t esp_sdr_rx_iq8(uint32_t word)
{
	return ((word >> 12) & 0xffU) | ((word << 6) & 0xff00U);
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ESP_SDR_ESP_SDR_RX_H_ */
