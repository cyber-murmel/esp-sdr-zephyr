/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Transmit stream, jitter cursor and loss handling (doc/dev/txs.md). Packets carry the absolute
 * 64 bit index of their first word; the play position only moves forward and the timeline never
 * shifts. The free window [lo, hi) is the group the CPU owns (txs_flip.h). Pure functions: a
 * packet goes in, the ranges to zero and to write come out, the caller does the copying.
 *
 * - A packet beyond the cursor leaves a gap, which is zeroed as the cursor passes it and is a hole
 *   until a reordered packet fills it (while the window is the CPU's: not after it is closed).
 * - A packet below the cursor that overlaps no hole is a duplicate. Up to TXS_HOLES holes are
 *   kept; a new one drops the oldest (its words stay zero).
 * - A packet for a closed window, or from before it, is late. One for the group after the window
 *   waits (the caller retries after the next flip); one far ahead is dropped. A packet that
 *   crosses the window's end gets used < n whatever the verdict (LATE and DUP too): the caller
 *   keeps the rest for the next window.
 * - At the point of no return the rest of the window is zeroed and closed (an underrun).
 */

#ifndef TXS_JIT_H_
#define TXS_JIT_H_

#include <stdbool.h>
#include <stdint.h>

#include "txs_flip.h"

#define TXS_HOLES 8U

struct txs_span {
	uint64_t lo, hi;
};

enum txs_verdict {
	/* Zero place.zero, write place.put; the packet is dealt with up to place.used. */
	TXS_PK_PLACE,
	/* Not yet: the window it belongs to is not open. Present it again after the flip. */
	TXS_PK_WAIT,
	/* Nothing new in it. */
	TXS_PK_DUP,
	/* Its window is closed or played. */
	TXS_PK_LATE,
	/* Further ahead than the queue may hold: dropped. */
	TXS_PK_FAR,
	/* Empty or index overflow. */
	TXS_PK_BAD,
};

struct txs_place {
	enum txs_verdict verdict;
	/* Words of the packet dealt with: when below n, the rest (idx + used) waits for the next window. */
	uint32_t used;
	/* Gap to zero before the write (lo == hi: none). */
	struct txs_span zero;
	/* Words to write: source word offset put.lo - idx (lo == hi: none). */
	struct txs_span put;
};

struct txs_jit {
	/* The free window and the first index of it neither written nor zeroed. */
	uint64_t lo, hi, cursor;
	bool closed;
	/* A data word was seen: closing a window short is an underrun from then on. */
	bool started;
	/* Words ahead of the window that a packet may announce. */
	uint32_t ahead;
	/* Unique data words placed in the window. */
	uint32_t win_data;
	uint32_t nholes;
	struct txs_span holes[TXS_HOLES];
	/* Counts, in words unless noted. placed counts a word once, a recovered one too. */
	uint64_t placed, lost, recovered, dup, late, far, zeroed;
	/* Windows closed or handed over short after the first data word, and the words they lacked. */
	uint32_t underruns;
	uint64_t unfilled;
};

/**
 * Start at the window of `f`; packets starting up to `ahead` words beyond it wait, further ones
 * are dropped as FAR. `ahead` must be at least the credit depth (txs_credit.h): a sender that
 * keeps to its credit never starts a packet past hi + depth, so a window held back by a lost
 * tail packet cannot make the queued packets beyond it FAR.
 * @retval 0, or -1 if ahead < depth.
 */
int txs_jit_init(struct txs_jit *j, const struct txs_flip *f, uint32_t ahead, uint32_t depth);

/**
 * Move to the window of `f` after txs_flip_done(). A window left without close and short of its
 * end after data was seen counts as an underrun (the sender resyncs on that count).
 */
void txs_jit_flip(struct txs_jit *j, const struct txs_flip *f);

/** Deal with a packet of n words at index idx. */
void txs_jit_packet(struct txs_jit *j, uint64_t idx, uint32_t n, struct txs_place *out);

/**
 * Close the window: the words [cursor, hi) are to be zeroed, returned in *zero. `drain` for a
 * stop (not an underrun).
 */
void txs_jit_close(struct txs_jit *j, bool drain, struct txs_span *zero);

/** Words lost: zeroed for a gap and not recovered. */
static inline uint64_t txs_jit_lost(const struct txs_jit *j)
{
	return j->lost - j->recovered;
}

/** The index a sender that lost its place restarts at: the first one that will be written. */
static inline uint64_t txs_jit_restart(const struct txs_jit *j)
{
	return j->closed ? j->hi : j->cursor;
}

#endif /* TXS_JIT_H_ */
