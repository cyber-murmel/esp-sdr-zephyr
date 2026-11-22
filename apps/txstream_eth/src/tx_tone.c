/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The test signal and the network's stand-in: a complex tone of k cycles per 351 samples
 * (k x rate / 351: 57 kHz steps at 20 MS/s), as the payload of a VITA packet.
 */

#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <esp_sdr/esp_sdr_place.h>

#include "tx_tone.h"
#include "tx_tone_gen.h"
#include "txs_expand.h"

static uint32_t tbl_w[TX_TONE_PERIOD] ESP_SDR_DATA_F(BULK, tbl_w, NOINIT);
static uint16_t tbl_8[TX_TONE_PERIOD] ESP_SDR_DATA_F(BULK, tbl_8, NOINIT);

/* The tables are for this k and amp (0: none yet). The tone of the network's verify (TXTG_K, TXTG_AMP) comes from
 * the constant tables of tx_tone_gen.h, with no math; others are computed (soft float: milliseconds). */
static int set_k, set_amp;
static K_MUTEX_DEFINE(tone_lock);

void tx_tone_lock(void)
{
	k_mutex_lock(&tone_lock, K_FOREVER);
}

void tx_tone_unlock(void)
{
	k_mutex_unlock(&tone_lock);
}

void tx_tone_set(int k, int amp)
{
	if (set_k == k && set_amp == amp && k != 0) {
		return;
	}
	set_k = 0;
	for (uint32_t i = 0; i < TX_TONE_PERIOD; i++) {
		if (k == TXTG_K && amp == TXTG_AMP) {
			tbl_w[i] = txtg_w[i];
			tbl_8[i] = (uint16_t)(((uint32_t)(uint8_t)txtg_i8[i]) | (((uint32_t)(uint8_t)txtg_q8[i]) << 8));
		} else {
			const int amp8 = MAX(1, (amp + 2) / 4);
			const double a = 6.283185307179586 * (double)k * (double)i / (double)TX_TONE_PERIOD;
			const int32_t i8 = (int32_t)lround(amp8 * cos(a)), q8 = (int32_t)lround(amp8 * sin(a));
			const int wi = (int)lround(amp * cos(a)), wq = (int)lround(amp * sin(a));

			tbl_w[i] = ((uint32_t)(wi < -511 ? -511 : (wi > 511 ? 511 : wi)) & 0x3ffU) |
				   (((uint32_t)(wq < -511 ? -511 : (wq > 511 ? 511 : wq)) & 0x3ffU) << 10);
			tbl_8[i] = (uint16_t)(((uint32_t)i8 & 0xffU) | (((uint32_t)q8 & 0xffU) << 8));
		}
	}
	set_k = k;
	set_amp = amp;
}

void tx_tone_pack(uint8_t *dst, uint64_t first, uint32_t words, enum txs_fmt fmt)
{
	const uint32_t size = fmt == TXS_FMT_IQ8 ? 2U : 4U;
	const uint8_t *tbl = fmt == TXS_FMT_IQ8 ? (const uint8_t *)tbl_8 : (const uint8_t *)tbl_w;
	uint32_t off = txtg_mod(first);

	while (words != 0U) {
		const uint32_t n = MIN(words, TX_TONE_PERIOD - off);

		memcpy(dst, tbl + (size_t)off * size, (size_t)n * size);
		dst += (size_t)n * size;
		words -= n;
		off = 0U;
	}
}

ESP_SDR_CODE(PACED_CHUNK, tx_tone_word)
uint32_t tx_tone_word(uint64_t idx, enum txs_fmt fmt)
{
	const uint32_t i = txtg_mod(idx);

	return fmt == TXS_FMT_IQ8 ? txs_iq8_word(tbl_8[i]) : tbl_w[i];
}
