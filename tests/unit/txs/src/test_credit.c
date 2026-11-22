/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Credits and the status record (txs_credit.h).
 */

#include <string.h>

#include <zephyr/ztest.h>

#include "txs_credit.h"

#define G TXS_GROUP_WORDS

ZTEST_SUITE(txs_credit, NULL, NULL, NULL, NULL, NULL);

ZTEST(txs_credit, test_limit)
{
	/* the open window's end plus the buffer depth, wherever the cursor is in the window */
	zassert_equal(txs_credit_limit(1000, 5000, 4096, 40000), 44096U);
	zassert_equal(txs_credit_limit(1000, 39000, 4096, 40000), 44096U);
	zassert_equal(txs_credit_limit(1000, 40000, 4096, 40000), 44096U);
	zassert_equal(txs_credit_limit(1000, 5000, 0, 40000), 40000U);
	/* no window published: the contiguous index plus the depth, as before */
	zassert_equal(txs_credit_limit(1000, 5000, 4096, 0), 9096U);
	zassert_equal(txs_credit_limit(1000, 5000, 0, 0), 5000U);
	/* contiguous behind the play index (late flip): from the play index */
	zassert_equal(txs_credit_limit(9000, 5000, 4096, 0), 13096U);
	zassert_equal(txs_credit_limit(9000, 9000, 100, 0), 9100U);
	/* the play index past the window's end (a missed flip): from the play index */
	zassert_equal(txs_credit_limit(50000, 40000, 100, 40000), 50100U);
	/* past 2^32 */
	zassert_equal(txs_credit_limit((1ULL << 32) + 5U, (1ULL << 32) + 70000U, 100, (1ULL << 32) + 98304U),
		      (1ULL << 32) + 98404U);
	/* it does not move while the board writes into the window, and rises by a group with a flip */
	for (uint64_t c = 100000; c < 100000 + 5000; c += 7) {
		zassert_equal(txs_credit_limit(99000, c, 3000, 131072), 134072U);
	}
	zassert_equal(txs_credit_limit(131072, 131072, 3000, 131072 + G), 134072U + G);
}

ZTEST(txs_credit, test_status_record)
{
	struct txs_flip f;
	struct txs_jit j;
	struct txs_status st, back;
	struct txs_flip_cfg cfg = { .cost = 4, .zcost = 2, .margin = 200, .lock_lead = 50, .chunk = 16,
				    .overhead = 20, .write_lat = 10 };
	struct txs_place pl;
	uint8_t raw[TXS_STATUS_BYTES];

	zassert_equal(txs_flip_init(&f, &cfg, 8U * G), 0);
	zassert_equal(txs_jit_init(&j, &f, 4000, 4000), 0);
	txs_jit_packet(&j, 8U * G, 100, &pl);
	txs_jit_packet(&j, 8U * G + 300U, 100, &pl);
	txs_jit_packet(&j, 6U * G, 100, &pl);
	f.stale_words = 77;
	txs_status_make(&st, 0xabcdef01U, 8U * G - 5000U, &j, &f, 4000);
	zassert_equal(st.session, 0xabcdef01U);
	zassert_equal(st.play, 8U * G - 5000U);
	zassert_equal(st.contig, 8U * G + 400U);
	/* the window's end (9 groups) plus the depth, not where the cursor is */
	zassert_equal(st.credit, 9U * G + 4000U);
	zassert_equal(st.restart, 8U * G + 400U);
	zassert_equal(st.underruns, 0U);
	zassert_equal(st.lost, 200U);
	zassert_equal(st.late, 100U);
	zassert_equal(st.stale, 77U);

	/* before the first data word: the window alone (the sender fills it, then the room beyond opens) */
	zassert_equal(txs_jit_init(&j, &f, 4000, 4000), 0);
	txs_status_make(&st, 1U, 8U * G - 5000U, &j, &f, 4000);
	zassert_equal(st.credit, 9U * G);
	zassert_equal(st.contig, 8U * G);
	txs_jit_packet(&j, 8U * G, 100, &pl);
	txs_status_make(&st, 1U, 8U * G - 5000U, &j, &f, 4000);
	zassert_equal(st.credit, 9U * G + 4000U);
	txs_status_make(&st, 0xabcdef01U, 8U * G - 5000U, &j, &f, 4000);
	st.underruns = 3;
	st.play |= 0x1122334400000000ULL;
	st.stale = 0xfedcba9876543210ULL;
	txs_status_put(raw, &st);
	zassert_equal(raw[0], 0xab);
	zassert_equal(raw[3], 0x01);
	zassert_equal(raw[56], 0xfe);
	zassert_equal(raw[63], 0x10);
	zassert_equal(txs_status_get(&back, raw, sizeof(raw)), 0);
	zassert_equal(back.session, st.session);
	zassert_equal(back.play, st.play);
	zassert_equal(back.contig, st.contig);
	zassert_equal(back.credit, st.credit);
	zassert_equal(back.restart, st.restart);
	zassert_equal(back.underruns, st.underruns);
	zassert_equal(back.lost, st.lost);
	zassert_equal(back.late, st.late);
	zassert_equal(back.stale, st.stale);
	zassert_equal(txs_status_get(&back, raw, sizeof(raw) - 1U), -1);
	zassert_equal(txs_status_get(&back, raw, sizeof(raw) + 1U), -1);
}

