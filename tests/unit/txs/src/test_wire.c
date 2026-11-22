/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The wire format (txs_wire.h): a good packet in both formats, every field's check, the payload's
 * address rule, truncation at every length and random input (nothing accepted that is not a
 * packet, nothing read outside the buffer given, and what is accepted is self consistent).
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <test_rng.h>

#include "txs_asan.h"
#include "txs_wire.h"

static const struct txs_wire_cfg CFG = {
	.stream_id = 0xdeadbeefU, .port = 4994, .fmt = TXS_FMT_IQ8, .max_words = 400,
};

/* A frame buffer: the UDP payload sits at 2 mod 4 as in a received frame. */
static uint8_t frame[2 + 16 + 1600 + 8] __aligned(4);
#define PKT (frame + 2)

ZTEST_SUITE(txs_wire, NULL, NULL, NULL, NULL, NULL);

static size_t build(const struct txs_wire_cfg *c, uint64_t idx, uint32_t words, uint8_t count)
{
	size_t bytes = c->fmt == TXS_FMT_IQ8 ? 2U * words : 4U * words;

	txs_wire_header(PKT, c, count, idx, words);
	for (size_t i = 0; i < bytes; i++) {
		PKT[16 + i] = (uint8_t)(i * 7U + 1U);
	}
	return 16U + bytes;
}

ZTEST(txs_wire, test_good_iq8_and_word32)
{
	struct txs_wire_cfg c = CFG;
	struct txs_wire_pkt o;
	size_t len;

	len = build(&c, 0x123456789abcULL, 350, 9);
	zassert_equal(txs_wire_parse(&c, 4994, PKT, len, &o), TXS_WIRE_OK);
	zassert_equal(o.index, 0x123456789abcULL);
	zassert_equal(o.words, 350U);
	zassert_equal(o.bytes, 700U);
	zassert_equal(o.count, 9U);
	zassert_equal(o.payload, PKT + 16);
	zassert_equal(((uintptr_t)o.payload & 3U), 2U, "payload at 2 mod 4");
	/* the bytes: version 0x1010 0000 | count | size words (16 + 700) / 4 = 179 */
	zassert_equal(PKT[0], 0x10);
	zassert_equal(PKT[1], 0x19);
	zassert_equal(PKT[2], 0x00);
	zassert_equal(PKT[3], 179U);
	zassert_equal(PKT[4], 0xde);
	zassert_equal(PKT[7], 0xef);

	c.fmt = TXS_FMT_WORD32;
	len = build(&c, 77, 350, 0);
	zassert_equal(txs_wire_parse(&c, 4994, PKT, len, &o), TXS_WIRE_OK);
	zassert_equal(o.words, 350U);
	zassert_equal(o.bytes, 1400U);
	/* the same packet read as IQ8 is 700 words: too many for this session */
	c.fmt = TXS_FMT_IQ8;
	zassert_equal(txs_wire_parse(&c, 4994, PKT, len, &o), TXS_WIRE_BIG);
	/* the largest the session allows, and one more */
	c.max_words = 350;
	len = build(&c, 0, 350, 0);
	zassert_equal(txs_wire_parse(&c, 4994, PKT, len, &o), TXS_WIRE_OK);
	c.max_words = 349;
	zassert_equal(txs_wire_parse(&c, 4994, PKT, len, &o), TXS_WIRE_BIG);
}

