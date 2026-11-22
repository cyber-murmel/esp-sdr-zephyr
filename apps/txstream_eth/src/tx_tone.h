/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TX_TONE_H_
#define TX_TONE_H_

#include <stdint.h>

#include "txs_wire.h"

/* A packet is 351 words (ready words) or 702 (8+8 bit): both a whole number of periods, so every
 * packet that starts at a multiple of 351 carries the same payload (the injector prepares it once).
 */
#include "tx_tone_gen.h"

#define TX_TONE_PERIOD TXTG_PERIOD

/**
 * The tone, k cycles of a complex exponential per 351 samples (1 to 175), amplitude amp in DAC counts (1 to
 * 511, as esp_sdr_replay_word()). Two tables: ready words, and 8+8 bit samples of amp / 4 (at
 * least 1) byte steps, the DAC words of which are 4 x I and 4 x Q. Sample i of the stream is entry
 * i mod 351: phase continuous across any gap.
 */
void tx_tone_set(int k, int amp);

/** Held from setting the tables to the start of the run that uses them (tx tone, the verify of a net run): two
 * of them cannot set different tones under each other, and nothing sets them while a run is going on. */
void tx_tone_lock(void);
void tx_tone_unlock(void);

/** The payload of the packet of `words` DAC words starting at stream index `first`, in `fmt`. */
void tx_tone_pack(uint8_t *dst, uint64_t first, uint32_t words, enum txs_fmt fmt);

/** The DAC word the stream has at index idx in `fmt`: what the bank must hold if it is not zero. */
uint32_t tx_tone_word(uint64_t idx, enum txs_fmt fmt);

#endif
