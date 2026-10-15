/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file
 * @brief ESP-SDR loop replay through the modem DAC engine (ESP32-C5).
 *
 * The DAC engine plays words out of the dump bank on its own clock, at 80 or
 * 40 MS/s: one pass, a number of passes, or endlessly until stopped. The CPU is
 * free meanwhile. Only periodic content fits: the engine owns the whole SRAM
 * block while it plays, so the CPU can neither refill the buffer nor run the
 * diagnostic-bus ring (esp_sdr_diag.h) in that time.
 *
 * Write the samples with esp_sdr_replay_word() into esp_sdr_replay_buf()
 * before esp_sdr_replay_start(): the engine drops CPU stores to its block.
 *
 * Registers after upstream ESP-SDR's first C5 transmitter (git show
 * e605684:main/c5_main.c, emit_dma()).
 */

#ifndef ESP_SDR_ESP_SDR_REPLAY_H_
#define ESP_SDR_ESP_SDR_REPLAY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_sdr/esp_sdr.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup esp_sdr_replay
 * @{
 */

/** Largest number of words in one pass (the engine's 14-bit count). */
#define ESP_SDR_REPLAY_MAX 16383U

/** Replay settings. */
struct esp_sdr_replay_cfg {
	/** ESP_SDR_RATE_80MSPS or ESP_SDR_RATE_40MSPS. */
	enum esp_sdr_rate rate;
	/** Words per pass, 1 to ESP_SDR_REPLAY_MAX. */
	size_t count;
	/** Passes: 0 endless (until esp_sdr_replay_stop()), else 1 to 255. */
	unsigned int passes;
	/**
	 * Transmit power step, 0 to 15, as esp_sdr_tx_paced_start(). Measured without
	 * effect: the level follows the samples times the scale.
	 */
	unsigned int power;
	/**
	 * DAC digital scale per branch, 1 to 255; 0 takes the default (15, as upstream). The
	 * chain compresses above samples times scale of about 1000.
	 */
	uint8_t scale;
	/** Bench use: bits ORed into the trigger word. 0 normally. */
	uint32_t ctrl_extra;
};

/** @return DAC word for I and Q, each -511 to 511 (clamped): I in bits 9:0, Q in 19:10. */
static inline uint32_t esp_sdr_replay_word(int32_t i, int32_t q)
{
	i = i < -511 ? -511 : (i > 511 ? 511 : i);
	q = q < -511 ? -511 : (q > 511 ? 511 : q);
	return ((uint32_t)i & 0x3ffU) | (((uint32_t)q & 0x3ffU) << 10);
}

/**
 * @brief The sample buffer the engine plays from.
 *
 * @param words Set to its size in words.
 * @return The buffer; write it only while no replay runs.
 */
uint32_t *esp_sdr_replay_buf(size_t *words);

/**
 * @brief Turn the transmit chain on and start the engine.
 *
 * Holds the radio until esp_sdr_replay_stop(), at the frequency set with
 * esp_sdr_set_freq().
 *
 * @return 0, -EINVAL for a bad setting, -EAGAIN before esp_sdr_init(),
 *	   -EBUSY while a replay runs, -ERANGE for a frequency the chain cannot take.
 */
int esp_sdr_replay_start(const struct esp_sdr_replay_cfg *cfg);

/**
 * @brief Wait for a finite replay to finish.
 *
 * @param timeout_us Longest wait.
 * @param cycles Set to the CPU cycles from the start to the done flag
 *	   (esp_sdr_paced_cycles() clock); may be NULL.
 * @return 0, -ETIMEDOUT, or -EINVAL for no replay or an endless one.
 */
int esp_sdr_replay_wait(uint32_t timeout_us, uint32_t *cycles);

/**
 * @brief Start a finished replay again, with the same settings and samples.
 *
 * The output pauses between the done flag and the restart (see
 * doc/hardware-quirks.md). Interrupts should be locked around wait and restart.
 *
 * @return 0, or -EINVAL for no replay.
 */
int esp_sdr_replay_restart(void);

/** @return The trigger register as last read (bench use). */
uint32_t esp_sdr_replay_ctrl(void);

/** @return Whether a replay holds the radio. */
bool esp_sdr_replay_active(void);

/** @brief Stop the engine, give the bank back and return to receive. */
void esp_sdr_replay_stop(void);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ESP_SDR_ESP_SDR_REPLAY_H_ */
