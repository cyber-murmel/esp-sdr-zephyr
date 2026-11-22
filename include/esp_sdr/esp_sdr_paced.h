/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file
 * @brief ESP-SDR CPU-paced streaming (ESP32-C6, ESP32-C5; transmit only on the
 * ESP32-S31, which has no IQ estimator): continuous receive and transmit, one
 * sample at a time from the caller's loop.
 *
 * The C6 has no dump path that runs gapless at a low rate and no DAC buffer
 * the CPU can refill while it plays, so the CPU paces both directions:
 *
 * - Receive differences the running sums of the modem's IQ estimator, which
 *   adds up the 80 MHz baseband samples in hardware: each output is the sum
 *   of the 80 MHz / rate samples since the previous one (a boxcar
 *   decimator). Gapless up to about 4.7 MS/s with nothing else in the loop.
 * - Transmit writes the TX DC value register, which drives the DAC directly
 *   once the vendor tone path has turned the transmit chain on, at the rate
 *   times an interpolation factor, through an 8-tap-per-phase FIR.
 *
 * Between start and stop the caller polls in a tight loop with interrupts
 * locked and serves its own I/O between polls. A poll that comes late moves
 * that sample's instant (esp_sdr_rx_paced_fill_iq8() spreads a poll whole
 * periods late over the samples it missed instead); the stats count it. One
 * direction at a time.
 *
 * On the ESP32-S31 the poll runs from IRAM and costs about 220 CPU cycles a write (a TX DC
 * store alone 48): rate times factor up to 1 MHz, such as 250 kS/s x4 or 125 kS/s x8, keeps
 * every write on time. With CONFIG_ESP_SDR_PACED_TX_BLOCK (SMP, off by default) the poll only
 * stores a DAC word computed ahead by a thread on the other core: rate times factor up to
 * 2 MHz, such as 250 kS/s x8 or 500 kS/s x4. Then:
 * - The fill thread is created by esp_sdr_tx_paced_start() on the core the caller is not on,
 *   spins there (cooperative, outranking every thread on that core, no kernel call) until
 *   esp_sdr_tx_paced_stop(), and ends. The caller must stay on its core from the start to the
 *   stop (pin it). Every other thread pinned to the fill core is frozen for the stream's length:
 *   the caller must not need a mutex or a service of such a thread between start and stop.
 *   Start and stop sleep (thread start and join): never call them with interrupts or the
 *   scheduler locked; lock interrupts only after the start and unlock them before the stop.
 * - The caller must call esp_sdr_paced_sync() before polling, and stop the stream when it fails:
 *   the sync has the thread take the first inputs while it waits, 1 s at most (-ETIMEDOUT).
 *   After a failure the poll plays the idle word and counts starved; the played-samples count
 *   stays at 0, so a loop that waits for it must bound itself.
 * - The input callback runs on that thread, up to CONFIG_ESP_SDR_PACED_BLOCKS blocks
 *   (CONFIG_ESP_SDR_PACED_BLOCK_WORDS words each) ahead of the DAC, concurrently with the
 *   caller: its data is shared between two cores, and it must neither take irq_lock() (under
 *   SMP the global lock the caller may hold while polling) nor make kernel calls that wait.
 * - The caller locks interrupts with arch_irq_lock(), not irq_lock(): that is the local mask.
 * - start returns -ENODEV, and nothing is started, when the other core cannot run the thread
 *   (no second core, not up, the pin refused): there is no fall back to the per-word path.
 * Keep the input callback and the polling loop in IRAM with their data in internal RAM; code
 * fetched through the flash cache stalls a write now and then by up to a few microseconds.
 */

