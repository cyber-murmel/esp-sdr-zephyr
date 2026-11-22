/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The flip scheduler (txs_flip.h): the operation chosen at each point of the window's time, the
 * point of no return to the cycle, the budget, late and early flips, a flip missed by a whole
 * group, the stop states (NOW, DRAIN) from every state and the hang count.
 */

#include <zephyr/ztest.h>

#include "txs_flip.h"

#define CPW 8U
#define G   TXS_GROUP_WORDS

static const struct txs_flip_cfg CFG = {
	.cost = 4, .zcost = 2, .margin = 1000, .lock_lead = 300, .chunk = 64, .overhead = 50,
	.write_lat = 40, .tol = 3, .hang_passes = 0,
};

static struct txs_flip f;
static struct txs_pos p;

/* The position: the engine `before_due` cycles before the flip at index `next`. */
static void setpos(uint64_t next, int64_t before_due)
{
	txs_pos_init(&p, 1000, CPW, next - G);
	txs_pos_update(&p, 1000 + (uint32_t)((uint64_t)G * CPW - (uint64_t)before_due));
}

/* A scheduler with its window at `first` (words) and the position before_due cycles before it. */
static void setup(uint64_t first, int64_t before_due, const struct txs_flip_cfg *cfg)
{
	zassert_equal(txs_flip_init(&f, cfg ? cfg : &CFG, first), 0);
	setpos(first, before_due);
}

ZTEST_SUITE(txs_flip, NULL, NULL, NULL, NULL, NULL);

ZTEST(txs_flip, test_ops_by_time)
{
	uint64_t lo = 5U * G, hi = lo + G;

	/* far from the flip, window empty: fill */
	setup(lo, (int64_t)G * CPW, NULL);
	zassert_equal(txs_pos_cycles_to(&p, lo), (int64_t)G * CPW);
	zassert_equal(txs_flip_poll(&f, &p, lo), TXS_OP_FILL);
	/* window full: idle until the lock lead, then flip */
	zassert_equal(txs_flip_poll(&f, &p, hi), TXS_OP_IDLE);
	setup(lo, 301, NULL);
	zassert_equal(txs_flip_poll(&f, &p, hi), TXS_OP_IDLE);
	setup(lo, 300, NULL);
	zassert_equal(txs_flip_poll(&f, &p, hi), TXS_OP_FLIP);
	setup(lo, -50, NULL);
	zassert_equal(txs_flip_poll(&f, &p, hi), TXS_OP_FLIP);
	/* the flip also when the window is short: the zeroing had its time before */
	setup(lo, 100, NULL);
	zassert_equal(txs_flip_poll(&f, &p, lo + 10U), TXS_OP_FLIP);
}

/* The point of no return: rest * zcost + margin cycles before the flip, to the cycle. */
ZTEST(txs_flip, test_point_of_no_return)
{
	uint64_t lo = 2U * G;

	for (uint64_t fill = lo; fill < lo + G; fill += 4999) {
		int64_t pnr = (int64_t)((lo + G - fill) * CFG.zcost + CFG.margin);

		setup(lo, pnr + 1, NULL);
		zassert_equal(txs_flip_poll(&f, &p, fill), pnr + 1 > 300 ? TXS_OP_FILL : TXS_OP_FLIP);
		setup(lo, pnr, NULL);
		zassert_equal(txs_flip_poll(&f, &p, fill), pnr > 300 ? TXS_OP_ZERO : TXS_OP_FLIP);
		if (pnr > 301) {
			setup(lo, pnr - 1, NULL);
			zassert_equal(txs_flip_poll(&f, &p, fill), TXS_OP_ZERO);
		}
	}
}

/* The checks of txs_flip_init(): the margin rule and the others. */
ZTEST(txs_flip, test_init_checks)
{
	struct txs_flip_cfg c = CFG;

	zassert_equal(txs_flip_init(&f, &c, 3U * G), 0);
	zassert_equal(txs_flip_init(&f, &c, 3U * G + 1U), -1);
	/* margin = lock_lead + chunk * zcost + overhead exactly: ok; one less: not */
	c.margin = c.lock_lead + c.chunk * c.zcost + c.overhead;
	zassert_equal(txs_flip_init(&f, &c, 3U * G), 0);
	c.margin--;
	zassert_equal(txs_flip_init(&f, &c, 3U * G), -1);
	c = CFG;
	c.chunk = 0;
	zassert_equal(txs_flip_init(&f, &c, 3U * G), -1);
	c = CFG;
	c.cost = c.zcost;
	zassert_equal(txs_flip_init(&f, &c, 3U * G), -1);
	c = CFG;
	c.write_lat = c.lock_lead + 1U;
	zassert_equal(txs_flip_init(&f, &c, 3U * G), -1);
}

