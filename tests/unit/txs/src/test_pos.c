/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The position model (txs_pos.h) against exact integer arithmetic: every cycles per word, the
 * counter wrapping many times, hours of stream time, the index passing 2^32.
 */

#include <zephyr/ztest.h>

#include <test_rng.h>

#include "txs_pos.h"

static const uint32_t cpws[] = { 4, 8, 16, 24 };

ZTEST_SUITE(txs_pos, NULL, NULL, NULL, NULL, NULL);

/* Run `total` cycles in random steps up to `maxstep` and compare with total / cpw every time. */
static void run(uint32_t cpw, uint32_t t0, uint64_t idx0, uint64_t total, uint32_t maxstep)
{
	struct txs_pos p;
	uint64_t t = 0;

	zassert_equal(txs_pos_init(&p, t0, cpw, idx0), 0);
	while (t < total) {
		uint32_t step = 1U + test_rng_u32() % maxstep;
		uint64_t idx;

		if ((test_rng_u32() & 15U) == 0U) {
			step = 1U + test_rng_u32() % 3U;
		}
		t += step;
		idx = txs_pos_update(&p, t0 + (uint32_t)t);
		zassert_equal(idx, idx0 + t / cpw, "cpw %u t %llu: %llu", cpw, t, idx);
		zassert_equal(p.rem, t % cpw);
		/* the start of the next word, and of the one after the next flip */
		zassert_equal(txs_pos_cycles_to(&p, idx + 1U), (int64_t)(cpw - t % cpw));
		zassert_equal(txs_pos_counter_at(&p, idx + 1U), t0 + (uint32_t)(((idx + 1U) - idx0) * cpw));
		zassert_equal(txs_pos_cycles_to(&p, idx), -(int64_t)(t % cpw));
	}
}

ZTEST(txs_pos, test_bad_cpw)
{
	struct txs_pos p;

	for (uint32_t c = 0; c < 40; c++) {
		bool ok = c == 4 || c == 8 || c == 16 || c == 24;

		zassert_equal(txs_pos_init(&p, 0, c, 0) == 0, ok, "cpw %u", c);
	}
}

/* The counter passes 2^32 (a counter value just below it at the start) several times. */
ZTEST(txs_pos, test_counter_wraps)
{
	test_rng_seed(1);
	for (size_t k = 0; k < ARRAY_SIZE(cpws); k++) {
		run(cpws[k], 0xFFFFFF00U, 0, 3ULL << 32, 1U << 30);
		run(cpws[k], 0xFFFFFFFFU, 12345, 2ULL << 32, 1000000U);
		run(cpws[k], 0x80000000U, 7, 1ULL << 31, 5000U);
	}
}

/* A step of nearly 2^32 cycles is still right: the only limit is one read per 2^32 cycles. */
ZTEST(txs_pos, test_longest_step)
{
	for (size_t k = 0; k < ARRAY_SIZE(cpws); k++) {
		struct txs_pos p;
		uint64_t t = 0;

		txs_pos_init(&p, 0xFFFFF000U, cpws[k], 0);
		for (int i = 0; i < 6; i++) {
			t += 0xFFFFFFFFULL;
			zassert_equal(txs_pos_update(&p, 0xFFFFF000U + (uint32_t)t), t / cpws[k]);
		}
	}
}

/* Hours of stream time at 320 MHz, steps of up to 0.1 s. */
ZTEST(txs_pos, test_hours)
{
	test_rng_seed(2);
	for (size_t k = 0; k < ARRAY_SIZE(cpws); k++) {
		run(cpws[k], 0x12345678U, 0, 320000000ULL * 3600 * 6, 32000000U);
	}
}

/* The index passes 2^32 words (and 2^33): the group, the bank slot and the next flip go on. */
ZTEST(txs_pos, test_index_past_2_32)
{
	test_rng_seed(3);
	for (size_t k = 0; k < ARRAY_SIZE(cpws); k++) {
		uint64_t base[] = { (1ULL << 32) - 3U * TXS_GROUP_WORDS - 17U,
				    (1ULL << 33) - 100U, 0xFFFFFFFFFFFF0000ULL - (1ULL << 20) };
		for (size_t b = 0; b < ARRAY_SIZE(base); b++) {
			struct txs_pos p;
			uint64_t idx, prev_group_start;
			uint32_t groups = 0;

			txs_pos_init(&p, 0xFFFF0000U, cpws[k], base[b]);
			idx = base[b];
			prev_group_start = idx >> TXS_GROUP_SHIFT;
			for (uint64_t t = 0; t < (uint64_t)cpws[k] * TXS_GROUP_WORDS * 8U;
			     t += 1U + test_rng_u32() % 20000U) {
				idx = txs_pos_update(&p, 0xFFFF0000U + (uint32_t)t);
				zassert_equal(idx, base[b] + t / cpws[k]);
				zassert_equal(txs_group_of(idx), (uint32_t)(idx >> 15) & 1U);
				zassert_equal(txs_slot_of(idx), (uint32_t)(idx & 0xffffU));
				zassert_equal(txs_next_flip(idx) & (TXS_GROUP_WORDS - 1U), 0U);
				zassert_true(txs_next_flip(idx) > idx &&
					     txs_next_flip(idx) - idx <= TXS_GROUP_WORDS);
				if ((idx >> TXS_GROUP_SHIFT) != prev_group_start) {
					/* the group changes exactly on a multiple of 32768 words */
					zassert_equal(idx >> TXS_GROUP_SHIFT, prev_group_start + 1U);
					groups++;
					prev_group_start = idx >> TXS_GROUP_SHIFT;
				}
			}
			zassert_true(groups >= 6U, "groups %u", groups);
		}
	}
}

/* The time of the next flip is known in advance and exact, whatever the counter does. */
ZTEST(txs_pos, test_next_flip_time)
{
	test_rng_seed(4);
	for (size_t k = 0; k < ARRAY_SIZE(cpws); k++) {
		uint32_t t0 = 0xFFFFFFF0U - (uint32_t)(test_rng_u32() % 1000U);
		struct txs_pos p;
		uint64_t idx = 0;

		txs_pos_init(&p, t0, cpws[k], 3U * TXS_GROUP_WORDS);
		for (uint64_t t = 0; t < (uint64_t)cpws[k] * TXS_GROUP_WORDS * 4U; t += 700) {
			uint64_t nf;
			uint32_t due;

			idx = txs_pos_update(&p, t0 + (uint32_t)t);
			nf = txs_next_flip(idx);
			due = txs_pos_counter_at(&p, nf);
			/* the boundary word starts at word count * cpw cycles from the start */
			zassert_equal(due, t0 + (uint32_t)((nf - 3U * TXS_GROUP_WORDS) * cpws[k]));
			zassert_true(txs_pos_cycles_to(&p, nf) > 0);
			zassert_equal((uint32_t)txs_pos_cycles_to(&p, nf), due - (t0 + (uint32_t)t));
		}
		zassert_true(idx > 3U * TXS_GROUP_WORDS);
	}
}
