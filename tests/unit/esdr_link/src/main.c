/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The framed link's codec. The vectors come from its definition (COBS by the rules of the paper,
 * zlib's CRC-32), not from this code; tests/host/test_esdr_link.py holds the Python twin to the
 * same ones.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <esp_sdr/esdr_link.h>

static uint8_t wire[ESDR_LINK_ENCODED_MAX(5000)];
static uint8_t data[5000];

/* Encodes and decodes @p len bytes of data; checks what comes back and the size bound. */
static void round_trip(size_t len, uint8_t type, uint16_t seq)
{
	struct esdr_link_frame f;
	size_t n = esdr_link_encode(wire, sizeof(wire), type, seq, data, len, NULL, 0);

	zassert_not_equal(n, 0, "length %zu did not fit", len);
	zassert_true(n <= ESDR_LINK_ENCODED_MAX(len), "length %zu took %zu", len, n);
	zassert_equal(wire[n - 1], 0, "no ending zero");
	zassert_is_null(memchr(wire, 0, n - 1), "a zero inside a frame of %zu", len);
	zassert_equal(esdr_link_decode(wire, n - 1, &f), 0, "length %zu", len);
	zassert_equal(f.type, type);
	zassert_equal(f.flags, 0);
	zassert_equal(f.seq, seq);
	zassert_equal(f.len, len, "length %zu came back as %zu", len, f.len);
	zassert_mem_equal(f.payload, data, len, "length %zu", len);
}

ZTEST(esdr_link, test_crc_check_value)
{
	const uint8_t s[] = "123456789";

	zassert_equal(~esdr_link_crc_add(0xFFFFFFFFU, s, 9), 0xCBF43926U, "CRC-32 of 123456789");
}

ZTEST(esdr_link, test_vector_empty)
{
	static const uint8_t want[] = {0x02, 0x01, 0x01, 0x01, 0x05, 0x79, 0xb8, 0xf8, 0x99, 0x00};
	size_t n = esdr_link_encode(wire, sizeof(wire), ESDR_LINK_HELLO, 0, NULL, 0, NULL, 0);

	zassert_equal(n, sizeof(want));
	zassert_mem_equal(wire, want, n);
}

ZTEST(esdr_link, test_vector_with_zeros)
{
	static const uint8_t payload[] = {0x00, 0x01, 0x00, 0x00, 0xff};
	static const uint8_t want[] = {0x02, 0x02, 0x03, 0x34, 0x12, 0x02, 0x01, 0x01,
				       0x06, 0xff, 0x11, 0x96, 0x73, 0x4f, 0x00};
	size_t n = esdr_link_encode(wire, sizeof(wire), ESDR_LINK_REQUEST, 0x1234, payload,
				    sizeof(payload), NULL, 0);

	zassert_equal(n, sizeof(want));
	zassert_mem_equal(wire, want, n);
}

ZTEST(esdr_link, test_vector_zeros_only)
{
	static const uint8_t payload[10] = {0};
	static const uint8_t want[] = {0x02, 0x06, 0x02, 0x07, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
				       0x01, 0x01, 0x01, 0x01, 0x05, 0xfe, 0xdb, 0xe1, 0x5d, 0x00};
	size_t n = esdr_link_encode(wire, sizeof(wire), ESDR_LINK_CREDIT, 7, payload,
				    sizeof(payload), NULL, 0);

	zassert_equal(n, sizeof(want));
	zassert_mem_equal(wire, want, n);
}

/* Past a COBS block's 254 bytes. */
ZTEST(esdr_link, test_vector_long)
{
	size_t n;

	for (size_t k = 0; k < 249; k++) {
		data[k] = (uint8_t)(k + 1U);
	}
	n = esdr_link_encode(wire, sizeof(wire), ESDR_LINK_BLOCK, 0xFFFF, data, 249, NULL, 0);
	zassert_equal(n, 260);
	zassert_equal(~esdr_link_crc_add(0xFFFFFFFFU, wire, n), 0xF2C42674U, "the frame's bytes");
}

ZTEST(esdr_link, test_round_trip_lengths)
{
	static const uint8_t fills[] = {0x00, 0xFF, 0x55};

	ARRAY_FOR_EACH(fills, k) {
		memset(data, fills[k], sizeof(data));
		for (size_t len = 0; len < 600; len++) {
			round_trip(len, ESDR_LINK_BLOCK, (uint16_t)len);
		}
		round_trip(4080, ESDR_LINK_BLOCK, 1);
		round_trip(4096, ESDR_LINK_BLOCK, 2);
	}
}

