/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The session, credit and packetizer logic of the transmit stream's sender, as a plain C core:
 * no sockets, no clocks (the caller passes CLOCK_MONOTONIC in ns), no threads, no globals, no
 * printing. vrt_tx.c is a thin command line around it; the Soapy module's VrtLink can call it from
 * C++ (the header is static inline and has no C++ trouble; see doc/dev/txs.md "Reusing the core").
 * Not thread safe: the caller serialises calls (one mutex), except txc_header(), which only the
 * thread that sends data packets calls.
 *
 * Use:
 *   txc_init(&c, rate_msps, fmt, flags, seconds, nonce, pkt_words, max_mbps);
 *   loop of the control thread:  now = clock;
 *       while ((n = txc_poll(&c, now, buf, sizeof(buf))) > 0) send(buf, n) to the control port;
 *       on a datagram from the board:  ev = txc_rx(&c, now, d, len);
 *       sleep until txc_wake(&c) or the next datagram.
 *   loop of the data thread, when txc_state(&c) == TXC_RUN:
 *       n = txc_plan(&c, now, 16, &pl);  for k < n: idx = pl.first + k * pkt_words;
 *       txc_header(&c, pkt + k * len, idx); fill the payload (pkt_words samples of stream index idx)
 *       at pkt + k * len + TXC_HDR_BYTES; send all to the data port;  sleep until pl.wake_ns.
 *   end: txc_stop(&c) (TXEN is sent by txc_poll()); TXC_DONE when the board says ENDED.
 */

#ifndef TX_CORE_H_
#define TX_CORE_H_

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "tx_pace.h"
#include "tx_proto.h"

#define TXC_HDR_BYTES 16U
#define TXC_PROBE_PONGS 100U /* pings before TXGO: D_max is known when the run starts */

enum txc_state {
	TXC_HELLO,  /* waiting for VRTC */
	TXC_SUB,    /* subscribed, probing the delay with pings */
	TXC_GO,     /* TXGO sent, waiting for TXAK */
	TXC_RUN,
	TXC_END,    /* TXEN sent, waiting for ENDED */
	TXC_DONE,
	TXC_FAILED, /* see c->fail */
};

/* txc_rx() results */
#define TXC_EV_STATUS 1U
#define TXC_EV_ACK    2U /* run accepted */
#define TXC_EV_FAIL   4U /* refused, busy, other host: c->fail, c->ak.code */
#define TXC_EV_ENDED  8U
#define TXC_EV_ARMWAIT 16U /* RF refused, not armed: the subscription stays and TXGO is retried every second */

/* c->fail, as vrt_tx's exit codes */
#define TXC_FAIL_NONE    0
#define TXC_FAIL_NOCONT  1 /* no answer */
#define TXC_FAIL_BUSY    3 /* another host (VRTB) or TXAK busy */
#define TXC_FAIL_REFUSED 4 /* TXGO refused: c->ak.code */
#define TXC_FAIL_ENDED   5 /* the board ended the run (ENDED, or not running for TXC_NOTRUN_NS), or went silent */
/* in the run: not RUNNING in the statuses for this long after it was, or no status at all */
#define TXC_NOTRUN_NS    200000000LL
#define TXC_SILENT_NS    2000000000LL

struct txc {
	struct txp_pace pace;
	enum txc_state state;
	int fail;
	uint8_t cookie[TXP_COOKIE_BYTES];
	bool have_cookie;
	uint32_t rate_msps, fmt, flags, seconds, nonce, pkt_words, sample_bytes;
	struct txp_ak ak;
	struct txp_st first_st, last_st;
	bool have_first;
	int64_t t_start, t_sub, t_ping, t_go, t_end;
	/* RF asked and refused with code 5: wait this long for `tx arm` on the board (0: fail at once) */
	int64_t arm_wait_ns, t_arm0;
	int64_t t_last_st, t_notrun; /* the last status; since when the statuses say not running (0: they say running) */
	uint32_t ping_seq, go_tries, end_tries;
	uint8_t count;
	uint32_t max_mbps;
	bool want_sub_now, stop_req;
};

