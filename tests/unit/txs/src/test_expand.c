/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The expansion kernels (txs_expand.h): the 8+8 word against esp_sdr_tx_word() and
 * esp_sdr_replay_word() for every 16 bit value, the fast kernels against the reference for
 * every count up to 70 and big ones, sources at 0 and 2 mod 4, canaries around the destination,
 * zero fill of a range, selection by name.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <esp_sdr/esp_sdr_replay.h>
#include <esp_sdr/esp_sdr_tx.h>
#include <test_rng.h>

#include "txs_asan.h"
#include "txs_expand.h"

ZTEST_SUITE(txs_expand, NULL, NULL, NULL, NULL, NULL);

/* The format's definition against the transmit word of the API, every I and Q byte. */
ZTEST(txs_expand, test_word_golden)
{
	for (uint32_t x = 0; x < 65536U; x++) {
		int16_t i = (int8_t)(x & 0xffU), q = (int8_t)(x >> 8);

		zassert_equal(txs_iq8_word(x), esp_sdr_tx_word(i * 4, q * 4), "x %04x", x);
		/* the replay word clamps at -511: only -128 differs, and only there */
		if (i != -128 && q != -128) {
			zassert_equal(txs_iq8_word(x), esp_sdr_replay_word(i * 4, q * 4), "x %04x", x);
		}
	}
	zassert_equal(txs_iq8_word(0x0000), 0U);
	zassert_equal(txs_iq8_word(0x0001), 4U);
	zassert_equal(txs_iq8_word(0x0100), 1U << 12);
	zassert_equal(txs_iq8_word(0x8080), 0x80200U);
	zassert_equal(txs_iq8_word(0xffff), 0x000003fcU | 0x000ff000U);
}

/* Both kernel sets read the bytes I0 Q0 I1 Q1 of the wire as little endian 16 bit samples. */
ZTEST(txs_expand, test_ref_reads_wire_order)
{
	static const uint8_t b[8] = { 0x01, 0x02, 0xfe, 0x80, 0x00, 0x7f, 0x10, 0xf0 };
	uint32_t d[4];

	txs_kern_ref.iq8(d, b, 4);
	for (int k = 0; k < 4; k++) {
		zassert_equal(d[k], esp_sdr_tx_word((int8_t)b[2 * k] * 4, (int8_t)b[2 * k + 1] * 4));
	}
}

#define CAN 0xA5A5A5A5U
#define MAXN 700U

/* Source buffers at 0 and 2 mod 4 (and 4, 6 for variety), 4 byte aligned base. */
static uint8_t srcbuf[4U * MAXN + 16U] __aligned(4);
static uint32_t dstbuf[MAXN + 16U] __aligned(4), refbuf[MAXN + 16U] __aligned(4);

static void compare(const struct txs_kern *k, bool iq8, uint32_t off, uint32_t n)
{
	const struct txs_kern *r = &txs_kern_ref;

	for (uint32_t i = 0; i < ARRAY_SIZE(dstbuf); i++) {
		dstbuf[i] = refbuf[i] = CAN;
	}
	if (iq8) {
		r->iq8(refbuf + 4, srcbuf + off, n);
		k->iq8(dstbuf + 4, srcbuf + off, n);
	} else {
		r->word32(refbuf + 4, srcbuf + off, n);
		k->word32(dstbuf + 4, srcbuf + off, n);
	}
	zassert_mem_equal(dstbuf, refbuf, sizeof(dstbuf), "%s %s off %u n %u", k->name,
			  iq8 ? "iq8" : "word32", off, n);
	/* the reference itself against the definition */
	for (uint32_t i = 0; i < n; i++) {
		if (iq8) {
			uint16_t x = srcbuf[off + 2U * i] | (srcbuf[off + 2U * i + 1U] << 8);

			zassert_equal(refbuf[4 + i], txs_iq8_word(x));
		} else {
			uint32_t w = srcbuf[off + 4U * i] | (srcbuf[off + 4U * i + 1U] << 8) |
				     (srcbuf[off + 4U * i + 2U] << 16) |
				     ((uint32_t)srcbuf[off + 4U * i + 3U] << 24);

			zassert_equal(refbuf[4 + i], w);
		}
	}
}