ZTEST(txs_wire, test_header_fields)
{
	struct txs_wire_pkt o;
	size_t len = build(&CFG, 5, 100, 3);
	uint8_t good[16];

	memcpy(good, PKT, 16);
	/* the index word, the count and the stream ID words */
	zassert_equal(txs_wire_parse(&CFG, 4994, PKT, len, &o), TXS_WIRE_OK);
	/* port */
	zassert_equal(txs_wire_parse(&CFG, 4993, PKT, len, &o), TXS_WIRE_PORT);
	/* stream ID: every bit */
	for (int b = 0; b < 32; b++) {
		PKT[4 + b / 8] ^= (uint8_t)(0x80U >> (b % 8));
		zassert_equal(txs_wire_parse(&CFG, 4994, PKT, len, &o), TXS_WIRE_STREAM, "bit %d", b);
		memcpy(PKT, good, 16);
	}
	/* word 0: the type, C, T, the reserved and the TSI and TSF bits, all rejected; count and size */
	for (int b = 20; b < 32; b++) {
		PKT[3 - b / 8] ^= (uint8_t)(1U << (b % 8));
		zassert_not_equal(txs_wire_parse(&CFG, 4994, PKT, len, &o), TXS_WIRE_OK, "bit %d", b);
		memcpy(PKT, good, 16);
	}
	/* TSI none is the only one, TSF sample count the only one */
	PKT[1] = 0x00 | 0x09; /* TSF none */
	zassert_equal(txs_wire_parse(&CFG, 4994, PKT, len, &o), TXS_WIRE_TIME);
	PKT[1] = 0x29; /* TSF real time */
	zassert_equal(txs_wire_parse(&CFG, 4994, PKT, len, &o), TXS_WIRE_TIME);
	PKT[1] = 0x39;
	zassert_equal(txs_wire_parse(&CFG, 4994, PKT, len, &o), TXS_WIRE_TIME);
	PKT[1] = 0x59; /* TSI UTC, TSF sample count */
	zassert_equal(txs_wire_parse(&CFG, 4994, PKT, len, &o), TXS_WIRE_TIME);
	memcpy(PKT, good, 16);
	/* type: context (4), data without ID (0), extension (2) */
	PKT[0] = 0x40;
	zassert_equal(txs_wire_parse(&CFG, 4994, PKT, len, &o), TXS_WIRE_TYPE);
	PKT[0] = 0x00;
	zassert_equal(txs_wire_parse(&CFG, 4994, PKT, len, &o), TXS_WIRE_TYPE);
	PKT[0] = 0x14; /* trailer */
	zassert_equal(txs_wire_parse(&CFG, 4994, PKT, len, &o), TXS_WIRE_TYPE);
	PKT[0] = 0x18; /* class ID */
	zassert_equal(txs_wire_parse(&CFG, 4994, PKT, len, &o), TXS_WIRE_TYPE);
	memcpy(PKT, good, 16);
	/* the count is free, 16 values */
	for (uint8_t k = 0; k < 16; k++) {
		PKT[1] = (uint8_t)(0x10 | k);
		zassert_equal(txs_wire_parse(&CFG, 4994, PKT, len, &o), TXS_WIRE_OK);
		zassert_equal(o.count, k);
	}
}

ZTEST(txs_wire, test_size_field_and_length)
{
	struct txs_wire_pkt o;
	size_t len = build(&CFG, 5, 100, 0);

	zassert_equal(txs_wire_parse(&CFG, 4994, PKT, len, &o), TXS_WIRE_OK);
	/* size off by one either way, length not a multiple of 4, longer than the size says */
	PKT[3]++;
	zassert_equal(txs_wire_parse(&CFG, 4994, PKT, len, &o), TXS_WIRE_SIZE);
	PKT[3] -= 2;
	zassert_equal(txs_wire_parse(&CFG, 4994, PKT, len, &o), TXS_WIRE_SIZE);
	PKT[3]++;
	zassert_equal(txs_wire_parse(&CFG, 4994, PKT, len - 1U, &o), TXS_WIRE_SIZE);
	zassert_equal(txs_wire_parse(&CFG, 4994, PKT, len + 4U, &o), TXS_WIRE_SIZE);
	/* no payload at all, and less */
	zassert_equal(txs_wire_parse(&CFG, 4994, PKT, 16, &o), TXS_WIRE_SHORT);
	zassert_equal(txs_wire_parse(&CFG, 4994, PKT, 0, &o), TXS_WIRE_SHORT);
}

ZTEST(txs_wire, test_alignment_rule)
{
	static uint8_t keep[16 + 200];
	struct txs_wire_pkt o;
	size_t len = build(&CFG, 5, 100, 0);

	memcpy(keep, PKT, len);
	/* at 2 mod 4 (frame) and at 0 mod 4 the payload is at an even address */
	memcpy(frame + 4, keep, len);
	zassert_equal(txs_wire_parse(&CFG, 4994, frame + 4, len, &o), TXS_WIRE_OK);
	zassert_equal(((uintptr_t)o.payload & 3U), 0U);
	/* an odd address is refused */
	memcpy(frame + 5, keep, len);
	zassert_equal(txs_wire_parse(&CFG, 4994, frame + 5, len, &o), TXS_WIRE_ALIGN);
	memcpy(frame + 7, keep, len);
	zassert_equal(txs_wire_parse(&CFG, 4994, frame + 7, len, &o), TXS_WIRE_ALIGN);
}