static inline void txc_init(struct txc *c, uint32_t rate_msps, uint32_t fmt, uint32_t flags,
			    uint32_t seconds, uint32_t nonce, uint32_t pkt_words, uint32_t max_mbps)
{
	memset(c, 0, sizeof(*c));
	c->rate_msps = rate_msps;
	c->fmt = fmt;
	c->flags = flags;
	c->seconds = seconds;
	c->nonce = nonce;
	c->sample_bytes = fmt == TXP_FMT_IQ8 ? 2U : 4U;
	c->pkt_words = pkt_words != 0U ? pkt_words : 1404U / c->sample_bytes;
	txp_pace_init(&c->pace, rate_msps, c->pkt_words, 32768U, 0U);
	txp_pace_set_line(&c->pace, TXC_HDR_BYTES + c->pkt_words * c->sample_bytes,
			  max_mbps != 0U ? max_mbps : TXP_MAX_MBPS);
	c->max_mbps = max_mbps != 0U ? max_mbps : TXP_MAX_MBPS;
	c->state = TXC_HELLO;
	c->t_start = -1;
}

static inline enum txc_state txc_state(const struct txc *c)
{
	return c->state;
}

/** Bytes of a data packet. */
static inline uint32_t txc_packet_bytes(const struct txc *c)
{
	return TXC_HDR_BYTES + c->pkt_words * c->sample_bytes;
}

/** Write the VITA header of the packet starting at stream index idx (data thread only). */
static inline void txc_header(struct txc *c, uint8_t *o, uint64_t idx)
{
	uint32_t size = txc_packet_bytes(c) / 4U;

	txp_put32(o, 0x10100000U | ((uint32_t)(c->count++ & 0xfU) << 16) | size);
	txp_put32(o + 4, c->ak.stream_id);
	txp_put64(o + 8, idx);
}

static inline void txc_stop(struct txc *c)
{
	c->stop_req = true;
}

static inline void txc_vrts(const struct txc *c, uint8_t *out, int *len)
{
	memcpy(out, "VRTS", 4);
	if (!c->have_cookie) {
		*len = 4;
		return;
	}
	memcpy(out + 4, c->cookie, 8);
	*len = 12;
}

/**
 * The next control datagram due at now_ns, written to out (cap at least 32): returns its length, or
 * 0 when none is due (call again until 0). It is also where the state advances on time.
 */
