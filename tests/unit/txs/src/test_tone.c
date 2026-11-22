/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The network verify's two ends agree: what vrt_tx --source board makes (tx_tone_gen.h, packed in a VITA
 * packet by txs_wire_header()), parsed by txs_wire_parse() and expanded by a kernel at the source
 * alignments a frame has (2 mod 4) and after a split at a window's end, is the word the board's
 * reference (tx_tone_word(), the tables of tx_tone_set()) says the bank holds, for random stream
 * indexes and both formats.
 */

#include <math.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <test_rng.h>

#include "tx_tone.h"
#include "tx_tone_gen.h"
#include "txs_expand.h"
#include "txs_wire.h"

ZTEST_SUITE(tx_tone, NULL, NULL, NULL, NULL, NULL);

/* The host's payload: the same code as source_fill() of vrt_tx.c for SRC_BOARD. */
static void host_fill(uint64_t idx, uint32_t words, uint8_t *dst, enum txs_fmt fmt)
{
	for (uint32_t k = 0; k < words; k++) {
		if (fmt == TXS_FMT_IQ8) {
			int8_t i8, q8;

			txtg_iq8(idx + k, &i8, &q8);
			dst[2U * k] = (uint8_t)i8;
			dst[2U * k + 1U] = (uint8_t)q8;
		} else {
			const uint32_t w = txtg_word(idx + k);

			dst[4U * k] = (uint8_t)w;
			dst[4U * k + 1U] = (uint8_t)(w >> 8);
			dst[4U * k + 2U] = (uint8_t)(w >> 16);
			dst[4U * k + 3U] = (uint8_t)(w >> 24);
		}
	}
}

static void run(const struct txs_kern *kern, enum txs_fmt fmt)
{
	const struct txs_wire_cfg wc = {.stream_id = 0x1234abcdU, .port = 4994, .fmt = fmt, .max_words = 702};
	/* a frame's payload starts at 2 mod 4: a 2 byte pad in front of a 4 aligned buffer */
	static uint8_t pk[2U + 16U + 4U * 702U + 16U] __aligned(4);
	static uint32_t bank[702U + 32U];

	for (unsigned int it = 0; it < 4000U; it++) {
		const uint64_t idx = ((uint64_t)test_rng_u32() << 32 | test_rng_u32()) >> (it % 40U);
		const uint32_t max = fmt == TXS_FMT_IQ8 ? 702U : 351U;
		const uint32_t words = 2U + (test_rng_u32() % (max / 2U)) * 2U;
		const uint32_t used = test_rng_u32() % (words + 1U); /* the part before a window's end */
		struct txs_wire_pkt p;
		const uint32_t bps = fmt == TXS_FMT_IQ8 ? 2U : 4U;

		txs_wire_header(pk + 2, &wc, 0, idx, words);
		host_fill(idx, words, pk + 2 + 16, fmt);
		zassert_equal(txs_wire_parse(&wc, 4994, pk + 2, 16U + bps * words, &p), TXS_WIRE_OK);
		zassert_equal(p.index, idx);
		zassert_equal(p.words, words);
		tx_tone_set(TXTG_K, TXTG_AMP);
		memset(bank, 0xa5, sizeof(bank));
		/* the head in one call, the tail (after the window's end) in another, as the stream does */
		if (used != 0U) {
			if (fmt == TXS_FMT_IQ8) {
				kern->iq8(bank, p.payload, used);
			} else {
				kern->word32(bank, p.payload, used);
			}
		}
		if (used != words) {
			if (fmt == TXS_FMT_IQ8) {
				kern->iq8(bank + used, p.payload + (size_t)used * bps, words - used);
			} else {
				kern->word32(bank + used, p.payload + (size_t)used * bps, words - used);
			}
		}
		for (uint32_t k = 0; k < words; k++) {
			zassert_equal(bank[k], tx_tone_word(idx + k, fmt), "%s %s idx %llu word %u of %u (split %u)",
				      kern->name, fmt == TXS_FMT_IQ8 ? "iq8" : "w32", (unsigned long long)idx, k,
				      words, used);
		}
		zassert_equal(bank[words], 0xa5a5a5a5U, "written past the end");
	}
}

ZTEST(tx_tone, test_ref_iq8)
{
	run(&txs_kern_ref, TXS_FMT_IQ8);
}

ZTEST(tx_tone, test_fast_iq8)
{
	run(&txs_kern_fast, TXS_FMT_IQ8);
}

ZTEST(tx_tone, test_ref_w32)
{
	run(&txs_kern_ref, TXS_FMT_WORD32);
}

ZTEST(tx_tone, test_fast_w32)
{
	run(&txs_kern_fast, TXS_FMT_WORD32);
}

