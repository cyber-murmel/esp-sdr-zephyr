/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Transmit stream, expansion kernels (doc/dev/txs.md): the payload of a packet to DAC words in the
 * bank. A kernel set is a table, so that the hardware's PIE version can be chosen at run time;
 * the portable reference is the golden one. Counts are DAC words, dst is word aligned, src is
 * 16 bit aligned (the payload sits at 2 mod 4 in a frame buffer, or at 0 mod 4). Little endian.
 * No Zephyr calls: runs on native_sim.
 *
 * iq8: a sample is two bytes, I then Q, signed; the DAC word is
 * ((x & 0xff) << 2) | ((x & 0xff00) << 4) for the 16 bit value x = I | Q << 8, that is
 * esp_sdr_tx_word(4 * I, 4 * Q). word32: ready DAC words, copied.
 */

#ifndef TXS_EXPAND_H_
#define TXS_EXPAND_H_

#include <stdint.h>

struct txs_kern {
	const char *name;
	void (*iq8)(uint32_t *dst, const uint8_t *src, uint32_t n);
	void (*word32)(uint32_t *dst, const uint8_t *src, uint32_t n);
	void (*zero)(uint32_t *dst, uint32_t n);
};

/** The reference: a sample at a time, the golden. */
extern const struct txs_kern txs_kern_ref;
/** Two samples per 32 bit load, a halfword at an unaligned start and end. */
extern const struct txs_kern txs_kern_fast;
#ifdef CONFIG_ESP_SDR_TXS_PIE
/** The PIE kernels (hardware only), same interface. */
extern const struct txs_kern txs_kern_pie;
#endif

/** @return the kernel set of that name ("ref", "fast", "pie" where built), or 0. */
const struct txs_kern *txs_kern_find(const char *name);

/** The DAC word of the 16 bit value x (the definition of the 8+8 format). */
static inline uint32_t txs_iq8_word(uint32_t x)
{
	return ((x & 0xffU) << 2) | ((x & 0xff00U) << 4);
}

#endif /* TXS_EXPAND_H_ */
