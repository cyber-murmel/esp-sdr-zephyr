/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Transmit stream, credits and the status record (doc/dev/txs.md). The board does not recover the
 * sender's clock: it tells the sender how far it may go. The sender paces itself to the sample
 * rate and never passes the credit limit, so a stalled board back-pressures the sample source
 * instead of overrunning the buffers. Plain C, no Zephyr calls.
 */

#ifndef TXS_CREDIT_H_
#define TXS_CREDIT_H_

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "txs_jit.h"

/**
 * The index the sender may reach: `depth` words past the end of the open window (win_end, the
 * group being filled while the other plays), or past the contiguous index or the play index when
 * those lie beyond it (a late flip). Words inside the open window are written the moment they
 * arrive, so the board holds in buffers only what lies beyond the window's end, and a sender that
 * stops `depth` words past it never exceeds a buffer capacity of `depth` words. The limit does not
 * depend on how far the window is filled, so it does not fall behind by a round trip: it rises by
 * one group at each flip and the sender knows it one status delay later. win_end 0 (no window
 * published yet) falls back to the contiguous index: contig + depth. (txs_status_make() passes depth 0
 * until the first data word: before it the sender is held to the first window.)
 *
 * One rule for depth and period: depth = the buffers' capacity in words of the session's smallest
 * packet, less two packets (one split by a window's end, one in progress). The status goes at
 * every flip and at least every 250 us (every depth / rate / 4 would do for the fallback).
 * (12 buffers of 702 words: depth 7020.)
 */
static inline uint64_t txs_credit_limit(uint64_t play, uint64_t contig, uint32_t depth,
					uint64_t win_end)
{
	uint64_t base = contig > play ? contig : play;

	return (win_end > base ? win_end : base) + depth;
}

/** The status record, sent once per flip and at least every 250 us. */
struct txs_status {
	uint32_t session;
	/* Word the engine plays now, the first one not written or zeroed, the limit, the restart. */
	uint64_t play, contig, credit, restart;
	/* Windows closed short, words lost (zeroed for a gap, not recovered), words of packets that
	 * came too late, words played stale.
	 */
	uint32_t underruns;
	uint64_t lost, late, stale;
};

#define TXS_STATUS_BYTES 64U

/**
 * Fill a record from the parts; `depth` as in txs_credit_limit(), the window end is j->hi
 * (0 before txs_jit_init()). CPU1 thread only (it reads the
 * live state, and `play` is that core's position): publish the result through a txs_snap for the
 * other core.
 */
void txs_status_make(struct txs_status *st, uint32_t session, uint64_t play,
		     const struct txs_jit *j, const struct txs_flip *f, uint32_t depth);

/** Big endian, 64 bytes. */
void txs_status_put(uint8_t *out, const struct txs_status *st);

/** @return 0, or -1 if len is not TXS_STATUS_BYTES. */
int txs_status_get(struct txs_status *st, const uint8_t *in, uint32_t len);

/**
 * The status record handed from the CPU1 thread to another core: a sequence lock around the 64
 * bytes as 16 words (a 64 bit load is two on RV32 and tears). One writer, any readers.
 */
struct txs_snap {
	_Atomic uint32_t seq;
	_Atomic uint32_t w[TXS_STATUS_BYTES / 4U];
};

void txs_snap_init(struct txs_snap *s);

/** Writer: txs_snap_write() is begin, put, end. */
void txs_snap_begin(struct txs_snap *s);
void txs_snap_put(struct txs_snap *s, const struct txs_status *st);
void txs_snap_end(struct txs_snap *s);

static inline void txs_snap_write(struct txs_snap *s, const struct txs_status *st)
{
	txs_snap_begin(s);
	txs_snap_put(s, st);
	txs_snap_end(s);
}

/** Reader: one attempt. @return false while a write is in progress or one came in between: retry. */
bool txs_snap_read(const struct txs_snap *s, struct txs_status *st);

/** The same, the record as the 64 bytes txs_status_put() makes (for a packet: no decode, no encode). */
bool txs_snap_read_raw(const struct txs_snap *s, uint8_t raw[TXS_STATUS_BYTES]);

#endif /* TXS_CREDIT_H_ */
