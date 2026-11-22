/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The control state machine of a network run: see tx_sess.h.
 */

#include <errno.h>
#include <string.h>

#include "tx_sess.h"

/* 1404 bytes of payload: 702 iq8 samples (a sample is one DAC word) or 351 ready words */
uint32_t tx_sess_max_words(uint8_t fmt)
{
	return fmt == TXP_FMT_IQ8 ? 702U : 351U;
}

uint32_t tx_sess_depth(const struct tx_sess *s, uint8_t fmt)
{
	return (s->buffers - 2U) * tx_sess_max_words(fmt);
}

void tx_sess_init(struct tx_sess *s, uint32_t buffers, uint32_t group)
{
	memset(s, 0, sizeof(*s));
	s->buffers = buffers;
	s->group = group;
}

void tx_sess_reset(struct tx_sess *s)
{
	s->running = false;
	s->have = false;
	s->nonce = 0U;
	memset(&s->ak, 0, sizeof(s->ak));
}

void tx_sess_ended(struct tx_sess *s)
{
	s->running = false;
}

void tx_sess_go(struct tx_sess *s, const struct tx_sess_ops *o, void *ctx, const struct txp_go *m,
		struct txp_ak *ak)
{
	enum txp_code code = txp_go_valid(m);
	const bool rf = (m->flags & TXP_GO_RF) != 0U;
	uint32_t id;
	int ret;

	memset(ak, 0, sizeof(*ak));
	ak->nonce = m->nonce;
	/* the PC resends until it hears: the same nonce gets the same answer while its run is alive; after
	 * the run ended it is a stale duplicate and starts nothing (code 7) */
	if (s->have && s->nonce == m->nonce) {
		if (s->running) {
			*ak = s->ak;
		} else {
			ak->code = TXP_NO_RESOURCE;
		}
		return;
	}
	if (code != TXP_OK) {
		ak->code = (uint16_t)code;
		return;
	}
	if (rf && !o->rf_ok(ctx, m->rate_msps)) {
		ak->code = TXP_RF_REFUSED;
		return;
	}
	if (s->running) {
		ak->code = TXP_BUSY;
		return;
	}
	do {
		id = o->rand32(ctx);
	} while (id == 0U);
	ret = o->start(ctx, m, id, rf);
	if (ret != 0) {
		ak->code = ret == -EBUSY ? TXP_BUSY : TXP_NO_RESOURCE;
		return;
	}
	ak->code = TXP_OK;
	ak->flags = rf ? TXP_AK_RF : 0U;
	ak->stream_id = id;
	ak->max_words = tx_sess_max_words(m->fmt);
	ak->depth = tx_sess_depth(s, m->fmt);
	ak->group = s->group;
	s->running = true;
	s->have = true;
	s->nonce = m->nonce;
	s->ak = *ak;
}

void tx_sess_en(struct tx_sess *s, const struct tx_sess_ops *o, void *ctx, const struct txp_en *m,
		struct txp_ak *ak)
{
	memset(ak, 0, sizeof(*ak));
	ak->nonce = m->nonce;
	ak->code = TXP_OK;
	if (s->running) {
		o->stop(ctx);
	}
}
