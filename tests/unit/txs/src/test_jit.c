/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The jitter cursor (txs_jit.h): in order, a gap and its fill by a reordered packet, duplicates,
 * packets across the window's end, ahead, late and far, the hole table's limit, the underrun
 * close, the restart index, and a random stream of packets (reordered, duplicated, lost) judged
 * by a shadow of the window.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <test_rng.h>

#include "txs_jit.h"

#define G TXS_GROUP_WORDS

static struct txs_flip f;
static struct txs_jit j;

static const struct txs_flip_cfg CFG = {
	.cost = 4, .zcost = 2, .margin = 1000, .lock_lead = 300, .chunk = 64, .overhead = 50,
	.write_lat = 40,
};

static void *setup(void)
{
	return NULL;
}

static void before(void *x)
{
	ARG_UNUSED(x);
	zassert_equal(txs_flip_init(&f, &CFG, 4U * G), 0);
	zassert_equal(txs_jit_init(&j, &f, 2000, 1500), 0);
}

ZTEST_SUITE(txs_jit, NULL, setup, before, NULL, NULL);

#define LO (4U * G)

static struct txs_place put(uint64_t idx, uint32_t n)
{
	struct txs_place pl;

	txs_jit_packet(&j, idx, n, &pl);
	return pl;
}

ZTEST(txs_jit, test_in_order)
{
	for (uint32_t k = 0; k < 10; k++) {
		struct txs_place pl = put(LO + k * 350U, 350);

		zassert_equal(pl.verdict, TXS_PK_PLACE);
		zassert_equal(pl.used, 350U);
		zassert_equal(pl.zero.lo, pl.zero.hi);
		zassert_equal(pl.put.lo, LO + k * 350U);
		zassert_equal(pl.put.hi, LO + (k + 1U) * 350U);
	}
	zassert_equal(j.cursor, LO + 3500U);
	zassert_equal(j.placed, 3500U);
	zassert_equal(j.win_data, 3500U);
	zassert_equal(j.lost + j.dup + j.late + j.nholes, 0U);
	zassert_true(j.started);
}

ZTEST(txs_jit, test_gap_zeroed_then_recovered)
{
	struct txs_place pl;

	put(LO, 100);
	pl = put(LO + 300U, 100);
	zassert_equal(pl.verdict, TXS_PK_PLACE);
	zassert_equal(pl.zero.lo, LO + 100U);
	zassert_equal(pl.zero.hi, LO + 300U);
	zassert_equal(j.lost, 200U);
	zassert_equal(j.nholes, 1U);
	zassert_equal(j.cursor, LO + 400U);
	/* the reordered packet fills the hole while the window is the CPU's */
	pl = put(LO + 100U, 200);
	zassert_equal(pl.verdict, TXS_PK_PLACE);
	zassert_equal(pl.put.lo, LO + 100U);
	zassert_equal(pl.put.hi, LO + 300U);
	zassert_equal(pl.zero.lo, pl.zero.hi);
	zassert_equal(j.recovered, 200U);
	zassert_equal(txs_jit_lost(&j), 0U);
	zassert_equal(j.nholes, 0U);
	zassert_equal(j.cursor, LO + 400U);
	zassert_equal(j.placed, 400U);
	/* the same again is a duplicate */
	pl = put(LO + 100U, 200);
	zassert_equal(pl.verdict, TXS_PK_DUP);
	zassert_equal(j.dup, 200U);
	zassert_equal(j.placed, 400U);
}

ZTEST(txs_jit, test_partial_fill_splits_hole)
{
	struct txs_place pl;

	put(LO, 100);
	put(LO + 1000U, 100);
	/* hole [100, 1000): a packet in its middle leaves two holes */
	pl = put(LO + 400U, 100);
	zassert_equal(pl.verdict, TXS_PK_PLACE);
	zassert_equal(j.nholes, 2U);
	zassert_equal(j.recovered, 100U);
	/* a packet over both holes and the middle: the words in holes count, the rest duplicates */
	pl = put(LO + 50U, 1000);
	zassert_equal(pl.verdict, TXS_PK_PLACE);
	zassert_equal(pl.put.lo, LO + 100U);
	zassert_equal(pl.put.hi, LO + 1000U);
	zassert_equal(j.nholes, 0U);
	zassert_equal(j.recovered, 900U);
	zassert_equal(j.dup, 50U + 100U + 50U /* [50,100) ; [400,500) ; [1000,1050) */);
	zassert_equal(txs_jit_lost(&j), 0U);
}