/* The range test to the cycle: new words, a gap before them, and a hole filled below the cursor. */
ZTEST(txs_flip, test_may_write)
{
	uint64_t lo = 2U * G, hi = lo + G;

	setup(lo, (int64_t)G * CPW, NULL);
	zassert_true(txs_flip_may_write(&f, &p, lo, lo, lo + G));
	zassert_false(txs_flip_may_write(&f, &p, lo, lo, lo + G + 1U));
	zassert_false(txs_flip_may_write(&f, &p, lo, lo - 1U, lo + 10U));
	/* nothing to write, or nothing in the window: fine, whatever the time */
	setpos(lo, 10);
	zassert_true(txs_flip_may_write(&f, &p, lo + 50U, lo + 50U, lo + 50U));
	zassert_true(txs_flip_may_write(&f, &p, lo + 50U, lo + 70U, lo + 20U));
	zassert_true(txs_flip_may_write(&f, &p, lo, lo - 100U, lo));
	zassert_true(txs_flip_may_write(&f, &p, lo, hi, hi + 5U));
	/* new words: n - fill at cost, the rest of the window at zcost, the margin */
	for (uint64_t n = lo + 1U; n <= hi; n += 3001U) {
		int64_t need = (int64_t)((n - lo) * CFG.cost + (hi - n) * CFG.zcost + CFG.margin);

		setpos(lo, need + 1);
		zassert_true(txs_flip_may_write(&f, &p, lo, lo, n), "n %llu", n);
		setpos(lo, need);
		zassert_false(txs_flip_may_write(&f, &p, lo, lo, n), "n %llu", n);
	}
	/* a gap [fill, lo2) is zeroed (zcost), then lo2..n written */
	{
		uint64_t fill = lo + 100U, lo2 = lo + 600U, n = lo + 900U;
		int64_t need = (int64_t)((n - lo2) * CFG.cost + (lo2 - fill) * CFG.zcost +
					 (hi - n) * CFG.zcost + CFG.margin);

		setpos(lo, need + 1);
		zassert_true(txs_flip_may_write(&f, &p, fill, lo2, n));
		setpos(lo, need);
		zassert_false(txs_flip_may_write(&f, &p, fill, lo2, n));
	}
	/* a hole below the cursor: the words cost, the zeroing of the rest does not shrink */
	{
		uint64_t fill = lo + 5000U, a = lo + 1000U, n = lo + 1256U;
		int64_t need = (int64_t)((n - a) * CFG.cost + (hi - fill) * CFG.zcost + CFG.margin);

		setpos(lo, need + 1);
		zassert_true(txs_flip_may_write(&f, &p, fill, a, n));
		setpos(lo, need);
		zassert_false(txs_flip_may_write(&f, &p, fill, a, n));
		/* a write over the cursor: the part below costs, the part above advances it */
		n = fill + 300U;
		need = (int64_t)((n - a) * CFG.cost + (hi - n) * CFG.zcost + CFG.margin);
		setpos(lo, need + 1);
		zassert_true(txs_flip_may_write(&f, &p, fill, a, n));
		setpos(lo, need);
		zassert_false(txs_flip_may_write(&f, &p, fill, a, n));
	}
	/* not in a stop */
	setpos(lo, (int64_t)G * CPW);
	txs_flip_stop(&f, TXS_STOP_DRAIN);
	zassert_false(txs_flip_may_write(&f, &p, lo, lo, lo + 10U));
	zassert_true(txs_flip_may_write(&f, &p, lo, lo, lo));
}