ZTEST(txs_wire, test_index_overflow)
{
	struct txs_wire_pkt o;
	size_t len = build(&CFG, UINT64_MAX - 100U, 100, 0);

	zassert_equal(txs_wire_parse(&CFG, 4994, PKT, len, &o), TXS_WIRE_OK);
	len = build(&CFG, UINT64_MAX - 99U, 100, 0);
	zassert_equal(txs_wire_parse(&CFG, 4994, PKT, len, &o), TXS_WIRE_INDEX);
}

/*
 * The parser may not look outside [udp, udp + len): every length from 0 to the full packet and a
 * bit more, in a buffer whose bytes around the packet are poisoned (the address sanitizer's
 * .asan scenario aborts on a read of one; without it the verdicts are still checked). Only the
 * full length is a packet.
 */
ZTEST(txs_wire, test_truncated_at_every_length)
{
	static uint8_t g[1024] __aligned(16);
	struct txs_wire_cfg c = CFG;
	struct txs_wire_pkt o;

	for (int f = 0; f < 2; f++) {
		size_t len;

		c.fmt = f ? TXS_FMT_WORD32 : TXS_FMT_IQ8;
		len = build(&c, 99, 120, 0);
		for (size_t l = 0; l <= len + 8U; l++) {
			enum txs_wire_err e;

			TXS_UNPOISON(g, sizeof(g));
			memset(g, 0xff, sizeof(g));
			memcpy(g + 64, PKT, MIN(l, len));
			txs_guard(g, sizeof(g), 64, l);
			e = txs_wire_parse(&c, 4994, g + 64, l, &o);
			zassert_equal(e == TXS_WIRE_OK, l == len, "fmt %d len %zu err %d", f, l, e);
		}
	}
	TXS_UNPOISON(g, sizeof(g));
}

/* Noise is never a packet; a good header with one bit flipped is one only if the bit is free. */
ZTEST(txs_wire, test_random_input)
{
	struct txs_wire_pkt o;
	static uint8_t g[256] __aligned(16);
	uint8_t *buf = g + 64;
	uint32_t ok = 0, noise = 0, flips = 0;

	test_rng_seed(11);
	for (uint32_t it = 0; it < 300000U; it++) {
		size_t len = 1U + test_rng_u32() % 80U;
		uint32_t mode = it % 4U;
		bool want = false;
		enum txs_wire_err e;

		TXS_UNPOISON(g, sizeof(g));
		for (size_t i = 0; i < len; i++) {
			buf[i] = (uint8_t)test_rng_u32();
		}
		if (mode != 0U && len >= 16U) {
			/* a header made for this length, with random count and index */
			/* the size field the header gets, in words (the payload rounds down to samples) */
			uint32_t size0 = (16U + 2U * ((uint32_t)(len - 16U) / 2U)) / 4U;
			bool fits = len >= 20U && ((len - 16U) % 4U) == 0U;
			size_t cut = 0;

			txs_wire_header(buf, &CFG, (uint8_t)test_rng_u32(),
					((uint64_t)test_rng_u32() << 32) | test_rng_u32(), (len - 16U) / 2U);
			want = fits;
			if (mode == 2U) {
				uint32_t b = test_rng_u32() % 16U, k = test_rng_u32() % 8U;

				buf[b] ^= (uint8_t)(1U << k);
				/* free bits: the count (byte 1, low nibble) and the index (bytes 8 to 15) */
				want = want && ((b == 1U && k < 4U) || b >= 8U);
				flips++;
			}
			if (mode == 3U) {
				/* the same header on a shorter packet: the size says otherwise */
				cut = test_rng_u32() % 3U;
				/* it may still be a packet: the size counts whole words */
				want = len - cut >= 20U && ((len - cut) % 4U) == 0U && (len - cut) / 4U == size0;
			}
			len -= cut;
		}
		txs_guard(g, sizeof(g), 64, len);
		e = txs_wire_parse(&CFG, 4994, buf, len, &o);
		zassert_equal(e == TXS_WIRE_OK, want, "it %u mode %u len %zu err %d", it, mode, len, e);
		if (e == TXS_WIRE_OK) {
			ok++;
			zassert_true(o.payload >= buf && o.payload + o.bytes <= buf + len);
			zassert_equal(o.bytes, len - 16U);
			zassert_equal(o.words, o.bytes / 2U);
			zassert_true(o.words <= CFG.max_words && o.words > 0U);
		}
		noise += mode == 0U;
	}
	TXS_UNPOISON(g, sizeof(g));
	zassert_true(ok > 10000U && flips > 50000U && noise > 50000U, "ok %u", ok);
}
