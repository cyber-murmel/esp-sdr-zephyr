/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A hot-swap kernel for the stage tx_gen of the 802.15.4 driver that is wrong on purpose: the built-in
 * algorithm (ieee154_dsp_tx_gen()), then one chip of the frame inverted, in the two slots it is in
 * (its own, where its pulse starts, and the next, where it ends). It shows that the oracle sees a
 * different generator: `wpan bench ack` after `multiapp stage put tx_gen ...` of this kernel says
 * "words differ from the built-in" with a number other than 0 (tx_gen_ref gives 0), and what the
 * ring plays has a chip flipped. Chip 500 is the second byte of the PSDU (the frame control's high byte,
 * in an ACK as in every frame), so every frame has it, inside the part the ACK's ring builds late.
 * Built INTERNAL like tx_gen_ref (see there).
 */

#include <esp_sdr/esp_sdr_place.h>

#include <multiapp/stage.h>

#include "ieee154_dsp.h"

#define ALT_CHIP 500U

/* The state of a chip as its slot template index (0 none, 1 and 2 the chip values), inverted. */
static inline __attribute__((always_inline)) uint32_t inverted(uint32_t state)
{
	return state == 0U ? 0U : 3U - state;
}

ESP_SDR_CODE(PACED_SAMPLE, tx_gen_alt)
static __attribute__((noclone, optimize("O3", "no-tree-loop-distribute-patterns",
					"no-unswitch-loops"))) void
tx_gen_alt(void *user, uint32_t *dst, uint64_t index, uint32_t n)
{
	const struct ieee154_dsp_tx *tx = user;

	ieee154_dsp_tx_gen(tx, dst, index, n);
	/* Slot s holds chip s's first half (cur) and chip s - 1's second half (prv). */
	for (uint32_t s = ALT_CHIP; s <= ALT_CHIP + 1U; s++) {
		uint32_t cur = (uint32_t)ieee154_dsp_chip(tx, s);
		uint32_t prv = (uint32_t)ieee154_dsp_chip(tx, s - 1U);
		const uint32_t *src;

		if (s == ALT_CHIP) {
			cur = inverted(cur);
		} else {
			prv = inverted(prv);
		}
		src = (*tx->tmpl)[s & 1U][cur][prv];
		for (uint32_t k = 0; k < IEEE154_DSP_SLOT; k++) {
			const uint64_t i = (uint64_t)s * IEEE154_DSP_SLOT + k;

			if (i >= index && i - index < n) {
				dst[i - index] = src[k];
			}
		}
	}
}

MULTIAPP_KERNEL_DEFINE(tx_gen, ieee154_dsp_tx_gen_fn_t, tx_gen_alt);