ZTEST(txs_jit, test_packet_overlapping_cursor)
{
	struct txs_place pl;

	put(LO, 100);
	put(LO + 150U, 100);
	/* [120, 400): fills part of the hole and runs on past the cursor */
	pl = put(LO + 120U, 280);
	zassert_equal(pl.verdict, TXS_PK_PLACE);
	zassert_equal(pl.put.lo, LO + 120U);
	zassert_equal(pl.put.hi, LO + 400U);
	zassert_equal(j.cursor, LO + 400U);
	zassert_equal(j.recovered, 30U);
	zassert_equal(j.dup, 100U);
	zassert_equal(j.placed, 100U + 100U + 30U + 150U);
	zassert_equal(j.nholes, 1U);
	zassert_equal(j.holes[0].lo, LO + 100U);
	zassert_equal(j.holes[0].hi, LO + 120U);
	zassert_equal(txs_jit_lost(&j), 50U - 30U);
}

ZTEST(txs_jit, test_across_window_end_and_wait)
{
	struct txs_place pl;
	uint64_t hi = LO + G;

	pl = put(hi - 100U, 350);
	zassert_equal(pl.verdict, TXS_PK_PLACE);
	zassert_equal(pl.used, 100U);
	zassert_equal(pl.zero.lo, LO);
	zassert_equal(pl.zero.hi, hi - 100U);
	zassert_equal(j.cursor, hi);
	/* the rest and anything past the window waits */
	pl = put(hi, 250);
	zassert_equal(pl.verdict, TXS_PK_WAIT);
	zassert_equal(pl.used, 0U);
	zassert_equal(j.placed, 100U);
	/* after the flip, the window moved on: the waiting packet goes in */
	txs_flip_done(&f, f.next, hi, 100);
	txs_jit_flip(&j, &f);
	zassert_equal(j.lo, hi);
	zassert_equal(j.cursor, hi);
	pl = put(hi, 250);
	zassert_equal(pl.verdict, TXS_PK_PLACE);
	zassert_equal(pl.put.hi, hi + 250U);
	zassert_equal(pl.zero.lo, pl.zero.hi);
}

ZTEST(txs_jit, test_late_far_bad)
{
	struct txs_place pl;

	/* before the window: late */
	pl = put(LO - 500U, 100);
	zassert_equal(pl.verdict, TXS_PK_LATE);
	zassert_equal(j.late, 100U);
	/* across its start: the part before is late, the rest goes in */
	pl = put(LO - 30U, 100);
	zassert_equal(pl.verdict, TXS_PK_PLACE);
	zassert_equal(pl.put.lo, LO);
	zassert_equal(pl.put.hi, LO + 70U);
	zassert_equal(pl.used, 100U);
	zassert_equal(j.late, 130U);
	/* ahead by more than `ahead` words: dropped, within: waits */
	zassert_equal(put(LO + G + 1999U, 10).verdict, TXS_PK_WAIT);
	zassert_equal(put(LO + G + 2000U, 10).verdict, TXS_PK_FAR);
	zassert_equal(j.far, 10U);
	/* empty, and an index that overflows */
	zassert_equal(put(LO, 0).verdict, TXS_PK_BAD);
	zassert_equal(put(UINT64_MAX - 3U, 10).verdict, TXS_PK_BAD);
	zassert_equal(j.cursor, LO + 70U);
}

ZTEST(txs_jit, test_index_past_2_32)
{
	uint64_t first = (1ULL << 32) - G;
	struct txs_place pl;

	zassert_equal(txs_flip_init(&f, &CFG, first), 0);
	zassert_equal(txs_jit_init(&j, &f, 2000, 1500), 0);
	pl = put(first + 10U, 100);
	zassert_equal(pl.verdict, TXS_PK_PLACE);
	zassert_equal(pl.zero.hi, first + 10U);
	txs_flip_done(&f, first, first + 110U, 100);
	txs_jit_flip(&j, &f);
	zassert_equal(j.lo, 1ULL << 32);
	pl = put((1ULL << 32) + 5U, 100);
	zassert_equal(pl.verdict, TXS_PK_PLACE);
	zassert_equal(pl.put.lo, (1ULL << 32) + 5U);
}

