/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Transmit stream, flip scheduler (doc/dev/txs.md): the DAC engine plays two groups of 32768
 * words in turn. While it plays one, the CPU owns the other and refills it, the window
 * [next, next + 32768) of word indices; at index `next` the ownership flips and the window moves
 * on by a group. Pure functions on a state struct: the caller does the writes, the ownership
 * change and the waiting. No Zephyr calls: runs on native_sim.
 *
 * Time is the position model's (txs_pos.h). A flip is due when the engine reaches index `next`.
 * The refill has a point of no return: when the time left to `next` falls below the cost of
 * zeroing the rest of the window plus a margin, the rest is zeroed and the flip is made on time,
 * so the timeline never shifts. A flip that comes late by L words plays L words of the old
 * content (stale) and is right again afterwards: no error accumulates.
 */

#ifndef TXS_FLIP_H_
#define TXS_FLIP_H_

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "txs_pos.h"

enum txs_mode {
	TXS_RUN,
	/* Stop after the window's rest and one full zero window were played. */
	TXS_DRAIN,
	/* The engine is to be switched off (or is). */
	TXS_OFF,
};

enum txs_stop {
	TXS_STOP_NONE,
	TXS_STOP_DRAIN,
	TXS_STOP_NOW,
	/* hang_passes windows in a row without a data word. */
	TXS_STOP_HANG,
	/* DRAIN ran through. */
	TXS_STOP_DRAINED,
	/* A flip was missed by a whole group while draining: the zero window was never played. */
	TXS_STOP_MISSED,
};

/** What the caller does next (txs_flip_poll()). */
enum txs_op {
	/* Fill words of the window from the stream, the part txs_flip_fit() allows. */
	TXS_OP_FILL,
	/* Zero [fill, txs_flip_hi()) and close the window (txs_jit_close()). */
	TXS_OP_ZERO,
	/* The window is full: nothing to do before the flip. */
	TXS_OP_IDLE,
	/* Within lock_lead of the flip: lock interrupts, spin to the due time, write the ownership. */
	TXS_OP_FLIP,
	/* Engine and chain off. */
	TXS_OP_OFF,
};

struct txs_flip_cfg {
	/* Cycles a word costs to fill and to zero, worst case; cost must exceed zcost. */
	uint32_t cost, zcost;
	/* Cycles kept free before the flip after the zeroing. Rule, checked by txs_flip_init():
	 * margin >= lock_lead + chunk * zcost + overhead.
	 */
	uint32_t margin;
	/* Cycles before the due time from which the caller locks and spins (worst chunk + 2 us). */
	uint32_t lock_lead;
	/* Most words a fill or zero step handles, and the cycles a step costs besides its words. */
	uint32_t chunk, overhead;
	/* Cycles the ownership write takes at least: the caller starts it that long before the due
	 * time (txs_flip_aim()); it must not exceed lock_lead.
	 */
	uint32_t write_lat;
	/* Words the write may land late (spin exit, a slower write) before the flip counts as late
	 * (late_flips; below it slack_flips): the words it plays stale are in stale_words either way.
	 */
	uint32_t tol;
	/* Windows in a row without data after which the chain goes off, 0 for never. */
	uint32_t hang_passes;
};

/*
 * Threading: every function takes the state of the CPU1 thread and is called from that thread
 * only, with the position model of the core whose counter it reads. Another core asks for a
 * stop with txs_flip_request() (an atomic word) and reads the status through a txs_snap
 * (txs_credit.h); it never touches the other fields.
 */