/* The reviewer's case, scaled: a hole fill of 256 words near the point of no return. */
ZTEST(txs_flip, test_hole_fill_near_pnr)
{
	struct txs_flip_cfg c = CFG;
	uint64_t lo = 2U * G, hi = lo + G, fill = lo + 20000U, a = lo + 3000U;
	int64_t pnr = (int64_t)((hi - fill) * c.zcost + c.margin);

	c.cost = 12;
	c.zcost = 6;
	c.chunk = 256;
	c.margin = c.lock_lead + c.chunk * c.zcost + c.overhead;
	zassert_equal(txs_flip_init(&f, &c, lo), 0);
	pnr = (int64_t)((hi - fill) * c.zcost + c.margin);
	/* 1536 cycles before the point: the cursor-advancing view says 128 words fit, the hole
	 * fill of 256 words (3072 cycles) does not
	 */
	setpos(lo, pnr + 1536);
	zassert_false(txs_flip_may_write(&f, &p, fill, a, a + 256U));
	zassert_true(txs_flip_may_write(&f, &p, fill, a, a + 127U));
	zassert_equal(txs_flip_fit(&f, &p, fill, a, a + 256U), a + 127U);
	/* and what is let through leaves the zeroing its time: the time left after the copy is
	 * at least the point's
	 */
	for (uint64_t e = a; e <= a + 256U; e++) {
		if (txs_flip_may_write(&f, &p, fill, a, e)) {
			zassert_true(pnr + 1536 - (int64_t)((e - a) * c.cost) >= 0);
		}
	}
	/* new words at the cursor cost only cost - zcost against the point: 255 of them fit */
	zassert_equal(txs_flip_fit(&f, &p, fill, fill, fill + 256U), fill + 255U);
}

ZTEST(txs_flip, test_fit)
{
	uint64_t lo = 2U * G, hi = lo + G;

	setup(lo, (int64_t)G * CPW, NULL);
	zassert_equal(txs_flip_fit(&f, &p, lo, lo, lo + 256U), lo + 256U);
	/* nothing beyond the window, nothing in it before the window */
	zassert_equal(txs_flip_fit(&f, &p, hi, hi + 10U, hi + 100U), hi + 100U);
	zassert_equal(txs_flip_fit(&f, &p, lo, lo - 100U, lo - 10U), lo - 10U);
	/* across the start: lo goes to the window's start */
	zassert_equal(txs_flip_fit(&f, &p, lo, lo - 100U, lo + 50U), lo + 50U);
	/* across the end: stops at it */
	setpos(lo, (int64_t)G * CPW);
	zassert_equal(txs_flip_fit(&f, &p, hi - 100U, hi - 100U, hi + 50U), hi);
	/* the largest that fits, found against may_write */
	for (int64_t extra = 0; extra < 3000; extra += 97) {
		uint64_t fill = lo + 1000U, end = fill + 400U, e;
		int64_t base = (int64_t)((hi - fill) * CFG.zcost + CFG.margin);

		setpos(lo, base + extra);
		e = txs_flip_fit(&f, &p, fill, fill, end);
		zassert_true(e >= fill && e <= end);
		zassert_true(e == fill || txs_flip_may_write(&f, &p, fill, fill, e));
		zassert_true(e == end || !txs_flip_may_write(&f, &p, fill, fill, e + 1U));
	}
	/* in a stop nothing fits */
	txs_flip_stop(&f, TXS_STOP_DRAIN);
	zassert_equal(txs_flip_fit(&f, &p, lo, lo, lo + 256U), lo);
}

ZTEST(txs_flip, test_aim_and_tolerance)
{
	uint64_t lo = 3U * G;

	setup(lo, 5000, NULL);
	zassert_equal(txs_flip_aim(&f, &p) , txs_pos_counter_at(&p, lo) - CFG.write_lat);
	/* a write that lands within tol words late: slack, not late; stale all the same */
	setup(lo, 0, NULL);
	txs_flip_done(&f, lo + 3U, lo + G, 5);
	zassert_equal(f.late_flips, 0U);
	zassert_equal(f.slack_flips, 1U);
	zassert_equal(f.stale_words, 3U);
	txs_flip_done(&f, f.next + 4U, f.next + G, 5);
	zassert_equal(f.late_flips, 1U);
	zassert_equal(f.late_worst, 4U);
	zassert_equal(f.stale_words, 7U);
}