ZTEST(txs_jit, test_hole_table_drops_oldest)
{
	uint64_t at = LO;

	/* nine gaps: the oldest is forgotten, its zeros stay */
	for (uint32_t k = 0; k < TXS_HOLES + 2U; k++) {
		put(at, 10);
		at += 20U;
	}
	zassert_equal(j.nholes, TXS_HOLES);
	zassert_equal(j.lost, 10U * (TXS_HOLES + 1U));
	/* the first hole [10, 20) is gone: a packet there is a duplicate (counted as such) */
	zassert_equal(put(LO + 10U, 10).verdict, TXS_PK_DUP);
	/* the second is still there */
	zassert_equal(put(LO + 30U, 10).verdict, TXS_PK_PLACE);
}

ZTEST(txs_jit, test_close_zeroes_rest_underrun)
{
	struct txs_span z;
	struct txs_place pl;

	/* no data yet: not an underrun, the rest is zeroed */
	txs_jit_close(&j, false, &z);
	zassert_equal(z.lo, LO);
	zassert_equal(z.hi, LO + G);
	zassert_equal(j.underruns, 0U);
	zassert_true(j.closed);
	zassert_equal(j.cursor, LO + G);
	/* a second close does nothing */
	txs_jit_close(&j, false, &z);
	zassert_equal(z.lo, z.hi);
	zassert_equal(j.zeroed, G);
	/* a packet for a closed window is late, also over a hole */
	pl = put(LO + 100U, 10);
	zassert_equal(pl.verdict, TXS_PK_LATE);
	zassert_equal(j.late, 10U);
	zassert_equal(j.placed, 0U);
	/* the next window: data, then a short close counts */
	txs_flip_done(&f, f.next, LO + G, 0);
	txs_jit_flip(&j, &f);
	zassert_false(j.closed);
	put(LO + G, 100);
	txs_jit_close(&j, false, &z);
	zassert_equal(z.lo, LO + G + 100U);
	zassert_equal(z.hi, LO + 2U * G);
	zassert_equal(j.underruns, 1U);
	/* a full window is none; a stop is none */
	txs_flip_done(&f, f.next, LO + 2U * G, 100);
	txs_jit_flip(&j, &f);
	put(j.lo, G);
	txs_jit_close(&j, false, &z);
	zassert_equal(z.lo, z.hi);
	zassert_equal(j.underruns, 1U);
	txs_flip_done(&f, f.next, f.next + G, 100);
	txs_jit_flip(&j, &f);
	put(j.lo, 100);
	txs_jit_close(&j, true, &z);
	zassert_equal(j.underruns, 1U);
	zassert_equal(z.hi - z.lo, G - 100U);
}

/* A late packet lost to the close is not recoverable, but the hole it left was already counted. */
ZTEST(txs_jit, test_reorder_after_close_is_late)
{
	struct txs_span z;

	put(LO, 100);
	put(LO + 200U, 100);
	txs_jit_close(&j, false, &z);
	zassert_equal(put(LO + 100U, 100).verdict, TXS_PK_LATE);
	zassert_equal(txs_jit_lost(&j), 100U);
	zassert_equal(j.late, 100U);
}

ZTEST(txs_jit, test_restart_index)
{
	struct txs_span z;

	zassert_equal(txs_jit_restart(&j), LO);
	put(LO, 500);
	zassert_equal(txs_jit_restart(&j), LO + 500U);
	txs_jit_close(&j, false, &z);
	zassert_equal(txs_jit_restart(&j), LO + G);
	/* the flip keeps the cursor: the window starts where the old one ended */
	txs_flip_done(&f, f.next, LO + G, 500);
	txs_jit_flip(&j, &f);
	zassert_equal(txs_jit_restart(&j), LO + G);
	/* a flip over an unfinished window moves the cursor to the new window */
	put(LO + G, 10);
	txs_flip_done(&f, f.next, LO + G + 10U, 10);
	txs_jit_flip(&j, &f);
	zassert_equal(j.cursor, LO + 2U * G);
}