#ifndef ESP_SDR_ESP_SDR_PACED_H_
#define ESP_SDR_ESP_SDR_PACED_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup esp_sdr_paced
 * @{
 */

/** Input rate of the estimator: each receive output sums 80 MHz / rate of its samples. */
#define ESP_SDR_PACED_INPUT_HZ 80000000U

struct esp_sdr_paced_stats {
	/** Samples produced (receive) or input samples consumed (transmit). */
	uint64_t samples;
	/** Polls that came a whole period or more late. */
	uint32_t late;
	/** Worst lateness, in CPU cycles. */
	uint32_t late_max;
	/** Receive: estimator restarts. Transmit: input samples that were missing (played as 0). */
	uint32_t events;
	/**
	 * Transmit with CONFIG_ESP_SDR_PACED_TX_BLOCK: blocks of DAC words that were not ready at the
	 * swap (the block played again). The fill thread did not keep up (a slow input callback,
	 * a stalled core): raise CONFIG_ESP_SDR_PACED_BLOCKS.
	 */
	uint32_t starved;
};

/**
 * @brief Start receiving at @p rate_hz.
 *
 * Takes the radio until esp_sdr_rx_paced_stop(). The receiver keeps its
 * frequency, gain and filter.
 *
 * @param rate_hz A divisor of both the CPU clock and 80 MHz, from 10 kHz.
 * @return 0, -EINVAL for an unsupported rate, -EAGAIN before esp_sdr_init().
 */
int esp_sdr_rx_paced_start(uint32_t rate_hz);

/**
 * @brief Produce the next receive sample if it is due.
 *
 * @param i, q The sums over this sample's window of 80 MHz / rate inputs,
 *             each input within -512 to 511.
 * @return true when a sample was produced.
 */
bool esp_sdr_rx_paced_poll(int32_t *i, int32_t *q);

/**
 * @brief Produce the next @p n receive samples as they fall due, as 8-bit I, Q pairs.
 * The tight form of esp_sdr_rx_paced_poll() for MS/s rates: blocks until the last one.
 * @param out 2 * @p n bytes, 2-byte aligned.
 * @param n Samples to produce.
 * A poll a whole period or more late (the caller's I/O between calls) spans
 * that many windows more: each sample it missed gets an equal share of its
 * sums, so that the samples stay on their grid.
 *
 * @param shift Right shift of each sum before it saturates to -127 to 127.
 * @return Polls among them that came a whole period or more late.
 */
uint32_t esp_sdr_rx_paced_fill_iq8(int8_t *out, size_t n, unsigned int shift);

/** @brief Stop receiving and release the radio; no-op when stopped. */
void esp_sdr_rx_paced_stop(void);

/**
 * Transmit input: store the next sample (full scale +-32767) and return true,
 * or return false when none is available (played as 0). Called from the poll,
 * once per input sample: must not block. With CONFIG_ESP_SDR_PACED_TX_BLOCK it runs on the
 * fill thread on the other core, blocks before the sample is played (see the file comment).
 */
typedef bool (*esp_sdr_tx_paced_input_t)(void *user, int16_t *i, int16_t *q);

/**
 * @brief Turn the transmit chain on at the current frequency and start
 * playing input at @p rate_hz.
 *
 * @param rate_hz Input rate; rate times @p interp must divide the CPU clock.
 * @param interp Interpolation factor: 1 (input straight to the DAC), 2, 4 or 8.
 * @param power Power step, 0 weakest to esp_sdr_tx_paced_power_max(). No effect on the
 *        ESP32-C5 and ESP32-S31, whose level follows the samples only
 *        (doc/radio/gain.md).
 * @param input Called once per input sample from esp_sdr_tx_paced_poll() (with
 *        CONFIG_ESP_SDR_PACED_TX_BLOCK the first two blocks of inputs are taken by
 *        esp_sdr_paced_sync(), not by this function).
 * @param user Passed to @p input.
 * @return 0, -EINVAL, -ERANGE outside the transmit range, -EAGAIN before
 *         esp_sdr_init(), -EBUSY when a stream runs (block path), -ENODEV when the fill thread
 *         cannot run (block path).
 */
int esp_sdr_tx_paced_start(uint32_t rate_hz, unsigned int interp, unsigned int power,
			   esp_sdr_tx_paced_input_t input, void *user);

/**
 * @brief Write the next DAC value if it is due, then take the one after (computed, or the
 * next off the block with CONFIG_ESP_SDR_PACED_TX_BLOCK).
 * @return true when a value was written.
 */
bool esp_sdr_tx_paced_poll(void);

/**
 * @brief Input samples played so far by the running transmit stream, the count a caller ends
 * its stream on (not the calls of its input callback, which run ahead of the DAC with
 * CONFIG_ESP_SDR_PACED_TX_BLOCK). Cheap: call it after a poll that returned true.
 */
uint64_t esp_sdr_tx_paced_samples(void);

/** @brief Turn the transmit chain off and release the radio; no-op when stopped. */
void esp_sdr_tx_paced_stop(void);

/** @brief Highest power step: 2 dB per step of the vendor tone's backoff (none on the C5, S31). */
unsigned int esp_sdr_tx_paced_power_max(void);

/** @brief The CPU cycle counter the paced streams run on (inlined into IRAM loops). */
static inline __attribute__((always_inline)) uint32_t esp_sdr_paced_cycles(void)
{
	uint32_t v;

#if defined(CONFIG_SOC_SERIES_ESP32C6)
	/* The C6's machine performance counter (mpccr). */
	__asm__ volatile("csrr %0, 0x7e2" : "=r"(v));
#else
	__asm__ volatile("csrr %0, cycle" : "=r"(v));
#endif
	return v;
}

/** @brief Make esp_sdr_paced_cycles() count CPU cycles (a stream's start does it too). */
static inline void esp_sdr_paced_cycles_enable(void)
{
#if defined(CONFIG_SOC_SERIES_ESP32C6)
	/* The machine performance counter (mpccr) counts CPU cycles. */
	__asm__ volatile("csrw 0x7e0, %0" ::"r"(1));
	__asm__ volatile("csrw 0x7e1, %0" ::"r"(1));
#endif
}

/**
 * @brief When the running stream's next sample is due, on esp_sdr_paced_cycles().
 *
 * For I/O between polls: stop it while (int32_t)(deadline - now) is small.
 */
uint32_t esp_sdr_paced_deadline(void);

/**
 * @brief Restart the running stream's clock: the next sample is due one
 * period from now. Call once interrupts are locked, after the start. With
 * CONFIG_ESP_SDR_PACED_TX_BLOCK the first call also waits for the fill thread's first two blocks
 * of inputs.
 * @return 0, -ETIMEDOUT when they did not come within 1 s (stop the stream; a second call may
 *         still succeed), -EPERM when the caller runs on the fill core. Always 0 otherwise.
 */
int esp_sdr_paced_sync(void);

/** @brief Statistics of the running or last stream. */
void esp_sdr_paced_get_stats(struct esp_sdr_paced_stats *stats);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ESP_SDR_ESP_SDR_PACED_H_ */