/* Around the block boundaries, where the code byte lands next to the end. */
ZTEST(esdr_link, test_round_trip_counter)
{
	for (size_t k = 0; k < sizeof(data); k++) {
		data[k] = (uint8_t)(k % 255U + 1U);
	}
	for (size_t len = 240; len < 270; len++) {
		round_trip(len, ESDR_LINK_BLOCK, 3);
	}
	for (size_t len = 490; len < 520; len++) {
		round_trip(len, ESDR_LINK_BLOCK, 4);
	}
	round_trip(sizeof(data), ESDR_LINK_BLOCK, 5);
}

ZTEST(esdr_link, test_two_segments_are_one_payload)
{
	static const uint8_t a[] = {1, 0, 2};
	static const uint8_t b[] = {3, 0};
	static const uint8_t whole[] = {1, 0, 2, 3, 0};
	static uint8_t other[64];
	size_t n = esdr_link_encode(wire, sizeof(wire), ESDR_LINK_REPLY, 9, a, sizeof(a), b,
				    sizeof(b));
	size_t m = esdr_link_encode(other, sizeof(other), ESDR_LINK_REPLY, 9, whole,
				    sizeof(whole), NULL, 0);

	zassert_equal(n, m);
	zassert_mem_equal(wire, other, n);
}

ZTEST(esdr_link, test_too_small_a_buffer)
{
	static const uint8_t payload[32] = {1};
	size_t full = esdr_link_encode(wire, sizeof(wire), ESDR_LINK_BLOCK, 0, payload,
				       sizeof(payload), NULL, 0);

	zassert_not_equal(full, 0);
	zassert_equal(esdr_link_encode(wire, full - 1, ESDR_LINK_BLOCK, 0, payload,
				       sizeof(payload), NULL, 0), 0, "one byte short");
	zassert_equal(esdr_link_encode(wire, full, ESDR_LINK_BLOCK, 0, payload, sizeof(payload),
				       NULL, 0), full, "just enough");
	zassert_equal(esdr_link_encode(wire, 1, ESDR_LINK_BLOCK, 0, NULL, 0, NULL, 0), 0);
	zassert_equal(esdr_link_encode(wire, 0, ESDR_LINK_BLOCK, 0, NULL, 0, NULL, 0), 0);
}

/* A wrong byte anywhere is noticed: in the data, the header or the CRC. */
ZTEST(esdr_link, test_every_flipped_byte_fails)
{
	static uint8_t copy[128];
	struct esdr_link_frame f;
	size_t n;

	for (size_t k = 0; k < 40; k++) {
		data[k] = (uint8_t)(k * 7U);
	}
	n = esdr_link_encode(wire, sizeof(wire), ESDR_LINK_BLOCK, 3, data, 40, NULL, 0);
	for (size_t k = 0; k + 1 < n; k++) {
		memcpy(copy, wire, n);
		copy[k] ^= 0x10U;
		if (copy[k] == 0U) {
			continue;
		}
		zassert_equal(esdr_link_decode(copy, n - 1, &f), -1, "byte %zu", k);
	}
}

/* Bytes lost in the middle cost that frame; the zero finds the next. */
ZTEST(esdr_link, test_a_cut_frame_costs_one_frame)
{
	static uint8_t stream[1024];
	static const uint8_t payload[] = {10, 20, 30, 0, 40};
	struct esdr_link_frame f;
	size_t a = esdr_link_encode(stream, sizeof(stream), ESDR_LINK_BLOCK, 1, payload,
				    sizeof(payload), NULL, 0);
	size_t b = esdr_link_encode(stream + a, sizeof(stream) - a, ESDR_LINK_BLOCK, 2, payload,
				    sizeof(payload), NULL, 0);

	zassert_not_equal(b, 0);
	zassert_equal(esdr_link_decode(stream, a - 4, &f), -1, "the cut one");
	zassert_equal(esdr_link_decode(stream + a, b - 1, &f), 0, "the next");
	zassert_equal(f.seq, 2);
}

ZTEST(esdr_link, test_rubbish_does_not_decode)
{
	static uint8_t junk[] = {0xFF, 1, 2};
	static uint8_t short_one[] = {0x02, 0x01};
	struct esdr_link_frame f;

	zassert_equal(esdr_link_decode(junk, sizeof(junk), &f), -1, "a code past the end");
	zassert_equal(esdr_link_decode(short_one, sizeof(short_one), &f), -1, "too short");
	zassert_equal(esdr_link_decode(junk, 0, &f), -1, "nothing");
}

ZTEST_SUITE(esdr_link, NULL, NULL, NULL, NULL, NULL);