/*
 * A random stream: packets of random size cut from a timeline, shuffled within a reorder
 * distance, some lost, some twice. A shadow window takes what the results say (zero, write): at
 * the end the shadow holds the data or zero everywhere, the counts equal the shadow's, and the
 * words are conserved: delivered = placed + dup + late + far.
 */
ZTEST(txs_jit, test_random_stream)
{
	static uint32_t shadow[G];
	struct pk { uint64_t idx; uint32_t n; };
	static struct pk pk[400];

	for (uint32_t seed = 1; seed <= 200; seed++) {
		uint64_t at = LO, delivered = 0, zeros = 0, data = 0;
		uint32_t np = 0;
		uint32_t reorder = 1U + seed % 12U;

		test_rng_seed(seed * 7919U);
		before(NULL);
		memset(shadow, 0xff, sizeof(shadow));
		while (at < LO + G + 3000U && np < 380U) {
			uint32_t n = 1U + test_rng_u32() % 700U;

			pk[np].idx = at;
			pk[np].n = n;
			np++;
			at += n;
			/* every so often send one twice, drop one */
			if (test_rng_u32() % 11U == 0U) {
				pk[np] = pk[np - 1U];
				np++;
			}
			if (test_rng_u32() % 9U == 0U) {
				np--;
			}
		}
		for (uint32_t k = 0; k + 1U < np; k++) {
			/* a bounded shuffle */
			uint32_t d = test_rng_u32() % reorder;

			if (k + d < np) {
				struct pk t = pk[k];

				pk[k] = pk[k + d];
				pk[k + d] = t;
			}
		}
		for (uint32_t k = 0; k < np; k++) {
			struct txs_place pl;
			uint64_t a = pk[k].idx, e = a + pk[k].n;

			if (a >= LO + G) {
				continue; /* beyond the window: it waits, outside this test */
			}
			e = e > LO + G ? LO + G : e;
			delivered += e - a;
			txs_jit_packet(&j, pk[k].idx, pk[k].n, &pl);
			if (pl.verdict == TXS_PK_PLACE) {
				for (uint64_t i = pl.zero.lo; i < pl.zero.hi; i++) {
					shadow[i - LO] = 0;
				}
				for (uint64_t i = pl.put.lo; i < pl.put.hi; i++) {
					shadow[i - LO] = (uint32_t)i | 1U;
				}
			}
		}
		/* the shadow: words below the cursor are data or zero, above untouched */
		for (uint64_t i = LO; i < LO + G; i++) {
			uint32_t w = shadow[i - LO];

			if (i >= j.cursor) {
				zassert_equal(w, 0xffffffffU);
			} else if (w == 0U) {
				zeros++;
			} else {
				zassert_equal(w, (uint32_t)i | 1U, "seed %u idx %llu", seed, i);
				data++;
			}
		}
		zassert_equal(data, j.placed, "seed %u", seed);
		zassert_equal(j.win_data, (uint32_t)j.placed);
		zassert_equal(zeros, txs_jit_lost(&j), "seed %u", seed);
		zassert_equal(delivered, j.placed + j.dup + j.late + j.far, "seed %u", seed);
	}
}

/* A window handed over before it was full is an underrun too (the sender must resync). */
ZTEST(txs_jit, test_flip_without_close_is_underrun)
{
	put(LO, 100);
	txs_flip_done(&f, f.next, LO + 100U, 100);
	txs_jit_flip(&j, &f);
	zassert_equal(j.underruns, 1U);
	zassert_equal(j.cursor, LO + G);
	/* a full window, and a window before any data: not */
	put(j.lo, G);
	txs_flip_done(&f, f.next, j.lo + G, 100);
	txs_jit_flip(&j, &f);
	zassert_equal(j.underruns, 1U);
}

/* ahead must cover the credit depth. */
ZTEST(txs_jit, test_init_ahead_vs_depth)
{
	zassert_equal(txs_jit_init(&j, &f, 1499, 1500), -1);
	zassert_equal(txs_jit_init(&j, &f, 1500, 1500), 0);
}
