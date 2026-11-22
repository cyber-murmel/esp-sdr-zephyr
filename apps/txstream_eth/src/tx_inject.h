/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The network's stand-in: a thread on CPU 0 that makes the packets the sender would,
 * 1420 B with absolute sample indexes (tx_tone.c's tone), in buffers of the receive pool, paced
 * by the credits the stream publishes, and hands them to tx_stream_put() as the receive hook will.
 * Optional loss, reorder and stall.
 */

#ifndef TX_INJECT_H_
#define TX_INJECT_H_

#include <stdbool.h>
#include <stdint.h>

#include "txs_wire.h"

struct tx_inj_cfg {
	/* packets dropped, in 1/1000 */
	uint32_t loss_pm;
	/* packets sent in reverse order, in batches of this many (0 or 1: in order) */
	uint32_t reorder;
	/* a pause of this many ms once a second (0: none, at most 50) */
	uint32_t stall_ms;
};

struct tx_inj_stats {
	uint64_t sent, words, dropped, reordered, stalls, resyncs, align_words, no_buf, ring_full, credit_waits, aborted;
	uint64_t next;
	uint32_t ms;
};

#if defined(CONFIG_APP_TXS_INJECT)
void tx_inject_set(const struct tx_inj_cfg *c);
void tx_inject_get(struct tx_inj_cfg *c);
/** Start the thread for the running stream's format. */
int tx_inject_start(void);
/** Ask it to end and wait for it (200 ms, then abort it); the ring goes back. Returns whether it ended
 * by itself. */
bool tx_inject_stop(void);
void tx_inject_stats(struct tx_inj_stats *s);
/** The packet rate alone: patch, reference, give back; no stream, no credit. cycles: per packet. */
int tx_inject_bench(enum txs_fmt fmt, uint32_t ms, uint64_t *words_per_s, uint32_t *cyc_per_pkt);
#else
/* Built without the injector (CONFIG_APP_TXS_INJECT=n): the network is the only source. */
#include <errno.h>
#include <string.h>

static inline void tx_inject_set(const struct tx_inj_cfg *c)
{
	(void)c;
}

static inline void tx_inject_get(struct tx_inj_cfg *c)
{
	memset(c, 0, sizeof(*c));
}

static inline int tx_inject_start(void)
{
	return -ENOTSUP;
}

static inline bool tx_inject_stop(void)
{
	return true;
}

static inline void tx_inject_stats(struct tx_inj_stats *s)
{
	memset(s, 0, sizeof(*s));
}

static inline int tx_inject_bench(enum txs_fmt fmt, uint32_t ms, uint64_t *words_per_s,
				  uint32_t *cyc_per_pkt)
{
	(void)fmt;
	(void)ms;
	(void)words_per_s;
	(void)cyc_per_pkt;
	return -ENOTSUP;
}
#endif

#endif
