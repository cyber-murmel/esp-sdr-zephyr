/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Sample sources of vrt_tx that the fake board and the tests share: the tone, a pure function of
 * the stream index (so a gap or a resync keeps the phase), in the iq8 and the word32 formats, and
 * the amplitude check of a block of samples.
 *
 * iq8: a sample is I then Q, signed bytes. word32: the DAC word of the same I and Q
 * (txs_expand.h: txs_iq8_word(I | Q << 8)), 32 bit little endian. The amplitude is in iq8 counts
 * (one count is 4 DAC counts of the +-511 range).
 */

#ifndef TX_SRC_H_
#define TX_SRC_H_

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tx_proto.h"

#define TXSRC_TBL_BITS 10U
#define TXSRC_TBL      (1U << TXSRC_TBL_BITS)

struct txsrc_tone {
	uint32_t rate_hz;
	int64_t freq_hz; /* may be negative */
	int amp;         /* iq8 counts, 1..127 */
	uint32_t fm;     /* freq_hz mod rate_hz */
	uint32_t step;   /* phase step in 2^-32 turns */
	int8_t i[TXSRC_TBL], q[TXSRC_TBL];
};

static inline void txsrc_tone_init(struct txsrc_tone *t, uint32_t rate_hz, int64_t freq_hz, int amp)
{
	int64_t m = freq_hz % (int64_t)rate_hz;

	t->rate_hz = rate_hz;
	t->freq_hz = freq_hz;
	t->amp = amp;
	t->fm = (uint32_t)(m < 0 ? m + (int64_t)rate_hz : m);
	t->step = (uint32_t)(((uint64_t)t->fm << 32) / rate_hz);
	for (uint32_t k = 0; k < TXSRC_TBL; k++) {
		double a = 6.283185307179586 * (double)k / (double)TXSRC_TBL;

		t->i[k] = (int8_t)lround(amp * cos(a));
		t->q[k] = (int8_t)lround(amp * sin(a));
	}
}

/* The DAC word of an iq8 sample, as the board's kernels make it (txs_iq8_word). */
static inline uint32_t txsrc_word(int8_t i, int8_t q)
{
	uint32_t x = (uint32_t)(uint8_t)i | ((uint32_t)(uint8_t)q << 8);

	return ((x & 0xffU) << 2) | ((x & 0xff00U) << 4);
}

/** words samples of the tone from stream index idx. fmt: TXP_FMT_IQ8 or TXP_FMT_WORD32. */
static inline void txsrc_tone_fill(const struct txsrc_tone *t, uint64_t idx, uint32_t words,
				   uint8_t *dst, uint32_t fmt)
{
	/* the exact phase at idx: (idx * fm mod rate) / rate turns, then a 32 bit accumulator */
	uint64_t ph = (uint64_t)(((unsigned __int128)idx * t->fm) % t->rate_hz);
	uint32_t acc = (uint32_t)((ph << 32) / t->rate_hz);

	for (uint32_t k = 0; k < words; k++, acc += t->step) {
		uint32_t e = ((acc + (1U << (31U - TXSRC_TBL_BITS))) >> (32U - TXSRC_TBL_BITS)) & (TXSRC_TBL - 1U);

		if (fmt == TXP_FMT_IQ8) {
			dst[2U * k] = (uint8_t)t->i[e];
			dst[2U * k + 1U] = (uint8_t)t->q[e];
		} else {
			uint32_t w = txsrc_word(t->i[e], t->q[e]);

			dst[4U * k] = (uint8_t)w;
			dst[4U * k + 1U] = (uint8_t)(w >> 8);
			dst[4U * k + 2U] = (uint8_t)(w >> 16);
			dst[4U * k + 3U] = (uint8_t)(w >> 24);
		}
	}
}

/** The largest |I| or |Q| (iq8 counts) in n samples of fmt (a word32's 10 bit fields divided by 4). */
static inline int txsrc_peak(const uint8_t *p, size_t n, uint32_t fmt)
{
	int peak = 0;

	for (size_t k = 0; k < n; k++) {
		int a, b;

		if (fmt == TXP_FMT_IQ8) {
			a = (int8_t)p[2U * k];
			b = (int8_t)p[2U * k + 1U];
		} else {
			uint32_t w = (uint32_t)p[4U * k] | ((uint32_t)p[4U * k + 1U] << 8) |
				     ((uint32_t)p[4U * k + 2U] << 16) | ((uint32_t)p[4U * k + 3U] << 24);
			int i10 = (int)(w & 0x3ffU), q10 = (int)((w >> 10) & 0x3ffU);

			a = (i10 >= 512 ? i10 - 1024 : i10) / 4;
			b = (q10 >= 512 ? q10 - 1024 : q10) / 4;
		}
		a = abs(a);
		b = abs(b);
		peak = a > peak ? a : peak;
		peak = b > peak ? b : peak;
	}
	return peak;
}

#endif /* TX_SRC_H_ */