struct txs_flip {
	struct txs_flip_cfg cfg;
	enum txs_mode mode;
	enum txs_stop reason;
	/* A stop asked for by another core (enum txs_stop), taken by txs_flip_poll(). */
	_Atomic uint32_t req;
	/* Index of the next flip: the free window is [next, next + TXS_GROUP_WORDS). */
	uint64_t next;
	/* The group the DAC owns after the last txs_flip_done(): the ownership to write. */
	uint32_t dac_group;
	/* DRAIN: flips still to make before the engine goes off. */
	uint8_t drain_left;
	uint32_t zero_passes;
	/* Counts. late_flips: beyond the tolerance; slack_flips: late within it. */
	uint64_t flips, late_flips, slack_flips, early_flips, missed_flips;
	/* Words played stale: late or early flip, a window handed over unfinished, a missed flip. */
	uint64_t stale_words;
	/* Worst lateness in words, and worst earliness. */
	uint64_t late_worst, early_worst;
};

/**
 * Start with the window at first (a multiple of the group size), the engine playing before it.
 * @retval 0, or -1 for a first that is no multiple of the group, cost <= zcost, chunk 0,
 *         write_lat > lock_lead, or a margin below lock_lead + chunk * zcost + overhead.
 */
int txs_flip_init(struct txs_flip *f, const struct txs_flip_cfg *cfg, uint64_t first);

/** @return the index one past the free window. */
static inline uint64_t txs_flip_hi(const struct txs_flip *f)
{
	return f->next + TXS_GROUP_WORDS;
}

/**
 * What to do now. `fill` is the first index of the window not yet written or zeroed (txs_jit's
 * cursor, at most txs_flip_hi()). Switches to TXS_OFF for NOW, a ran through DRAIN and a hang.
 */
enum txs_op txs_flip_poll(struct txs_flip *f, const struct txs_pos *p, uint64_t fill);

/**
 * Whether the words [lo, n) may be written into the window now and in time. The fill cursor is
 * `fill`. Words at or past the cursor advance it and save their zeroing; words below it (a
 * reordered packet filling a hole) cost the same and save nothing; a gap between the cursor and
 * lo is zeroed. The time to the due time must cover all that, the zeroing of the rest and the
 * margin. An empty range, or one that touches nothing of the window, is always fine; a write
 * outside the window or while stopping is not.
 */
bool txs_flip_may_write(const struct txs_flip *f, const struct txs_pos *p, uint64_t fill,
			uint64_t lo, uint64_t n);

/**
 * The end of the largest write [lo, e) with e at most `end` that txs_flip_may_write() allows
 * (lo is taken to the window's start, and e stops at its end). Returns `end` when the range does
 * not touch the window, and the clipped lo when nothing fits: the caller then waits.
 */
uint64_t txs_flip_fit(const struct txs_flip *f, const struct txs_pos *p, uint64_t fill,
		      uint64_t lo, uint64_t end);

/** The counter value at which the ownership write is to start: the due time less write_lat. */
uint32_t txs_flip_aim(const struct txs_flip *f, const struct txs_pos *p);

/**
 * The flip was made: `play` is the index of the first word that reads the new ownership
 * (txs_pos_word_after() after a txs_pos_update() at the write, not at the poll). `fill` is where
 * the window stood (words from there to the end played stale), `data` the words of the window
 * that carry stream data. Accounts a late or early flip, moves the window and counts a DRAIN or
 * hang step. The ownership to write is f->dac_group (the group of the window playing at `play`;
 * not the group of the old `next` when a flip was missed by groups).
 *
 * A flip missed by whole groups plays the old content of groups that were never refilled: every
 * word from the old `next` to the end of the window playing at `play`, groups * 32768 words, all
 * stale. During DRAIN the zero window was never played, so the engine goes off (TXS_STOP_MISSED).
 * @return groups the window moved on by: 1, more after a flip missed by a whole group.
 */
uint32_t txs_flip_done(struct txs_flip *f, uint64_t play, uint64_t fill, uint32_t data);

/** Stop (CPU1 thread): DRAIN zeroes everything from now on and ends two flips later, NOW at the next poll. */
void txs_flip_stop(struct txs_flip *f, enum txs_stop how);

/** Ask for a stop from any core (atomic); the CPU1 thread takes it in txs_flip_poll(). NOW wins over DRAIN. */
void txs_flip_request(struct txs_flip *f, enum txs_stop how);

#endif /* TXS_FLIP_H_ */