/* The ownership to write is the group of the window playing at `play`. */
ZTEST(txs_flip, test_dac_group)
{
	uint64_t lo = 3U * G;

	setup(lo, 0, NULL);
	txs_flip_done(&f, lo, lo + G, 5);
	zassert_equal(f.dac_group, txs_group_of(lo));
	/* late within the group: the same */
	txs_flip_done(&f, f.next + 77U, f.next + G, 5);
	zassert_equal(f.dac_group, txs_group_of(lo + G));
	/* early: the group of the window the flip is for, not the one still playing */
	txs_flip_done(&f, f.next - 3U, f.next + G, 5);
	zassert_equal(f.dac_group, txs_group_of(lo + 2U * G));
	/* missed by one group (an even count of groups moves on by two): the group playing now, which
	 * differs from the old window's
	 */
	setup(lo, 0, NULL);
	zassert_equal(txs_flip_done(&f, lo + G + 5U, lo + G, 1), 2U);
	zassert_equal(f.dac_group, txs_group_of(lo + G));
	zassert_not_equal(f.dac_group, txs_group_of(lo));
	/* the next window to refill is the other group, as it has to be */
	zassert_equal(txs_group_of(f.next), txs_group_of(lo));
	setup(lo, 0, NULL);
	zassert_equal(txs_flip_done(&f, lo + 2U * G + 5U, lo + G, 1), 3U);
	zassert_equal(f.dac_group, txs_group_of(lo + 2U * G));
	zassert_equal(f.dac_group, txs_group_of(lo));
	zassert_not_equal(txs_group_of(f.next), f.dac_group);
}

/* A flip missed by whole groups while draining: the zero window was never played: off. */
ZTEST(txs_flip, test_drain_missed_flip)
{
	uint64_t lo = 3U * G;

	setup(lo, 0, NULL);
	txs_flip_stop(&f, TXS_STOP_DRAIN);
	txs_flip_done(&f, lo, lo + G, 10);
	zassert_equal(f.drain_left, 1U);
	zassert_equal(f.mode, TXS_DRAIN);
	txs_flip_done(&f, f.next + G + 3U, f.next + G, 0);
	zassert_equal(f.mode, TXS_OFF);
	zassert_equal(f.reason, TXS_STOP_MISSED);
	zassert_equal(f.missed_flips, 1U);
	/* in RUN a missed flip goes on */
	setup(lo, 0, NULL);
	txs_flip_done(&f, lo + G + 3U, lo + G, 0);
	zassert_equal(f.mode, TXS_RUN);
}

/* Stops asked for from another core are taken by the poll, NOW over DRAIN, HANG not undone. */
ZTEST(txs_flip, test_stop_request)
{
	uint64_t lo = 3U * G;

	setup(lo, 100000, NULL);
	txs_flip_request(&f, TXS_STOP_DRAIN);
	zassert_equal(f.mode, TXS_RUN, "nothing happens before the poll");
	zassert_equal(txs_flip_poll(&f, &p, lo + 5U), TXS_OP_ZERO);
	zassert_equal(f.mode, TXS_DRAIN);
	zassert_equal(f.drain_left, 2U);
	txs_flip_request(&f, TXS_STOP_NOW);
	txs_flip_request(&f, TXS_STOP_DRAIN);
	zassert_equal(txs_flip_poll(&f, &p, lo + 5U), TXS_OP_OFF);
	zassert_equal(f.reason, TXS_STOP_NOW);
	/* a request after a hang does not change the reason */
	struct txs_flip_cfg c = CFG;

	c.hang_passes = 1;
	setup(lo, 0, &c);
	txs_flip_done(&f, f.next, f.next + G, 0);
	zassert_equal(f.reason, TXS_STOP_HANG);
	txs_flip_request(&f, TXS_STOP_NOW);
	zassert_equal(txs_flip_poll(&f, &p, lo), TXS_OP_OFF);
	zassert_equal(f.reason, TXS_STOP_HANG);
}

ZTEST(txs_flip, test_flip_on_time_moves_window)
{
	uint64_t lo = 3U * G;

	setup(lo, 0, NULL);
	zassert_equal(txs_flip_done(&f, lo, lo + G, 100), 1U);
	zassert_equal(f.next, lo + G);
	zassert_equal(f.flips, 1U);
	zassert_equal(f.late_flips + f.early_flips + f.missed_flips + f.stale_words, 0U);
	zassert_equal(txs_flip_hi(&f), lo + 2U * G);
}

ZTEST(txs_flip, test_late_flip_counts_words)
{
	uint64_t lo = 3U * G;

	/* late by 1, 100, G - 1 words: that many stale, then right again */
	uint32_t late[] = { 1, 100, G - 1U };
	uint64_t stale = 0;

	setup(lo, 0, NULL);
	for (size_t i = 0; i < ARRAY_SIZE(late); i++) {
		uint64_t next = f.next;

		zassert_equal(txs_flip_done(&f, next + late[i], next + G, 5), 1U);
		stale += late[i];
		zassert_equal(f.stale_words, stale);
		zassert_equal(f.next, next + G, "the timeline does not shift");
		zassert_equal(f.late_flips + f.slack_flips, i + 1U);
		zassert_equal(f.late_worst, MAX(late[i], i ? late[i - 1U] : 0U));
	}
	/* then on time again */
	zassert_equal(txs_flip_done(&f, f.next, f.next + G, 5), 1U);
	zassert_equal(f.stale_words, stale);
	zassert_equal(f.late_flips + f.slack_flips, 3U);
	zassert_equal(f.missed_flips, 0U);
}