static inline int txc_poll(struct txc *c, int64_t now, uint8_t *out, size_t cap)
{
	int len = 0;

	if (cap < 32U || c->state == TXC_DONE || c->state == TXC_FAILED) {
		return 0;
	}
	if (c->t_start < 0) {
		c->t_start = now;
	}
	if (c->state == TXC_HELLO && now - c->t_start > 5000000000LL) {
		c->state = TXC_FAILED;
		c->fail = TXC_FAIL_NOCONT;
		return 0;
	}
	if (c->state == TXC_RUN && c->t_last_st != 0 && now - c->t_last_st > TXC_SILENT_NS) {
		c->state = TXC_FAILED;
		c->fail = TXC_FAIL_ENDED;
		return 0;
	}
	if (c->stop_req && c->state == TXC_RUN) {
		c->state = TXC_END;
		c->t_end = now;
		c->end_tries = 0;
	}
	if (c->stop_req && (c->state == TXC_HELLO || c->state == TXC_SUB || c->state == TXC_GO)) {
		c->state = TXC_FAILED;
		return 0;
	}
	if (c->state == TXC_END) {
		if (c->end_tries == 0U || (now - c->t_end > 500000000LL * (int64_t)c->end_tries &&
					   c->end_tries < 6U)) {
			struct txp_en en = {.nonce = c->nonce};

			memcpy(en.cookie, c->cookie, 8);
			c->end_tries++;
			return txp_en_put(out, cap, &en);
		}
		if (now - c->t_end > 3000000000LL) {
			c->state = TXC_DONE; /* the board never said ENDED */
		}
		return 0;
	}
	if (now >= c->t_sub) {
		txc_vrts(c, out, &len);
		c->t_sub = now + (c->have_cookie ? 250000000LL : 200000000LL);
		return len;
	}
	if (c->have_cookie && c->state != TXC_END && now >= c->t_ping) {
		struct txp_ping pg = {.seq = c->ping_seq++, .t_ns = (uint64_t)now};

		memcpy(pg.cookie, c->cookie, 8);
		c->t_ping = now + 5000000LL;
		return txp_pg_put(out, cap, &pg);
	}
	if (c->state == TXC_SUB && (c->pace.pongs >= TXC_PROBE_PONGS || c->ping_seq > 200)) {
		c->state = TXC_GO;
		c->t_go = now;
	}
	if (c->state == TXC_GO && now >= c->t_go) {
		struct txp_go go = {.rate_msps = (uint8_t)c->rate_msps, .fmt = (uint8_t)c->fmt,
				    .flags = (uint16_t)c->flags, .seconds = c->seconds, .nonce = c->nonce};

		if (c->go_tries++ >= 10U) {
			c->state = TXC_FAILED;
			c->fail = TXC_FAIL_NOCONT;
			return 0;
		}
		memcpy(go.cookie, c->cookie, 8);
		c->t_go = now + 300000000LL;
		return txp_go_put(out, cap, &go);
	}
	return 0;
}

/** When txc_poll() has something to do next (ns). */
static inline int64_t txc_wake(const struct txc *c)
{
	int64_t w = c->t_sub;

	if (c->have_cookie && c->t_ping < w) {
		w = c->t_ping;
	}
	if (c->state == TXC_GO && c->t_go < w) {
		w = c->t_go;
	}
	if (c->state == TXC_END) {
		w = c->t_end + 500000000LL * (int64_t)(c->end_tries ? c->end_tries : 1U);
	}
	return w;
}

