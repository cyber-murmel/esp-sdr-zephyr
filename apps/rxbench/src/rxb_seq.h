/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Sequence accounting of the consumer: gaps (lost packets and gap events), duplicates (the
 * previous packet again), packets out of order (an older number than the previous one: the late
 * arrival of a packet counted as lost, so lost minus ooo is the net loss) and restarts (a number
 * more than RXB_SEQ_RESTART behind: the sender began again, counting from its own number; the
 * accounting follows it without counting a loss). Plain C.
 */

#ifndef RXB_SEQ_H_
#define RXB_SEQ_H_

#include <stdbool.h>
#include <stdint.h>

/* Further behind than this is a sender that started again, not a late packet */
#define RXB_SEQ_RESTART 65536

struct rxb_seq_counts {
	uint32_t lost;	/* sequence numbers skipped */
	uint32_t gaps;	/* skips */
	uint32_t dups;
	uint32_t ooo;
	uint32_t restarts;
};

struct rxb_seq {
	bool have;		/* a first packet has set expected */
	uint32_t expected;	/* the next number */
	uint32_t first;		/* the first number of the run, last: the latest accepted */
	uint32_t last;
};

/* Forget the history: the next packet only sets the number to expect. */
static inline void rxb_seq_reset(struct rxb_seq *s)
{
	s->have = false;
	s->expected = 0U;
}

static inline void rxb_seq_track(struct rxb_seq *s, uint32_t seq, struct rxb_seq_counts *c)
{
	int32_t d;

	if (!s->have) {
		s->have = true;
		s->expected = seq + 1U;
		s->first = seq;
		s->last = seq;
		return;
	}
	d = (int32_t)(seq - s->expected);
	if (d == 0) {
		s->expected++;
		s->last = seq;
	} else if (d > 0) {
		c->lost += (uint32_t)d;
		c->gaps++;
		s->expected = seq + 1U;
		s->last = seq;
	} else if (d == -1) {
		c->dups++;
	} else if (d < -RXB_SEQ_RESTART) {
		c->restarts++;
		s->expected = seq + 1U;
		s->first = seq;
		s->last = seq;
	} else {
		c->ooo++;
	}
}

#endif /* RXB_SEQ_H_ */