ZTEST(txs_flip, test_late_flip_over_unfinished_window)
{
	uint64_t lo = 3U * G;

	/* late by 100 and the window written only to lo + 40: 100 late, the rest from lo + 100 on */
	setup(lo, 0, NULL);
	txs_flip_done(&f, lo + 100U, lo + 40U, 1);
	zassert_equal(f.stale_words, 100U + (G - 100U));
	/* written to lo + 500, late by 100: only the late words */
	setup(lo, 0, NULL);
	txs_flip_done(&f, lo + 100U, lo + 500U, 1);
	zassert_equal(f.stale_words, 100U + (G - 500U));
	/* on time, written to hi - 30 only (a missed point of no return): 30 stale */
	setup(lo, 0, NULL);
	txs_flip_done(&f, lo, lo + G - 30U, 1);
	zassert_equal(f.stale_words, 30U);
}

ZTEST(txs_flip, test_early_flip)
{
	uint64_t lo = 3U * G;

	setup(lo, 0, NULL);
	txs_flip_done(&f, lo - 20U, lo + G, 1);
	zassert_equal(f.early_flips, 1U);
	zassert_equal(f.early_worst, 20U);
	zassert_equal(f.stale_words, 20U);
	zassert_equal(f.next, lo + G);
}

/* A flip a whole group late (a hung CPU): the window is the one after the playing group. */
ZTEST(txs_flip, test_flip_missed_by_groups)
{
	uint64_t lo = 3U * G;

	setup(lo, 0, NULL);
	zassert_equal(txs_flip_done(&f, lo + G + 5U, lo + G, 1), 2U);
	zassert_equal(f.missed_flips, 1U);
	zassert_equal(f.next, lo + 2U * G);
	zassert_true(f.next > lo + G + 5U);
	/* the engine replays a group that was never refilled: both windows from next on are stale */
	zassert_equal(f.stale_words, 2U * G);
	setup(lo, 0, NULL);
	zassert_equal(txs_flip_done(&f, lo + 3U * G, lo + G, 1), 4U);
	zassert_equal(f.stale_words, 4U * G);
	zassert_equal(f.missed_flips, 3U);
	zassert_equal(f.next, lo + 4U * G);
}

ZTEST(txs_flip, test_stop_now_from_every_state)
{
	uint64_t lo = 3U * G;
	struct { int64_t before; uint64_t fill; } st[] = {
		{ (int64_t)G * CPW, 3U * G }, { 100000, 3U * G + 5U }, { 2500, 3U * G },
		{ 300, 4U * G }, { -10, 3U * G + 4U }, { 5000, 4U * G },
	};

	for (size_t i = 0; i < ARRAY_SIZE(st); i++) {
		setup(lo, st[i].before, NULL);
		txs_flip_stop(&f, TXS_STOP_NOW);
		zassert_equal(txs_flip_poll(&f, &p, st[i].fill), TXS_OP_OFF, "state %zu", i);
		zassert_equal(f.reason, TXS_STOP_NOW);
		zassert_equal(f.mode, TXS_OFF);
		/* stays off, a later stop changes nothing */
		txs_flip_stop(&f, TXS_STOP_DRAIN);
		zassert_equal(f.reason, TXS_STOP_NOW);
		zassert_equal(txs_flip_poll(&f, &p, st[i].fill), TXS_OP_OFF);
	}
	/* after DRAIN began, NOW still wins */
	setup(lo, 100000, NULL);
	txs_flip_stop(&f, TXS_STOP_DRAIN);
	txs_flip_stop(&f, TXS_STOP_NOW);
	zassert_equal(txs_flip_poll(&f, &p, lo), TXS_OP_OFF);
}

/*
 * DRAIN: zero the rest of the window (even far from the point of no return), flip, zero the whole
 * next window, flip, and the engine goes off shortly before the third flip. Entered from every
 * point of the window's time.
 */
