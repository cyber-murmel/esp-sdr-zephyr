/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Transmit stream, the PIE kernel set (CONFIG_ESP_SDR_TXS_PIE): see txs_expand.h. The 8+8 bit
 * expansion runs in blocks of 16 samples on the PIE, the ready words are copied 8 at a time; the
 * rest of a packet and the zero fill are the fast set's. CPU 1 only, after txs_pie_enable() on that core.
 */

#include <esp_sdr/esp_sdr_place.h>

#include "txs_expand.h"

uint32_t txs_pie_enable(void);
void txs_pie_expand(uint32_t *dst, const uint8_t *src, uint32_t blocks16);
void txs_pie_copy(uint32_t *dst, const uint8_t *src, uint32_t blocks8);

ESP_SDR_CODE(PACED_SAMPLE, txs_pie_iq8)
static void txs_pie_iq8(uint32_t *dst, const uint8_t *src, uint32_t n)
{
	const uint32_t blocks = n / 16U;

	if (blocks != 0U) {
		txs_pie_expand(dst, src, blocks);
	}
	if ((n & 15U) != 0U) {
		txs_kern_fast.iq8(dst + 16U * blocks, src + 32U * blocks, n & 15U);
	}
}

ESP_SDR_CODE(PACED_SAMPLE, txs_pie_word32)
static void txs_pie_word32(uint32_t *dst, const uint8_t *src, uint32_t n)
{
	const uint32_t blocks = n / 8U;

	if (blocks != 0U) {
		txs_pie_copy(dst, src, blocks);
	}
	if ((n & 7U) != 0U) {
		txs_kern_fast.word32(dst + 8U * blocks, src + 32U * blocks, n & 7U);
	}
}

ESP_SDR_CODE(PACED_SAMPLE, txs_pie_zero)
static void txs_pie_zero(uint32_t *dst, uint32_t n)
{
	txs_kern_fast.zero(dst, n);
}

const struct txs_kern txs_kern_pie = {
	.name = "pie", .iq8 = txs_pie_iq8, .word32 = txs_pie_word32, .zero = txs_pie_zero,
};