/* Half away from zero, a value within 1e-9 of a half is the half: what tools/gen_tone_table.py does. */
static int rnd(double x)
{
	const double ax = fabs(x), fl = floor(ax);
	const int r = (int)(fabs(ax - fl - 0.5) < 1e-9 ? fl + 1.0 : floor(ax + 0.5));

	return x < 0 ? -r : r;
}

/* The constant tables are the exact tone, the tie samples (cosine or sine exactly +-0.5: the computed value
 * rounds either way, which is what made the board and the host disagree) away from zero; the board's table
 * for the default tone is the same one, with no math. */
ZTEST(tx_tone, test_table_is_the_tone)
{
	unsigned int ties = 0;

	tx_tone_set(TXTG_K, TXTG_AMP);
	for (uint32_t i = 0; i < TXTG_PERIOD; i++) {
		const double a = 6.283185307179586 * TXTG_K * (double)i / (double)TXTG_PERIOD;
		const double c = cos(a), s = sin(a);
		const int wi = rnd(TXTG_AMP * c), wq = rnd(TXTG_AMP * s);
		const uint32_t w = ((uint32_t)wi & 0x3ffU) | (((uint32_t)wq & 0x3ffU) << 10);
		int8_t i8, q8;

		txtg_iq8(i, &i8, &q8);
		zassert_equal(i8, rnd(c), "I of %u", i);
		zassert_equal(q8, rnd(s), "Q of %u", i);
		zassert_equal(txtg_word(i), w, "word of %u", i);
		/* and the board's reference holds the same */
		zassert_equal(tx_tone_word(i, TXS_FMT_WORD32), w);
		zassert_equal(tx_tone_word(i, TXS_FMT_IQ8),
			      txs_iq8_word((uint16_t)((uint8_t)i8 | ((uint16_t)(uint8_t)q8 << 8))));
		if (fabs(fabs(c) - 0.5) < 1e-9 || fabs(fabs(s) - 0.5) < 1e-9) {
			ties++;
			zassert_true(i8 != 0 || fabs(c) < 0.4, "a tie rounds away from zero");
		}
	}
	zassert_equal(ties, 18U, "samples at exactly +-0.5");
	/* the one that the board got wrong: sample 130 is at 240 degrees, cosine -0.5 */
	zassert_equal(txtg_i8[130], -1);
}

/* The tone never makes a zero word (so a zero in the bank is a gap, not data), in both formats. */
ZTEST(tx_tone, test_never_zero)
{
	tx_tone_set(TXTG_K, TXTG_AMP);
	for (uint32_t i = 0; i < TXTG_PERIOD; i++) {
		zassert_not_equal(tx_tone_word(i, TXS_FMT_IQ8), 0U);
		zassert_not_equal(tx_tone_word(i, TXS_FMT_WORD32), 0U);
	}
}

/* The index is 64 bit: 2^32 mod 351 is 22, so the reduction with 32 bit arithmetic must carry the high word. */
ZTEST(tx_tone, test_index_past_2_32)
{
	static const uint64_t idx[] = {0xffffffffULL, 0x100000000ULL, 0x100000001ULL, 351ULL << 32,
				       (351ULL << 32) + 350U, 0x123456789abcdefULL, 0xfffffffffffff000ULL,
				       (1ULL << 40) + 12345U, 32768ULL + (3ULL << 33)};
	uint8_t buf[2U * 702U];

	tx_tone_set(TXTG_K, TXTG_AMP);
	for (size_t n = 0; n < ARRAY_SIZE(idx); n++) {
		for (uint32_t d = 0; d < 800U; d++) {
			const uint64_t i = idx[n] + d;
			const uint32_t want = (uint32_t)(i % TXTG_PERIOD);
			int8_t a, b;

			zassert_equal(txtg_mod(i), want, "idx %llu", (unsigned long long)i);
			txtg_iq8(i, &a, &b);
			zassert_equal(a, txtg_i8[want]);
			zassert_equal(txtg_word(i), txtg_w[want]);
			zassert_equal(tx_tone_word(i, TXS_FMT_WORD32), txtg_w[want]);
		}
		/* the injector's packing starts anywhere in the period */
		tx_tone_pack(buf, idx[n], 702U, TXS_FMT_IQ8);
		for (uint32_t k = 0; k < 702U; k++) {
			const uint32_t want = (uint32_t)((idx[n] + k) % TXTG_PERIOD);

			zassert_equal((int8_t)buf[2U * k], txtg_i8[want], "pack idx %llu + %u", (unsigned long long)idx[n], k);
			zassert_equal((int8_t)buf[2U * k + 1U], txtg_q8[want]);
		}
	}
}
