/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The control state machine of a network run (doc/dev/txs.md, "Network protocol"): what a TXGO or a
 * TXEN from the subscriber does, and what the board answers. Plain C, no Zephyr calls: what the board
 * does (start the stream, stop it, the RF gate, the random number) comes in through the ops, so the
 * unit test (tests/unit/txs/src/test_sess.c) drives it on the host.
 */

#ifndef TX_SESS_H_
#define TX_SESS_H_

#include <stdbool.h>
#include <stdint.h>

#include "tx_proto.h"

struct tx_sess_ops {
	/** RF may go on air now: armed on the UART, `tx init` done, the tuned frequency passes txs_rf_ok(). */
	bool (*rf_ok)(void *ctx, uint8_t rate_msps);
	/** Start the stream; 0, or -errno (-EBUSY: a run goes on or has not been cleaned up). */
	int (*start)(void *ctx, const struct txp_go *go, uint32_t stream_id, bool rf);
	/** Drain and stop the run. */
	void (*stop)(void *ctx);
	/** A random 32 bit value. */
	uint32_t (*rand32)(void *ctx);
};

struct tx_sess {
	/** The packets the stream holds at most and the words of a window: the numbers TXAK carries. */
	uint32_t buffers, group;
	/** A run was started by the subscriber and the stream has not said it ended. */
	bool running;
	/** A run was started in this subscription (its TXAK is kept for a retried TXGO). */
	bool have;
	uint32_t nonce;
	struct txp_ak ak;
};

/** @param buffers the packets the stream holds at most, @param group words of a window */
void tx_sess_init(struct tx_sess *s, uint32_t buffers, uint32_t group);

/** The subscriber changed or went: forget the run (the caller stops it). */
void tx_sess_reset(struct tx_sess *s);

/** The stream reports the end of the run. */
void tx_sess_ended(struct tx_sess *s);

/** A TXGO from the subscriber (its cookie checked): the answer in *ak. */
void tx_sess_go(struct tx_sess *s, const struct tx_sess_ops *o, void *ctx, const struct txp_go *m,
		struct txp_ak *ak);

/** A TXEN from the subscriber: the answer in *ak (code 0, every other field 0, the nonce echoed). */
void tx_sess_en(struct tx_sess *s, const struct tx_sess_ops *o, void *ctx, const struct txp_en *m,
		struct txp_ak *ak);

/** Words in a data packet of the format, and the credit depth: (buffers - 2) of them. */
uint32_t tx_sess_max_words(uint8_t fmt);
uint32_t tx_sess_depth(const struct tx_sess *s, uint8_t fmt);

#endif