/** A datagram from the control port arrived at now_ns. Returns TXC_EV_* bits. */
static inline unsigned txc_rx(struct txc *c, int64_t now, const uint8_t *d, size_t len)
{
	unsigned ev = 0;
	char k = txp_kind(d, len);

	if (len == 12 && memcmp(d, "VRTC", 4) == 0) {
		memcpy(c->cookie, d + 4, 8);
		c->have_cookie = true;
		c->t_sub = now; /* subscribe at once with the cookie */
		if (c->state == TXC_HELLO) {
			c->state = TXC_SUB;
		}
	} else if (len == 4 && memcmp(d, "VRTB", 4) == 0) {
		c->state = TXC_FAILED;
		c->fail = TXC_FAIL_BUSY;
		ev |= TXC_EV_FAIL;
	} else if (k == 'O') {
		struct txp_ping po;

		if (txp_po_get(&po, d, len) == 0) {
			txp_pace_pong(&c->pace, now, (int64_t)po.t_ns, (int64_t)po.b_ns);
		}
	} else if (k == 'A') {
		struct txp_ak a;

		if (txp_ak_get(&a, d, len) != 0 || a.nonce != c->nonce) {
			return 0;
		}
		if (c->state == TXC_GO) {
			if (a.code == TXP_RF_REFUSED && c->arm_wait_ns > 0 && (c->flags & TXP_GO_RF) != 0U &&
			    (c->t_arm0 == 0 || now - c->t_arm0 < c->arm_wait_ns)) {
				/* not armed (or out of band): the board keeps this subscriber; ask again every second */
				if (c->t_arm0 == 0) {
					c->t_arm0 = now;
					ev |= TXC_EV_ARMWAIT;
				}
				c->go_tries = 0;
				c->t_go = now + 1000000000LL;
			} else if (a.code != TXP_OK) {
				c->ak = a;
				c->state = TXC_FAILED;
				c->fail = a.code == TXP_BUSY ? TXC_FAIL_BUSY : TXC_FAIL_REFUSED;
				ev |= TXC_EV_FAIL;
			} else {
				c->ak = a;
				if (c->fmt == TXP_FMT_IQ8 ? c->pkt_words > (a.max_words & ~1U)
							  : c->pkt_words > a.max_words) {
					c->pkt_words = c->fmt == TXP_FMT_IQ8 ? a.max_words & ~1U : a.max_words;
					c->pace.pkt_words = c->pkt_words;
					txp_pace_set_line(&c->pace, txc_packet_bytes(c), c->max_mbps);
				}
				c->pace.group = a.group;
				c->pace.depth = a.depth;
				c->state = TXC_RUN;
				c->t_last_st = now;
				c->t_notrun = 0;
				ev |= TXC_EV_ACK;
			}
		}
	} else if (k == 'S' && c->state >= TXC_RUN) {
		struct txp_st m;

		if (txp_st_get(&m, d, len) == 0) {
			if (!c->have_first && (m.flags & TXP_ST_RUNNING)) {
				c->first_st = m;
				c->have_first = true;
			}
			txp_pace_status(&c->pace, now, &m);
			c->last_st = m;
			c->t_last_st = now;
			ev |= TXC_EV_STATUS;
			if (c->state == TXC_RUN) {
				/* the board ended the run (a hang, the deadman, a stop from its console): do not wait */
				if ((m.flags & TXP_ST_ENDED) != 0U) {
					c->state = TXC_FAILED;
					c->fail = TXC_FAIL_ENDED;
					ev |= TXC_EV_FAIL;
				} else if (c->have_first && (m.flags & TXP_ST_RUNNING) == 0U) {
					if (c->t_notrun == 0) {
						c->t_notrun = now;
					} else if (now - c->t_notrun > TXC_NOTRUN_NS) {
						c->state = TXC_FAILED;
						c->fail = TXC_FAIL_ENDED;
						ev |= TXC_EV_FAIL;
					}
				} else {
					c->t_notrun = 0;
				}
			}
			if ((m.flags & TXP_ST_ENDED) && c->state == TXC_END) {
				c->state = TXC_DONE;
				ev |= TXC_EV_ENDED;
			}
		}
	}
	return ev;
}

/** What to send now: see txp_pace_plan(). Only in TXC_RUN. Returns the packets (0 if none or not running). */
static inline uint32_t txc_plan(struct txc *c, int64_t now, uint32_t max_pk, struct txp_plan *pl)
{
	if (c->state != TXC_RUN) {
		memset(pl, 0, sizeof(*pl));
		pl->wake_ns = now + 300000;
		return 0;
	}
	txp_pace_plan(&c->pace, now, max_pk, pl);
	return pl->n;
}

/** Events since the first RUNNING status: underruns, lost words, late words; and resyncs. */
struct txc_counts {
	uint32_t underruns;
	uint64_t lost, late, stale, resyncs;
};

static inline struct txc_counts txc_counts(const struct txc *c)
{
	struct txc_counts r = {0};

	if (c->have_first) {
		r.underruns = c->last_st.status.underruns - c->first_st.status.underruns;
		r.lost = c->last_st.status.lost - c->first_st.status.lost;
		r.late = c->last_st.status.late - c->first_st.status.late;
		r.stale = c->last_st.status.stale - c->first_st.status.stale;
	}
	r.resyncs = c->pace.resyncs;
	return r;
}

/** True when the delays measured (status + data direction, worst) exceed what the credit depth and the line carry. */
static inline bool txc_depth_short(const struct txc *c)
{
	return c->pace.depth != 0U &&
	       txp_owd_max_ns(&c->pace) + txp_owd_stat_max_ns(&c->pace) > txp_delay_budget_ns(&c->pace);
}

#endif /* TX_CORE_H_ */
