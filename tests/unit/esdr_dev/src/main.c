/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit test of the osmosdr protocol helpers: esdr_dev.h's request checks and GET_INFO reply,
 * with the rate tables of the S3 (ring rate over 32 to 1024) and of osmosdr_serial over SDIO
 * (2 MS/s and seven halvings), a rate list (osmosdr_hs's), and esdr_proto.h's block header check.
 */
#include <string.h>

#include <zephyr/ztest.h>

#include "esdr_dev.h"

ZTEST_SUITE(esdr_dev, NULL, NULL, NULL, NULL, NULL);

ZTEST(esdr_dev, test_rate_round)
{
	/* Each rate wins from the geometric mean with the one below it up. */
	zassert_equal(esdr_rate_round(500000U, 6U, 1000000000U), 500000U);
	zassert_equal(esdr_rate_round(500000U, 6U, 500000U), 500000U);
	zassert_equal(esdr_rate_round(500000U, 6U, 353554U), 500000U);
	zassert_equal(esdr_rate_round(500000U, 6U, 353553U), 250000U);
	zassert_equal(esdr_rate_round(500000U, 6U, 250000U), 250000U);
	zassert_equal(esdr_rate_round(500000U, 6U, 22097U), 15625U);
	zassert_equal(esdr_rate_round(500000U, 6U, 1U), 15625U);
	zassert_equal(esdr_rate_round(500000U, 6U, 0U), 15625U);
	zassert_equal(esdr_rate_round(2000000U, 8U, 2048000U), 2000000U);
	zassert_equal(esdr_rate_round(2000000U, 8U, 1000000U), 1000000U);
	zassert_equal(esdr_rate_round(2000000U, 8U, 100000U), 125000U);
	zassert_equal(esdr_rate_round(2000000U, 8U, 10000U), 15625U);
	zassert_equal(esdr_rate_round(250000U, 1U, 1000U), 250000U);
	/* 2 hz^2 passes 2^64 from 3.04 GHz up. */
	zassert_equal(esdr_rate_round(500000U, 6U, 3100000000U), 500000U);
	zassert_equal(esdr_rate_round(2000000U, 8U, UINT32_MAX), 2000000U);
	/* Every offered rate rounds to itself. */
	for (uint32_t k = 0; k < 8U; k++) {
		zassert_equal(esdr_rate_round(2000000U, 8U, 2000000U >> k), 2000000U >> k);
	}
}

ZTEST(esdr_dev, test_rate_round_list)
{
	/* osmosdr_hs: 160 MHz over even dividers, 20 MS/s to 2. */
	static const uint32_t r[] = {20000000U, 16000000U, 10000000U, 8000000U, 5000000U,
				     4000000U, 3200000U, 2500000U, 2000000U};
	const size_t n = ARRAY_SIZE(r);

	zassert_equal(esdr_rate_round_list(r, n, UINT32_MAX), 20000000U);
	zassert_equal(esdr_rate_round_list(r, n, 40000000U), 20000000U);
	/* The geometric mean of 20 and 16 MS/s is 17.889 MS/s. */
	zassert_equal(esdr_rate_round_list(r, n, 17888543U), 16000000U);
	zassert_equal(esdr_rate_round_list(r, n, 17888544U), 20000000U);
	zassert_equal(esdr_rate_round_list(r, n, 12000000U), 10000000U);
	zassert_equal(esdr_rate_round_list(r, n, 2048000U), 2000000U);
	zassert_equal(esdr_rate_round_list(r, n, 1U), 2000000U);
	zassert_equal(esdr_rate_round_list(r, n, 0U), 2000000U);
	/* Every listed rate rounds to itself, so does every max >> k an old host sends. */
	for (size_t k = 0; k < n; k++) {
		zassert_equal(esdr_rate_round_list(r, n, r[k]), r[k]);
	}
	for (uint32_t k = 0; k < 3U; k++) {
		zassert_equal(esdr_rate_round_list(r, n, 20000000U >> k), 20000000U >> k);
	}
	/* A ladder of halvings rounds as esdr_rate_round() does. */
	static const uint32_t h[] = {2000000U, 1000000U, 500000U, 250000U, 125000U, 62500U, 31250U,
				     15625U};

	for (uint32_t hz = 1000U; hz < 4000000U; hz = hz * 21U / 20U + 1U) {
		zassert_equal(esdr_rate_round_list(h, ARRAY_SIZE(h), hz),
			      esdr_rate_round(2000000U, 8U, hz), "%u", hz);
	}
}

ZTEST(esdr_dev, test_freq_dgain)
{
	zassert_true(esdr_freq_ok(2450000000ULL));
	zassert_true(esdr_freq_ok((uint64_t)ESP_SDR_FREQ_MIN_MHZ * 1000000U));
	zassert_true(esdr_freq_ok((uint64_t)ESP_SDR_FREQ_MAX_MHZ * 1000000U));
	zassert_false(esdr_freq_ok((uint64_t)ESP_SDR_FREQ_MIN_MHZ * 1000000U - 1U));
	zassert_false(esdr_freq_ok((uint64_t)ESP_SDR_FREQ_MAX_MHZ * 1000000U + 1U));
	zassert_false(esdr_freq_ok(0U));

	zassert_true(esdr_dgain_ok(ESDR_DGAIN_DEFAULT));
	for (uint16_t db = 0; db <= 48U; db++) {
		zassert_equal(esdr_dgain_ok(db), db % 6U == 0U && db <= ESDR_DGAIN_MAX_DB, "%u", db);
	}
}

ZTEST(esdr_dev, test_info_init)
{
	struct esdr_info info;

	memset(&info, 0xa5, sizeof(info));
	esdr_info_init(&info, 2000000U, 8U, 61U, 15U, "a firmware name longer than the field");
	zassert_equal(info.magic, ESDR_INFO_MAGIC);
	zassert_equal(info.proto_version, ESDR_PROTO_VERSION);
	zassert_equal(info.info_size, sizeof(info));
	zassert_equal(info.freq_min_hz, (uint64_t)ESP_SDR_FREQ_MIN_MHZ * 1000000U);
	zassert_equal(info.freq_max_hz, (uint64_t)ESP_SDR_FREQ_MAX_MHZ * 1000000U);
	zassert_equal(info.rx_rate_max_hz, 2000000U);
	zassert_equal(info.rx_rate_count, 8U);
	zassert_equal(info.rx_gain_max, 61U);
	zassert_equal(info.tx_gain_max, 15U);
	zassert_equal(info.block_bytes, ESDR_BLOCK_BYTES);
	zassert_equal(info.full_scale16, 32767U);
	/* No capability unless the device sets it. */
	zassert_equal(info.caps, 0U);
	/* Cut to the field, always terminated. */
	zassert_equal(strlen(info.firmware), sizeof(info.firmware) - 1U);
	zassert_mem_equal(info.firmware, "a firmware name longer than the", sizeof(info.firmware) - 1U);
}

static void put_hdr(uint8_t *b, uint32_t magic, uint16_t flags, uint16_t samples)
{
	struct esdr_block_hdr h = {.magic = magic, .flags = flags, .samples = samples};

	memcpy(b, &h, sizeof(h));
}

ZTEST(esdr_dev, test_block_hdr_ok)
{
	/* Odd offset: the check must not need an aligned header. */
	static uint8_t buf[1 + sizeof(struct esdr_block_hdr)];
	uint8_t *b = &buf[1];
	const uint16_t pairs8 = ESDR_BLOCK_PAYLOAD / 2U, items4 = ESDR_BLOCK_PAYLOAD;

	put_hdr(b, ESDR_BLOCK_MAGIC, ESDR_BLK_GAP, pairs8);
	zassert_true(esdr_block_hdr_ok(b, ESDR_BLK_KNOWN));
	put_hdr(b, ESDR_BLOCK_MAGIC ^ 1U, ESDR_BLK_GAP, pairs8);
	zassert_false(esdr_block_hdr_ok(b, ESDR_BLK_KNOWN));
	/* Samples beyond the payload at the item width. */
	put_hdr(b, ESDR_BLOCK_MAGIC, 0U, pairs8 + 1U);
	zassert_false(esdr_block_hdr_ok(b, ESDR_BLK_KNOWN));
	put_hdr(b, ESDR_BLOCK_MAGIC, ESDR_BLK_4BIT, items4);
	zassert_true(esdr_block_hdr_ok(b, ESDR_BLK_KNOWN));
	put_hdr(b, ESDR_BLOCK_MAGIC, ESDR_BLK_4BIT, items4 + 1U);
	zassert_false(esdr_block_hdr_ok(b, ESDR_BLK_KNOWN));
	put_hdr(b, ESDR_BLOCK_MAGIC, ESDR_BLK_16BIT, ESDR_BLOCK_PAYLOAD / 4U);
	zassert_true(esdr_block_hdr_ok(b, ESDR_BLK_KNOWN));
	put_hdr(b, ESDR_BLOCK_MAGIC, ESDR_BLK_16BIT, ESDR_BLOCK_PAYLOAD / 4U + 1U);
	zassert_false(esdr_block_hdr_ok(b, ESDR_BLK_KNOWN));
	/* Flags outside the accepted set, known or not. */
	put_hdr(b, ESDR_BLOCK_MAGIC, ESDR_BLK_SEG, 0U);
	zassert_true(esdr_block_hdr_ok(b, ESDR_BLK_KNOWN));
	zassert_false(esdr_block_hdr_ok(b, ESDR_BLK_KNOWN & ~ESDR_BLK_SEG));
	put_hdr(b, ESDR_BLOCK_MAGIC, (uint16_t)(ESDR_BLK_KNOWN + 1U), 0U);
	zassert_false(esdr_block_hdr_ok(b, ESDR_BLK_KNOWN));
	put_hdr(b, ESDR_BLOCK_MAGIC, 0x8000U, 0U);
	zassert_false(esdr_block_hdr_ok(b, ESDR_BLK_KNOWN));
}
