/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Module-internal DAC session for streaming transmit backends.
 */

#ifndef ESP_SDR_DAC_H_
#define ESP_SDR_DAC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_sdr/esp_sdr.h>

/* Take the receiver lock and switch the front end to TX at the current step. */
int esp_sdr_dac_begin(enum esp_sdr_rate rate);

/*
 * Two DAC banks: 0 is the capture bank (SRAM bank 2), 1 the transmit bank
 * (SRAM bank 1, reserved by esp_sdr_bank1.ld). The engine owns the bank it
 * plays, the CPUs the other one: fill that one with plain stores, then
 * esp_sdr_dac_start() it once the previous burst is done.
 */
#define ESP_SDR_DAC_BANKS 2

/* Bank idx and its size in words. */
uint32_t *esp_sdr_dac_buf(int idx, size_t *words);

/* Hand bank idx to the engine and play its first count words; returns at once. */
void esp_sdr_dac_start(int idx, size_t count);

/* Wait for the burst started last; false on timeout. */
bool esp_sdr_dac_wait(void);

/*
 * Loop mode: with the trigger bit held the engine replays the first count
 * words of the selected bank back to back, gapless, at exactly the DAC rate
 * (stores to other banks do not slow it), until esp_sdr_dac_halt(). It reads
 * whichever bank is selected at the moment, so esp_sdr_dac_select() switches
 * mid-pass at the current offset. Call with interrupts locked to timestamp
 * the start: sample s plays s DAC periods after the call.
 */
void esp_sdr_dac_loop(int idx, size_t count);

/* Hand bank idx to the engine (the CPUs keep every other one). */
void esp_sdr_dac_select(int idx);

/* Stop the engine at once. */
void esp_sdr_dac_halt(void);

/* Back to RX and release the lock. */
void esp_sdr_dac_end(void);

#endif /* ESP_SDR_DAC_H_ */
