/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ZNP dispatcher: see include/zigbee/znp.h.
 */
#include <string.h>

#include <zigbee/znp.h>

#include "znp_priv.h"

static void send_frame(struct znp *z, const struct mt_frame *f)
{
	uint8_t wire[MT_MAX_WIRE];
	size_t n = mt_encode(f, wire);

	k_mutex_lock(&z->tx_lock, K_FOREVER);
	z->cfg.tx(z->cfg.user, wire, n);
	k_mutex_unlock(&z->tx_lock);
}

static void rpc_error(struct znp *z, const struct mt_frame *req, enum mt_rpc_error err)
{
	struct mt_frame f = {.type = MT_SRSP, .sub = MT_RPC_ERROR, .cmd = 0, .len = 3};

	f.data[0] = err;
	f.data[1] = (uint8_t)((req->type << 5) | req->sub);
	f.data[2] = req->cmd;
	send_frame(z, &f);
}

static void flush_deferred(struct znp *z)
{
	for (uint8_t i = 0; i < z->ndeferred; i++) {
		znp_send_areq(z, z->deferred[i].sub, z->deferred[i].cmd, z->deferred[i].data,
			      z->deferred[i].len);
	}
	z->ndeferred = 0;
}

static void dispatch_one(struct znp *z, const struct mt_frame *req)
{
	znp_handler_t h = req->sub < MT_SUBSYS_COUNT ? z->handler[req->sub] : NULL;

	if (req->type == MT_AREQ) {
		/*
		 * SYS_RESET_REQ is the one AREQ a host sends. Handlers of other commands write an
		 * SRSP and have none to write to.
		 */
		if (h != NULL && req->sub == MT_SYS && req->cmd == 0x00U) {
			(void)h(z, req, NULL);
		}
		return;
	}
	if (req->type != MT_SREQ) {
		return;
	}
	if (h == NULL) {
		rpc_error(z, req, MT_ERR_SUBSYSTEM);
		return;
	}

	struct mt_frame *rsp = &z->rsp;
	int err;

	rsp->type = MT_SRSP;
	rsp->sub = req->sub;
	rsp->cmd = req->cmd;
	rsp->len = 0;
	err = h(z, req, rsp);
	if (err != 0) {
		rpc_error(z, req, (enum mt_rpc_error)err);
	} else {
		send_frame(z, rsp);
	}
}

static void dispatch(struct znp *z, const struct mt_frame *req)
{
	z->dispatching = k_current_get();
	z->ndeferred = 0;
	dispatch_one(z, req);
	z->dispatching = NULL;
	flush_deferred(z);
}

void znp_init(struct znp *z, const struct znp_config *cfg)
{
	memset(z, 0, sizeof(*z));
	z->cfg = *cfg;
	z->tx_power = 20;
	k_mutex_init(&z->tx_lock);
	mt_parser_init(&z->parser);
	znp_register(z, MT_SYS, znp_sys_handler);
	znp_register(z, MT_UTIL, znp_util_handler);
}

/* The MT capability bit of a subsystem. */
static uint16_t caps_bit(enum mt_subsys sub)
{
	switch (sub) {
	case MT_ZGP:
		return BIT(9);
	case MT_APP_CNF:
		return BIT(10);
	case MT_RPC_ERROR:
		return 0;
	default:
		return sub >= MT_SYS && sub <= MT_APP ? BIT(sub - 1) : 0;
	}
}

void znp_register(struct znp *z, enum mt_subsys sub, znp_handler_t handler)
{
	z->handler[sub] = handler;
	z->caps |= caps_bit(sub);
}

void znp_rx(struct znp *z, const uint8_t *buf, size_t n)
{
	while (n-- > 0U) {
		if (mt_parser_feed(&z->parser, *buf++) == MT_FEED_FRAME) {
			dispatch(z, &z->parser.frame);
		}
	}
}

void znp_rx_idle(struct znp *z)
{
	mt_parser_reset(&z->parser);
}

void znp_send_areq(struct znp *z, enum mt_subsys sub, uint8_t cmd, const void *data, uint8_t len)
{
	struct mt_frame f = {.type = MT_AREQ, .sub = sub, .cmd = cmd, .len = len};

	memcpy(f.data, data, len);
	send_frame(z, &f);
}

void znp_send_areq_after(struct znp *z, enum mt_subsys sub, uint8_t cmd, const void *data,
			 uint8_t len)
{
	if (z->dispatching == k_current_get() && z->ndeferred < ZNP_DEFER_MAX &&
	    len <= ZNP_DEFER_DATA) {
		struct znp_deferred *d = &z->deferred[z->ndeferred++];

		d->sub = sub;
		d->cmd = cmd;
		d->len = len;
		memcpy(d->data, data, len);
		return;
	}
	znp_send_areq(z, sub, cmd, data, len);
}

void znp_send_reset_ind(struct znp *z, enum znp_reset_reason reason)
{
	const uint8_t d[] = {reason, ZNP_TRANSPORT_REV, ZNP_PRODUCT_ID, ZNP_VERSION_MAJOR,
			     ZNP_VERSION_MINOR, ZNP_VERSION_MAINT};

	znp_send_areq(z, MT_SYS, 0x80, d, sizeof(d));
}