ZTEST(txs_flip, test_drain_sequence)
{
	uint64_t lo = 3U * G;
	int64_t befores[] = { (int64_t)G * CPW, 200000, 5000, 2500, 400, 300, 10 };

	for (size_t i = 0; i < ARRAY_SIZE(befores); i++) {
		uint64_t fill = lo + 77U;

		setup(lo, befores[i], NULL);
		txs_flip_stop(&f, TXS_STOP_DRAIN);
		zassert_equal(f.mode, TXS_DRAIN);
		/* window one */
		if (befores[i] > 300) {
			zassert_equal(txs_flip_poll(&f, &p, fill), TXS_OP_ZERO, "state %zu", i);
			fill = lo + G;
			zassert_equal(txs_flip_poll(&f, &p, fill), TXS_OP_IDLE);
		}
		/* the due time of flip one */
		setpos(lo, 250);
		zassert_equal(txs_flip_poll(&f, &p, fill), TXS_OP_FLIP);
		txs_flip_done(&f, lo, lo + G, 10);
		zassert_equal(f.mode, TXS_DRAIN);
		zassert_equal(f.drain_left, 1U);
		/* window two: all zeros, from its start, at once */
		setpos(lo + G, (int64_t)G * CPW);
		zassert_equal(txs_flip_poll(&f, &p, lo + G), TXS_OP_ZERO);
		zassert_equal(txs_flip_poll(&f, &p, lo + 2U * G), TXS_OP_IDLE);
		setpos(lo + G, 250);
		zassert_equal(txs_flip_poll(&f, &p, lo + 2U * G), TXS_OP_FLIP);
		txs_flip_done(&f, lo + G, lo + 2U * G, 0);
		zassert_equal(f.drain_left, 0U);
		/* window three: not filled, nothing zeroed; off before its flip, not before */
		setpos(lo + 2U * G, 5000);
		zassert_equal(txs_flip_poll(&f, &p, lo + 2U * G), TXS_OP_IDLE);
		setpos(lo + 2U * G, 300);
		zassert_equal(txs_flip_poll(&f, &p, lo + 2U * G), TXS_OP_OFF);
		zassert_equal(f.reason, TXS_STOP_DRAINED);
		zassert_equal(txs_flip_poll(&f, &p, lo + 2U * G), TXS_OP_OFF);
	}
}

ZTEST(txs_flip, test_drain_ignored_when_off_or_repeated)
{
	setup(3U * G, 100000, NULL);
	txs_flip_stop(&f, TXS_STOP_DRAIN);
	f.drain_left = 1;
	txs_flip_stop(&f, TXS_STOP_DRAIN);
	zassert_equal(f.drain_left, 1U, "a second DRAIN does not restart the countdown");
}

ZTEST(txs_flip, test_hang_passes)
{
	struct txs_flip_cfg cfg = CFG;
	uint64_t lo = 3U * G;

	cfg.hang_passes = 3;
	setup(lo, 0, &cfg);
	/* two empty windows, then one with data: the count restarts */
	txs_flip_done(&f, f.next, f.next + G, 0);
	txs_flip_done(&f, f.next, f.next + G, 0);
	zassert_equal(f.mode, TXS_RUN);
	txs_flip_done(&f, f.next, f.next + G, 1);
	zassert_equal(f.zero_passes, 0U);
	zassert_equal(f.mode, TXS_RUN);
	txs_flip_done(&f, f.next, f.next + G, 0);
	txs_flip_done(&f, f.next, f.next + G, 0);
	zassert_equal(f.mode, TXS_RUN);
	txs_flip_done(&f, f.next, f.next + G, 0);
	zassert_equal(f.mode, TXS_OFF);
	zassert_equal(f.reason, TXS_STOP_HANG);
	setup(f.next, 100000, &cfg);
	f.mode = TXS_OFF;
	zassert_equal(txs_flip_poll(&f, &p, f.next), TXS_OP_OFF);
	/* 0: never */
	setup(lo, 0, NULL);
	for (int i = 0; i < 50; i++) {
		txs_flip_done(&f, f.next, f.next + G, 0);
	}
	zassert_equal(f.mode, TXS_RUN);
}

/* A late flip does not break the hang count: it is by windows without data, not by time. */
ZTEST(txs_flip, test_hang_not_in_drain)
{
	struct txs_flip_cfg cfg = CFG;

	cfg.hang_passes = 1;
	setup(3U * G, 0, &cfg);
	txs_flip_stop(&f, TXS_STOP_DRAIN);
	txs_flip_done(&f, f.next, f.next + G, 0);
	zassert_equal(f.mode, TXS_DRAIN);
	zassert_equal(f.reason, TXS_STOP_DRAIN);
}
