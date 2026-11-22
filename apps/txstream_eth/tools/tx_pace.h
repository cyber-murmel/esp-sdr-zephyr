/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * vrt_tx: the pacing and credit logic of the sender, without sockets or clocks of its own (the
 * caller passes CLOCK_MONOTONIC in ns), so that the unit test (test_tx.c) can drive it against a
 * simulated board. Not thread safe: vrt_tx holds a mutex around every call.
 *
 * The board plays at a fixed rate; the sender never sees its clock, only TXST records (play
 * index at the board's time, one way delay unknown and varying, 100 to 600 us). It estimates when
 * the play index was 0 on its own clock:
 *     t0 = min over the last 100 ms of (arrival time - play * ns_per_word)  -  owd_min
 * The minimum over many records picks the one with the least delay; the remainder, owd_min, is
 * half the least round trip of the TXPG/TXPO pings of the last 5 s (symmetric paths assumed: an
 * asymmetry of a us is an error of half a us). The error left is the jitter of the best of 400
 * records, a few us, plus half the drift of the two clocks in 100 ms (50 ppm: 2.5 us).
 *
 * The spread of the delay of each direction is measured on its own, from clocks that need not
 * agree: the board stamps each ping with its own free running clock (TXPO b_ns), so
 * b_ns - t_ns varies with the PC to board delay only (range over 1 s = the spread of that
 * direction, up to the drift of the two clocks in 1 s, which only makes it larger); the same for
 * the board to PC direction from arrival - play * ns_per_word of the records. The largest one way
 * delay is then owd_min + spread. (Half of the largest round trip would be wrong: a spike in one
 * direction only is half as big as the round trip.)
 *
 * Pacing: credit limited, not sample rate paced. The board writes the open window on arrival
 * (credit = end of the open window + depth, rising by G = 32768 at each flip), the line is faster
 * than the stream, so the sender sends everything up to the credit as soon as it knows it, in
 * bursts of at most TXP_BURST (TX_BURST, 4) packets back to back, bursts spaced so that the average stays
 * under --max-mbps (default 900) of the line: a burst of n packets leaves the next one no earlier than
 * n * line_ns later (line_ns = wire bytes * 8 / max_mbps). The credit is never exceeded.
 *   What it needs of the network: right after a flip the board wants the rest of the new window,
 *   G - depth words, in the time to the next flip, G / rate. The sender learns of the flip one
 *   status delay late and the data needs a data delay to arrive, so
 *       D_status + D_data <= G/rate - ceil((G - depth)/packet) * line_ns - point of no return
 *   (txp_delay_budget_ns(): 40 MS/s, depth 7020, 900 Mb/s: about 0.3 ms; depth 11200: 0.45 ms;
 *   20 MS/s: 1.1 and 1.2 ms). The sample rate does not enter except through G/rate.
 *   The lead (words ahead of the estimated play index) is no longer a rule: it is a floor that
 *   warns (lead_low counts plans that end below G + 1000, min_lead keeps the least).
 *
 * Resync: a packet sent now arrives owd + jitter later; if the sender's index is below
 * play + rate * (owd_min + 100 us) the packet would be late for any window. Then (after a stall of the PC or
 * of the board) the sender jumps to max(restart of the board, play + that horizon), which leaves
 * a gap the board zeroes, and goes on from there.
 */

#ifndef TX_PACE_H_
#define TX_PACE_H_

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "tx_hold.h"
#include "tx_proto.h"

#define TXP_MINQ_N 4096U

/* Sliding window minimum, a monotonic deque: values rise from the front to the back. */
struct txp_minq {
	int64_t t[TXP_MINQ_N], v[TXP_MINQ_N];
	uint32_t head, n;
};

static inline void txp_minq_reset(struct txp_minq *q)
{
	q->head = 0;
	q->n = 0;
}

static inline void txp_minq_push(struct txp_minq *q, int64_t t, int64_t v, int64_t win)
{
	while (q->n != 0U && q->v[(q->head + q->n - 1U) % TXP_MINQ_N] >= v) {
		q->n--;
	}
	if (q->n == TXP_MINQ_N) {
		q->head = (q->head + 1U) % TXP_MINQ_N;
		q->n--;
	}
	q->t[(q->head + q->n) % TXP_MINQ_N] = t;
	q->v[(q->head + q->n) % TXP_MINQ_N] = v;
	q->n++;
	while (q->n != 0U && q->t[q->head] < t - win) {
		q->head = (q->head + 1U) % TXP_MINQ_N;
		q->n--;
	}
}

static inline bool txp_minq_get(const struct txp_minq *q, int64_t *v)
{
	if (q->n == 0U) {
		return false;
	}
	*v = q->v[q->head];
	return true;
}

#define TXP_EST_WIN_NS (100 * 1000000LL)
#define TXP_RTT_WIN_NS (5000 * 1000000LL)
#define TXP_SPREAD_WIN_NS (1000 * 1000000LL)
#define TXP_FLOOR_BASE 1000U        /* the lead floor over G, words: point of no return and the estimate's error */
#define TXP_OWD_DEFAULT_NS 200000LL /* D_max before the first pongs */
#define TXP_BURST      TX_BURST     /* packets back to back: tx_hold.h (the board holds TX_HOLD buffers: depth + burst + 2) */
#define TXP_ARREARS_NS    30000LL      /* the most of a late wakeup that the next pause makes up */
#define TXP_WIRE_OVERHEAD 86U       /* per packet on the wire beyond the VITA packet: Ethernet 14 + FCS 4 + preamble and gap 20 + IPv6 40 + UDP 8 */
#define TXP_MAX_MBPS   900U
#define TXP_HORIZON_NS 100000LL /* margin of the resync horizon, over the least delay (not D_max: a spike must not make the sender jump) */

struct txp_pace {
	uint32_t rate_hz, npw; /* words per second, ns per word */
	uint32_t pkt_words, group, depth;
	int64_t line_ns;       /* ns of line time one packet is charged (wire bits / max_mbps) */
	int64_t next_burst_ns; /* no burst before this */
	int64_t run_since_ns;
	int64_t min_lead;      /* least lead (words) 100 ms after the engine runs; INT64_MAX: none */
	uint64_t lead_low;     /* plans that ended with a lead under group + TXP_FLOOR_BASE */
	uint64_t sent;         /* the next index to send */
	bool have_start, running, jumping;
	uint64_t credit, restart, st_play;
	uint32_t underruns;
	int64_t st_rx_ns;
	struct txp_minq est, rtt;
	/* data direction: min and max (negated) of b_ns - t_ns; status direction: of arrival - play * npw */
	struct txp_minq up_min, up_max, dn_min, dn_max;
	/* statistics */
	uint64_t resyncs, credit_waits, statuses, pongs;
};

struct txp_plan {
	uint64_t first; /* index of the first packet to send */
	uint32_t n;     /* packets, consecutive */
	int64_t wake_ns; /* when to plan again (a status may come sooner: see credit_bound) */
	bool resync;
	bool credit_bound; /* nothing to send until a status raises the credit */
};

/** Charge one packet of pkt_bytes (the VITA packet) at max_mbps of the line. */
static inline void txp_pace_set_line(struct txp_pace *p, uint32_t pkt_bytes, uint32_t max_mbps)
{
	p->line_ns = (int64_t)(pkt_bytes + TXP_WIRE_OVERHEAD) * 8000 / (int64_t)(max_mbps ? max_mbps : 1);
}

static inline void txp_pace_init(struct txp_pace *p, uint32_t rate_msps, uint32_t pkt_words,
				 uint32_t group, uint32_t depth)
{
	memset(p, 0, sizeof(*p));
	p->rate_hz = rate_msps * 1000000U;
	p->npw = 1000U / rate_msps;
	p->pkt_words = pkt_words;
	p->group = group;
	p->depth = depth;
	p->min_lead = INT64_MAX;
	txp_pace_set_line(p, 1420U, TXP_MAX_MBPS);
}

static inline int64_t txp_owd_ns(const struct txp_pace *p)
{
	int64_t r;

	return txp_minq_get(&p->rtt, &r) ? r / 2 : 0;
}

static inline int64_t txp_spread(const struct txp_minq *lo, const struct txp_minq *negmax)
{
	int64_t a, b;

	return txp_minq_get(lo, &a) && txp_minq_get(negmax, &b) ? -b - a : TXP_OWD_DEFAULT_NS;
}

/** The largest one way delay of the data direction (PC to board), ns. */
static inline int64_t txp_owd_max_ns(const struct txp_pace *p)
{
	return txp_owd_ns(p) + txp_spread(&p->up_min, &p->up_max);
}

/** The largest one way delay of the status direction (board to PC), ns. */
static inline int64_t txp_owd_stat_max_ns(const struct txp_pace *p)
{
	return txp_owd_ns(p) + txp_spread(&p->dn_min, &p->dn_max);
}

/**
 * The most D_status + D_data the board's credit and the line carry (ns): the time from a flip to the
 * next one, less the time to send the rest of the new window at line_ns a packet, less the point
 * of no return (300 words) and a packet. Negative: the depth cannot work at all.
 */
static inline int64_t txp_delay_budget_ns(const struct txp_pace *p)
{
	uint64_t rest = p->group > p->depth ? p->group - p->depth : 0;
	int64_t pk = (int64_t)((rest + p->pkt_words - 1U) / p->pkt_words);

	return (int64_t)p->group * (int64_t)p->npw - pk * p->line_ns - 300 * (int64_t)p->npw - p->line_ns;
}

/** Play index estimate at now_ns. */
static inline uint64_t txp_pace_play(const struct txp_pace *p, int64_t now_ns)
{
	int64_t u;

	if (!p->running || !txp_minq_get(&p->est, &u)) {
		return p->st_play;
	}
	u -= txp_owd_ns(p);
	return now_ns > u ? (uint64_t)(now_ns - u) / p->npw : 0U;
}

/** The time at which the estimated play index is w. */
static inline int64_t txp_pace_when(const struct txp_pace *p, uint64_t w)
{
	int64_t u = 0;

	(void)txp_minq_get(&p->est, &u);
	return u - txp_owd_ns(p) + (int64_t)w * (int64_t)p->npw;
}

/** The echo of a ping the sender made at t_ns arrives at now_ns; the board's clock read b_ns. */
static inline void txp_pace_pong(struct txp_pace *p, int64_t now_ns, int64_t t_ns, int64_t b_ns)
{
	if (now_ns > t_ns) {
		txp_minq_push(&p->rtt, now_ns, now_ns - t_ns, TXP_RTT_WIN_NS);
		txp_minq_push(&p->up_min, now_ns, b_ns - t_ns, TXP_SPREAD_WIN_NS);
		txp_minq_push(&p->up_max, now_ns, -(b_ns - t_ns), TXP_SPREAD_WIN_NS);
		p->pongs++;
	}
}

/** A TXST record arrived at now_ns. */
static inline void txp_pace_status(struct txp_pace *p, int64_t now_ns, const struct txp_st *m)
{
	const struct txp_status *s = &m->status;
	bool run = (m->flags & TXP_ST_RUNNING) != 0U;

	p->statuses++;
	if (s->credit > p->credit) {
		p->credit = s->credit;
	}
	p->restart = s->restart;
	p->underruns = s->underruns;
	if (!p->have_start) {
		p->have_start = true;
		p->sent = s->restart;
	}
	if (run) {
		if (!p->running) {
			txp_minq_reset(&p->est);
			txp_minq_reset(&p->dn_min);
			txp_minq_reset(&p->dn_max);
		}
		int64_t u = now_ns - (int64_t)(s->play * p->npw);

		txp_minq_push(&p->est, now_ns, u, TXP_EST_WIN_NS);
		txp_minq_push(&p->dn_min, now_ns, u, TXP_SPREAD_WIN_NS);
		txp_minq_push(&p->dn_max, now_ns, -u, TXP_SPREAD_WIN_NS);
	}
	p->running = run;
	p->st_play = s->play;
	p->st_rx_ns = now_ns;
}

/**
 * What to send now. The caller sends plan->n packets of pkt_words words from index plan->first,
 * back to back, and calls this again at plan->wake_ns, or as soon as a status arrives if
 * plan->credit_bound.
 */
static inline void txp_pace_plan(struct txp_pace *p, int64_t now_ns, uint32_t max_pk,
				 struct txp_plan *out)
{
	const uint64_t pkt = p->pkt_words;
	uint64_t play = 0, n;

	out->first = p->sent;
	out->n = 0;
	out->resync = false;
	out->credit_bound = false;
	out->wake_ns = now_ns + 1000000;
	if (!p->have_start) {
		return;
	}
	if (p->running) {
		const uint64_t horizon = (uint64_t)((txp_owd_ns(p) + TXP_HORIZON_NS) / p->npw);
		uint64_t floor_idx;

		play = txp_pace_play(p, now_ns);
		floor_idx = play + horizon;
		if (p->sent < floor_idx || p->sent < p->restart) {
			uint64_t to = p->restart > floor_idx ? p->restart : floor_idx;

			/* never beyond the credit: a later packet may be late, waiting is worse */
			if (to + pkt > p->credit && p->credit >= p->restart + pkt) {
				to = p->credit - pkt;
			}
			/* and never back: a sender at the edge of its credit (room under a packet, the usual place) would
			 * land under its own index and send the words of the last packet again (the board counts them
			 * as duplicates, 377 words on average each); it waits for the credit of the next flip instead */
			if (to <= p->sent || to + pkt > p->credit) {
				p->credit_waits++;
				out->credit_bound = true;
				return;
			}
			p->sent = to;
			out->first = to;
			/* one resync per episode: the jumps of a stall that goes on are one */
			if (!p->jumping) {
				p->resyncs++;
				out->resync = true;
			}
			p->jumping = true;
		} else {
			p->jumping = false;
		}
		if (p->run_since_ns == 0) {
			p->run_since_ns = now_ns;
		}
	}
	if (now_ns < p->next_burst_ns) {
		out->wake_ns = p->next_burst_ns;
		return;
	}
	n = p->credit > p->sent ? (p->credit - p->sent) / pkt : 0U;
	if (n > TXP_BURST) {
		n = TXP_BURST;
	}
	if (n > max_pk) {
		n = max_pk;
	}
	if (n == 0U) {
		p->credit_waits++;
		out->credit_bound = max_pk != 0U;
		return;
	}
	out->n = (uint32_t)n;
	p->sent += n * pkt;
	/* a wakeup that came late is made up by a shorter pause after it, up to TXP_ARREARS_NS: with bursts of 4 a
	 * flip's 36 packets are 9 wakeups, and the lateness of each must not add up */
	p->next_burst_ns = (p->next_burst_ns > now_ns - TXP_ARREARS_NS ? p->next_burst_ns : now_ns - TXP_ARREARS_NS) +
			   (int64_t)n * p->line_ns;
	out->wake_ns = p->next_burst_ns;
	if (p->running && now_ns - p->run_since_ns > 100000000LL) {
		int64_t lead = (int64_t)p->sent - (int64_t)play;

		if (lead < p->min_lead) {
			p->min_lead = lead;
		}
		if (lead < (int64_t)p->group + TXP_FLOOR_BASE) {
			p->lead_low++;
		}
	}
}

#endif /* TX_PACE_H_ */
