/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A model of the board for the sender's tests (test_tx.c on a virtual clock, tx_fake.c on a real
 * one): a play index advancing at the sample rate, two windows of `group` words, the credit rule
 * of doc/dev/txs.md (credit = end of the open window + depth; before the first packet only the
 * first window's end), packets placed into the open window by index, underruns when a window is
 * closed short, a restart index. Time is a parameter, in ns. Not the real jitter cursor (no
 * reordering, holes or duplicates), only what the sender's pacing depends on.
 */

#ifndef TX_BOARD_H_
#define TX_BOARD_H_

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "tx_proto.h"

#define TBM_WQ 512U

struct tbm {
	uint32_t rate_msps, npw, group, depth, pnr, session;
	int64_t start_delay_ns, drift_ppm;
	uint64_t base;              /* index of the first word of the first window */
	bool started, run, closed;  /* started: a data word was seen */
	int64_t t0;                 /* time of play == base */
	uint64_t lo, hi, cursor;
	uint32_t underruns, status_seq;
	uint64_t lost, late, far, placed, unfilled;
	struct {
		uint64_t idx;
		uint32_t n;
	} wq[TBM_WQ];
	uint32_t nwq;
};

static inline void tbm_init(struct tbm *b, uint32_t rate_msps, uint32_t group, uint32_t depth,
			    uint64_t first, int64_t start_delay_ns)
{
	memset(b, 0, sizeof(*b));
	b->rate_msps = rate_msps;
	b->npw = 1000U / rate_msps;
	b->group = group;
	b->depth = depth;
	b->pnr = 300U;
	b->session = 0x5d5d1234U;
	b->start_delay_ns = start_delay_ns;
	b->base = first;
	b->lo = first;
	b->hi = first + group;
	b->cursor = first;
}

static inline uint64_t tbm_play(const struct tbm *b, int64_t now)
{
	int64_t e;

	if (!b->run || now <= b->t0) {
		return b->base;
	}
	e = now - b->t0;
	e = e * (1000000 + b->drift_ppm) / 1000000; /* the board's clock runs drift_ppm fast */
	return b->base + (uint64_t)e / b->npw;
}

static inline void tbm_place(struct tbm *b, uint64_t idx, uint32_t n);

static inline void tbm_flush_wait(struct tbm *b)
{
	uint32_t k = b->nwq;

	b->nwq = 0;
	for (uint32_t i = 0; i < k; i++) {
		uint64_t idx = b->wq[i].idx;
		uint32_t n = b->wq[i].n;

		tbm_place(b, idx, n);
	}
}

static inline void tbm_place(struct tbm *b, uint64_t idx, uint32_t n)
{
	while (n != 0U) {
		uint64_t wlo = b->closed ? b->hi : b->lo;
		uint32_t take;

		if (idx + n <= wlo) {
			b->late += n;
			return;
		}
		if (idx < wlo) {
			b->late += wlo - idx;
			n -= (uint32_t)(wlo - idx);
			idx = wlo;
		}
		if (idx >= b->hi + b->depth) {
			b->far += n;
			return;
		}
		if (idx >= b->hi) {
			if (b->nwq < TBM_WQ) {
				b->wq[b->nwq].idx = idx;
				b->wq[b->nwq].n = n;
				b->nwq++;
			} else {
				b->far += n;
			}
			return;
		}
		take = (uint32_t)(b->hi - idx < n ? b->hi - idx : n);
		if (idx > b->cursor) {
			b->lost += idx - b->cursor;
			b->cursor = idx;
		}
		if (idx + take > b->cursor) {
			b->placed += idx + take - b->cursor;
			b->cursor = idx + take;
		}
		idx += take;
		n -= take;
	}
}

static inline void tbm_close(struct tbm *b)
{
	if (b->cursor < b->hi) {
		if (b->started) {
			b->underruns++;
		}
		b->unfilled += b->hi - b->cursor;
		b->cursor = b->hi;
	}
	b->closed = true;
}

/** Do what is due at now: start the engine, close and flip the windows. Not called while the CPU stalls. */
static inline void tbm_advance(struct tbm *b, int64_t now)
{
	if (!b->run) {
		if (b->started && b->cursor >= b->hi) {
			b->run = true;
			b->t0 = now + b->start_delay_ns;
			b->lo = b->hi;
			b->hi += b->group;
			b->cursor = b->lo;
			tbm_flush_wait(b);
		}
		return;
	}
	for (;;) {
		uint64_t play = tbm_play(b, now);

		if (!b->closed && play + b->pnr >= b->lo) {
			tbm_close(b);
		}
		if (play < b->lo) {
			break;
		}
		if (!b->closed) {
			tbm_close(b);
		}
		b->lo = b->hi;
		b->hi += b->group;
		b->cursor = b->lo;
		b->closed = false;
		tbm_flush_wait(b);
	}
}

/** When tbm_advance() has to run next (INT64_MAX: when a packet comes). */
static inline int64_t tbm_deadline(const struct tbm *b)
{
	if (!b->run) {
		return INT64_MAX;
	}
	/* the elapsed time at which tbm_play() reaches the word, with the drift: rounded up */
	{
		int64_t words = (int64_t)(b->closed ? b->lo : b->lo - b->pnr) - (int64_t)b->base;
		int64_t need = words * (int64_t)b->npw;

		return b->t0 + (need * 1000000 + 1000000 + b->drift_ppm - 1) / (1000000 + b->drift_ppm);
	}
}

/** A data packet of n words at idx arrives (tbm_advance() was called for now). */
static inline void tbm_packet(struct tbm *b, int64_t now, uint64_t idx, uint32_t n)
{
	b->started = true;
	tbm_place(b, idx, n);
	tbm_advance(b, now);
}

static inline void tbm_status(struct tbm *b, int64_t now, struct txp_st *m)
{
	struct txp_status *s = &m->status;

	memset(m, 0, sizeof(*m));
	m->seq = b->status_seq++;
	s->session = b->session;
	s->play = tbm_play(b, now);
	s->contig = b->cursor;
	s->credit = b->started ? b->hi + b->depth : b->hi;
	s->restart = b->closed ? b->hi : b->cursor;
	s->underruns = b->underruns;
	s->lost = b->lost;
	s->late = b->late;
	m->flags = b->run && now >= b->t0 ? TXP_ST_RUNNING : 0U;
}

#endif /* TX_BOARD_H_ */