/* The snapshot for the other core: whole records only. */
ZTEST(txs_credit, test_snapshot)
{
	struct txs_snap s;
	struct txs_status a = { .session = 7, .play = 0x100000005ULL, .contig = 0x200000006ULL,
				.credit = 0x300000007ULL, .restart = 0x400000008ULL, .underruns = 9,
				.lost = 0x500000001ULL, .late = 0x600000002ULL, .stale = 0x700000003ULL };
	struct txs_status b = a, r;

	b.play = 0xffffffffffffffffULL;
	b.stale = 0;
	txs_snap_init(&s);
	zassert_true(txs_snap_read(&s, &r), "the empty record reads (zeros)");
	zassert_equal(r.play, 0U);
	txs_snap_write(&s, &a);
	zassert_true(txs_snap_read(&s, &r));
	zassert_equal(r.play, a.play);
	zassert_equal(r.stale, a.stale);
	/* a write in progress: the words are half old, half new, the reader must refuse */
	txs_snap_begin(&s);
	zassert_false(txs_snap_read(&s, &r));
	txs_snap_put(&s, &b);
	zassert_false(txs_snap_read(&s, &r));
	txs_snap_end(&s);
	zassert_true(txs_snap_read(&s, &r));
	zassert_equal(r.play, b.play);
	zassert_equal(r.stale, b.stale);
	zassert_equal(r.restart, b.restart);
}

/* The record as the 64 bytes of the packet, without a decode. */
ZTEST(txs_credit, test_snapshot_raw)
{
	struct txs_snap s;
	struct txs_status a = { .session = 7, .play = 0x100000005ULL, .contig = 0x200000006ULL,
				.credit = 0x300000007ULL, .restart = 0x400000008ULL, .underruns = 9,
				.lost = 0x500000001ULL, .late = 0x600000002ULL, .stale = 0x700000003ULL };
	uint8_t raw[TXS_STATUS_BYTES], want[TXS_STATUS_BYTES];

	txs_snap_init(&s);
	txs_snap_write(&s, &a);
	txs_status_put(want, &a);
	zassert_true(txs_snap_read_raw(&s, raw));
	zassert_mem_equal(raw, want, sizeof(raw));
	txs_snap_begin(&s);
	zassert_false(txs_snap_read_raw(&s, raw));
	txs_snap_put(&s, &a);
	zassert_false(txs_snap_read_raw(&s, raw));
	txs_snap_end(&s);
	zassert_true(txs_snap_read_raw(&s, raw));
}