ZTEST(txs_expand, test_fast_equals_ref)
{
	static const uint32_t offs[] = { 0, 2, 4, 6 };

	test_rng_seed(5);
	for (int pass = 0; pass < 3; pass++) {
		for (uint32_t i = 0; i < sizeof(srcbuf); i++) {
			srcbuf[i] = pass == 0 ? (uint8_t)test_rng_u32() : (pass == 1 ? 0xff : 0x80);
		}
		for (size_t o = 0; o < ARRAY_SIZE(offs); o++) {
			for (uint32_t n = 0; n <= 70U; n++) {
				compare(&txs_kern_fast, true, offs[o], n);
				compare(&txs_kern_fast, false, offs[o], n);
			}
			compare(&txs_kern_fast, true, offs[o], MAXN);
			compare(&txs_kern_fast, false, offs[o], MAXN);
			compare(&txs_kern_fast, true, offs[o], 333);
			compare(&txs_kern_fast, false, offs[o], 349);
		}
	}
}

/*
 * The kernels read nothing outside the payload: every size from 1 to 40 words, the payload at 0
 * and 2 mod 4, in a buffer whose bytes around the payload are poisoned. Under the address
 * sanitizer (the .asan scenario) a read of one aborts the test; without it nothing could show
 * that, so the test says so and skips.
 */
ZTEST(txs_expand, test_no_read_past_the_end)
{
	static uint8_t g[1024] __aligned(16);
	uint32_t d[40], r[40];

	if (!TXS_ASAN) {
		ztest_test_skip();
		return;
	}
	test_rng_seed(6);
	for (uint32_t off = 0; off <= 2U; off += 2U) {
		for (uint32_t n = 1; n <= 40U; n++) {
			TXS_UNPOISON(g, sizeof(g));
			for (uint32_t i = 0; i < sizeof(g); i++) {
				g[i] = (uint8_t)test_rng_u32();
			}
			/* the payload at 64 + off (0 or 2 mod 4): nothing before or after may be read */
			txs_guard(g, sizeof(g), 64U + off, 2U * n);
			txs_kern_fast.iq8(d, g + 64U + off, n);
			txs_kern_ref.iq8(r, g + 64U + off, n);
			zassert_mem_equal(d, r, 4U * n);
			txs_guard(g, sizeof(g), 64U + off, 4U * n);
			txs_kern_fast.word32(d, g + 64U + off, n);
			txs_kern_ref.word32(r, g + 64U + off, n);
			zassert_mem_equal(d, r, 4U * n);
		}
	}
	TXS_UNPOISON(g, sizeof(g));
}

ZTEST(txs_expand, test_zero_range)
{
	static const struct txs_kern *ks[] = { &txs_kern_ref, &txs_kern_fast };

	for (size_t k = 0; k < ARRAY_SIZE(ks); k++) {
		for (uint32_t n = 0; n < 70U; n++) {
			for (uint32_t st = 0; st < 5U; st++) {
				for (uint32_t i = 0; i < 100U; i++) {
					dstbuf[i] = CAN;
				}
				ks[k]->zero(dstbuf + st, n);
				for (uint32_t i = 0; i < 100U; i++) {
					zassert_equal(dstbuf[i], (i >= st && i < st + n) ? 0U : CAN,
						      "%s n %u st %u i %u", ks[k]->name, n, st, i);
				}
			}
		}
	}
}

ZTEST(txs_expand, test_find)
{
	zassert_equal(txs_kern_find("ref"), &txs_kern_ref);
	zassert_equal(txs_kern_find("fast"), &txs_kern_fast);
	zassert_is_null(txs_kern_find("pie"));
	zassert_is_null(txs_kern_find(""));
}
