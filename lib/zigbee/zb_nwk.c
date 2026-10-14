/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The coordinator's NWK, APS and trust center runtime: network key and frame counters, secured
 * frames to and from neighbours, APS acknowledgements, link status and the delivery of the
 * network key to a joining device. No routing: only direct neighbours are reachable.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/random/random.h>

#include <zigbee/mt.h>
#include <zigbee/zb_net.h>

#include "zb_net_priv.h"

/* Frame counters are written to flash this far ahead, so a restart never reuses one. */
#define COUNTER_STEP        1024U
#define APS_ACK_TIMEOUT_MS  3000
#define LINK_STATUS_MS      15000
#define DUP_MS              8000
#define TC_FIRST_DELAY_MS   30
#define TC_TRIES            3
/* The MAC payload that holds a NWK frame. */
#define MAC_PAYLOAD_MAX     (ZB_MAC_MAX_FRAME - 9)
/* Link status: incoming and outgoing cost 1. */
#define LINK_COST           0x11
#define LINK_STATUS_ENTRIES 31
#define LINK_FIRST_LAST     (BIT(5) | BIT(6))

/* The Zigbee default global trust center link key. */
static const uint8_t default_tc_link_key[ZB_KEY_LEN] = {'Z', 'i', 'g', 'B', 'e', 'e', 'A', 'l',
							'l', 'i', 'a', 'n', 'c', 'e', '0', '9'};

void zbn_trace(struct zb_net *n, const char *fmt, ...)
{
	char msg[96];
	va_list ap;

	if (n->cb.trace == NULL) {
		return;
	}
	va_start(ap, fmt);
	(void)vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	n->cb.trace(n->cb.ctx, msg);
}

/* ------------------------------------------------------------------------------------------ */
/* Keys and counters */

void zbn_security_load(struct zb_net *n)
{
	uint8_t kd[ZB_KEYDESC_LEN], sm[ZB_SECMAT_LEN], b[4];

	memcpy(n->tc_link_key, default_tc_link_key, ZB_KEY_LEN);
	n->keys_ok = false;
	if (znp_nv_read(n->nv, znp_nv_osal(ZB_NV_NWK_ACTIVE_KEY_INFO), 0, kd, sizeof(kd)) !=
	    (int)sizeof(kd)) {
		return;
	}
	n->nwk_key_seq = kd[0];
	memcpy(n->nwk_key, &kd[1], ZB_KEY_LEN);

	n->nwk_fc = 0;
	if (znp_nv_read(n->nv, zbn_exkey(ZB_NVEX_NWK_SEC_MATERIAL, 0), 0, sm, sizeof(sm)) ==
	    (int)sizeof(sm)) {
		n->nwk_fc = mt_get_u32(sm);
	}
	n->nwk_fc_limit = n->nwk_fc;

	if (znp_nv_len(n->nv, znp_nv_osal(ZB_NV_APS_FRAME_COUNTER)) != 4U) {
		memset(b, 0, sizeof(b));
		(void)znp_nv_set(n->nv, znp_nv_osal(ZB_NV_APS_FRAME_COUNTER), b, sizeof(b));
	}
	n->aps_fc = 0;
	if (znp_nv_read(n->nv, znp_nv_osal(ZB_NV_APS_FRAME_COUNTER), 0, b, sizeof(b)) == 4) {
		n->aps_fc = mt_get_u32(b);
	}
	n->aps_fc_limit = n->aps_fc;

	n->nwk_seq = (uint8_t)sys_rand32_get();
	n->aps_counter = (uint8_t)sys_rand32_get();
	n->next_link_status = k_uptime_get() + 5000;
	n->keys_ok = true;
}

/* The next frame counter; the flash is written (and flushed) before the old limit is passed. */
static int next_counter(struct zb_net *n, uint32_t *fc, uint32_t *limit, struct znp_nv_key key,
			uint32_t *out)
{
	int r = 0;

	k_mutex_lock(&n->lock, K_FOREVER);
	if (*fc >= *limit) {
		uint8_t b[4];
		uint32_t next = *fc + COUNTER_STEP;

		mt_put_u32(b, next);
		r = znp_nv_write(n->nv, key, 0, b, sizeof(b));
		if (r == 0) {
			r = znp_nv_flush(n->nv);
		}
		if (r == 0) {
			*limit = next;
		}
	}
	if (r == 0) {
		*out = (*fc)++;
	}
	k_mutex_unlock(&n->lock);
	return r;
}

static int next_nwk_fc(struct zb_net *n, uint32_t *out)
{
	return next_counter(n, &n->nwk_fc, &n->nwk_fc_limit, zbn_exkey(ZB_NVEX_NWK_SEC_MATERIAL, 0),
			    out);
}

static int next_aps_fc(struct zb_net *n, uint32_t *out)
{
	return next_counter(n, &n->aps_fc, &n->aps_fc_limit, znp_nv_osal(ZB_NV_APS_FRAME_COUNTER),
			    out);
}

static uint8_t take_nwk_seq(struct zb_net *n)
{
	uint8_t s;

	k_mutex_lock(&n->lock, K_FOREVER);
	s = n->nwk_seq++;
	k_mutex_unlock(&n->lock);
	return s;
}

/* ------------------------------------------------------------------------------------------ */
/* Sending */

/* A NWK frame with the given payload, secured with the network key, as one MAC data frame. */
static int nwk_send(struct zb_net *n, struct zb_nwk_hdr *h, const uint8_t *pl, size_t plen,
		    uint16_t mac_dst, bool ar)
{
	uint8_t f[MAC_PAYLOAD_MAX];
	size_t hl, tl;
	uint32_t fc;

	if (!n->keys_ok) {
		return -ENOTCONN;
	}
	h->security = true;
	h->proto_ver = 2;
	h->seq = take_nwk_seq(n);
	hl = zb_nwk_build(h, f);
	if (hl + plen > sizeof(f)) {
		return -EMSGSIZE;
	}
	memcpy(&f[hl], pl, plen);
	if (next_nwk_fc(n, &fc) != 0) {
		return -EIO;
	}
	tl = zb_sec_protect(f, sizeof(f), hl, plen, n->nwk_key, ZB_KEYID_NETWORK, n->nwk_key_seq, fc,
			    n->eui);
	if (tl == 0U) {
		return -EMSGSIZE;
	}
	return zb_mac_send_data(n->mac, mac_dst, f, tl, ar);
}

static uint8_t confirm_status(int r)
{
	switch (r) {
	case 0:
		return ZB_CONFIRM_OK;
	case -ENOMSG:
		return ZB_CONFIRM_MAC_NO_ACK;
	case -EAGAIN:
		return ZB_CONFIRM_CHANNEL_BUSY;
	default:
		return ZB_CONFIRM_FAILURE;
	}
}

static void confirm(struct zb_net *n, uint8_t status, uint32_t tag)
{
	if (n->cb.data_confirm != NULL) {
		n->cb.data_confirm(n->cb.ctx, status, tag);
	}
}

static struct zb_aps_wait *wait_alloc(struct zb_net *n)
{
	for (int i = 0; i < ZB_APS_WAITS; i++) {
		if (!n->waits[i].used) {
			return &n->waits[i];
		}
	}
	return NULL;
}

int zb_net_send(struct zb_net *n, const struct zb_aps_tx *tx)
{
	uint8_t pl[MAC_PAYLOAD_MAX];
	struct zb_aps_hdr ah = {
		.type = ZB_APS_DATA,
		.delivery = tx->broadcast ? ZB_APS_BROADCAST : ZB_APS_UNICAST,
		.ack_req = tx->ack_req && !tx->broadcast,
		.dst_ep = tx->dst_ep,
		.src_ep = tx->src_ep,
		.cluster = tx->cluster,
		.profile = tx->profile,
	};
	struct zb_nwk_hdr nh = {.type = ZB_NWK_DATA, .dst = tx->dst, .src = 0x0000,
				.radius = ZB_NWK_RADIUS};
	struct zb_aps_wait *w = NULL;
	size_t o;
	int r;

	if (!n->formed || !n->keys_ok) {
		return -ENOTCONN;
	}
	if (tx->len > ZB_APS_MAX_PAYLOAD) {
		return -EMSGSIZE;
	}
	k_mutex_lock(&n->lock, K_FOREVER);
	ah.counter = n->aps_counter++;
	if (ah.ack_req) {
		w = wait_alloc(n);
		if (w != NULL) {
			*w = (struct zb_aps_wait){.used = true, .dst = tx->dst, .counter = ah.counter,
						  .tag = tx->tag,
						  .deadline = k_uptime_get() + APS_ACK_TIMEOUT_MS};
		}
	}
	k_mutex_unlock(&n->lock);
	if (ah.ack_req && w == NULL) {
		return -ENOMEM;
	}

	o = zb_aps_build(&ah, pl);
	memcpy(&pl[o], tx->data, tx->len);
	r = nwk_send(n, &nh, pl, o + tx->len, tx->broadcast ? ZB_SHORT_BROADCAST : tx->dst,
		     !tx->broadcast);
	if (r != 0) {
		if (w != NULL) {
			k_mutex_lock(&n->lock, K_FOREVER);
			w->used = false;
			k_mutex_unlock(&n->lock);
		}
		if (tx->confirm) {
			confirm(n, confirm_status(r), tx->tag);
		}
		return 0;
	}
	if (w == NULL && tx->confirm) {
		confirm(n, ZB_CONFIRM_OK, tx->tag);
	}
	return 0;
}

static void send_aps_ack(struct zb_net *n, const struct zb_child *c, const struct zb_aps_hdr *in)
{
	struct zb_aps_hdr ah = {
		.type = ZB_APS_ACK,
		.dst_ep = in->src_ep,
		.src_ep = in->dst_ep,
		.cluster = in->cluster,
		.profile = in->profile,
		.counter = in->counter,
	};
	struct zb_nwk_hdr nh = {.type = ZB_NWK_DATA, .dst = c->nwk, .src = 0x0000,
				.radius = ZB_NWK_RADIUS};
	uint8_t pl[16];
	size_t o = zb_aps_build(&ah, pl);

	(void)nwk_send(n, &nh, pl, o, c->nwk, true);
}

/* ------------------------------------------------------------------------------------------ */
/* Link status and rejoin */

static void send_link_status(struct zb_net *n)
{
	struct zb_nwk_hdr nh = {.type = ZB_NWK_COMMAND, .dst = ZB_BCAST_ROUTERS, .src = 0x0000,
				.radius = 1};
	uint8_t pl[2 + 3 * LINK_STATUS_ENTRIES];
	size_t o = 2;
	uint8_t count = 0;

	pl[0] = ZB_NWKC_LINK_STATUS;
	for (int i = 0; i < ZB_NET_MAX_CHILDREN && count < LINK_STATUS_ENTRIES; i++) {
		const struct zb_child *c = &n->children[i];

		if (c->used && c->authorized) {
			mt_put_u16(&pl[o], c->nwk);
			pl[o + 2] = LINK_COST;
			o += 3;
			count++;
		}
	}
	pl[1] = (uint8_t)(count | LINK_FIRST_LAST);
	(void)nwk_send(n, &nh, pl, o, ZB_SHORT_BROADCAST, false);
}

static void handle_rejoin(struct zb_net *n, struct zb_child *c)
{
	struct zb_nwk_hdr nh = {.type = ZB_NWK_COMMAND, .dst = c->nwk, .src = 0x0000, .radius = 1};
	uint8_t pl[4] = {ZB_NWKC_REJOIN_RSP};

	mt_put_u16(&pl[1], c->nwk);
	pl[3] = 0; /* success: the device keeps its address */
	(void)nwk_send(n, &nh, pl, sizeof(pl), c->nwk, true);
}

/* ------------------------------------------------------------------------------------------ */
/* Receiving */

static bool dup_seen(struct zb_net *n, uint16_t src, uint8_t counter)
{
	int64_t now = k_uptime_get();
	int free_slot = -1;

	for (int i = 0; i < ZB_APS_DUPS; i++) {
		if (n->dups[i].until > now && n->dups[i].src == src && n->dups[i].counter == counter) {
			return true;
		}
		if (n->dups[i].until <= now) {
			free_slot = i;
		}
	}
	if (free_slot < 0) {
		free_slot = (int)(now % ZB_APS_DUPS);
	}
	n->dups[free_slot] = (struct zb_aps_dup){.src = src, .counter = counter, .until = now + DUP_MS};
	return false;
}

static void aps_rx(struct zb_net *n, struct zb_child *c, const struct zb_nwk_hdr *nh,
		   const uint8_t *p, size_t len, uint8_t lqi)
{
	struct zb_aps_hdr ah;
	bool bcast = nh->dst >= ZB_BCAST_ROUTERS;

	if (zb_aps_parse(p, len, &ah) != 0 || ah.ext_hdr) {
		zbn_trace(n, "aps: %04x unparsable", c->nwk);
		return;
	}
	zbn_trace(n, "aps: %04x type %u ep %u->%u cluster %04x profile %04x counter %u len %u",
		  c->nwk, ah.type, ah.src_ep, ah.dst_ep, ah.cluster, ah.profile, ah.counter,
		  (unsigned int)(len - ah.len));
	switch (ah.type) {
	case ZB_APS_ACK:
		for (int i = 0; i < ZB_APS_WAITS; i++) {
			struct zb_aps_wait *w = &n->waits[i];

			if (w->used && w->dst == c->nwk && w->counter == ah.counter) {
				uint32_t tag = w->tag;

				k_mutex_lock(&n->lock, K_FOREVER);
				w->used = false;
				k_mutex_unlock(&n->lock);
				confirm(n, ZB_CONFIRM_OK, tag);
				return;
			}
		}
		return;
	case ZB_APS_DATA:
		break;
	default:
		return; /* APS commands (key requests and the like) are not served yet */
	}
	if (ah.security) {
		return; /* APS link key security is not applied to data here */
	}
	if (ah.ack_req && ah.delivery == ZB_APS_UNICAST && !bcast) {
		send_aps_ack(n, c, &ah);
	}
	if (dup_seen(n, c->nwk, ah.counter)) {
		return;
	}
	if (n->cb.aps_rx != NULL) {
		struct zb_aps_rx rx = {
			.src = c->nwk,
			.cluster = ah.cluster,
			.profile = ah.profile,
			.src_ep = ah.src_ep,
			.dst_ep = ah.dst_ep,
			.broadcast = bcast || ah.delivery == ZB_APS_BROADCAST,
			.lqi = lqi,
			.aps_counter = ah.counter,
			.data = &p[ah.len],
			.len = (uint8_t)(len - ah.len),
		};

		n->cb.aps_rx(n->cb.ctx, &rx);
	}
}

void zbn_rx(struct zb_net *n, const struct zb_mac_hdr *mh, const uint8_t *payload, size_t len,
	    uint8_t lqi)
{
	uint8_t f[ZB_MAC_MAX_FRAME];
	struct zb_nwk_hdr nh;
	struct zb_sec_hdr sh;
	struct zb_child *c;
	size_t plen;

	ARG_UNUSED(mh);
	if (!n->formed || !n->keys_ok || len > sizeof(f)) {
		return;
	}
	memcpy(f, payload, len);
	if (zb_nwk_parse(f, len, &nh) != 0 || nh.proto_ver != 2U || nh.type > ZB_NWK_COMMAND) {
		zbn_trace(n, "nwk: not a PRO frame (%u bytes)", (unsigned int)len);
		return;
	}
	/* Without routing, only frames for this device or broadcasts are of use. */
	if (nh.dst != 0x0000 && nh.dst < ZB_BCAST_ROUTERS) {
		return;
	}
	if (!nh.security || zb_sec_parse(f, len, nh.len, &sh) != 0 ||
	    sh.key_id != ZB_KEYID_NETWORK || sh.key_seq != n->nwk_key_seq) {
		zbn_trace(n, "nwk: %04x -> %04x unsecured or other key (type %u)", nh.src, nh.dst, nh.type);
		return;
	}
	c = zbn_child_by_ext(n, sh.src);
	if (c == NULL || !c->authorized || c->nwk != nh.src) {
		zbn_trace(n, "nwk: %04x not a joined neighbour", nh.src);
		return;
	}
	if (c->rx_fc != 0U && sh.counter <= c->rx_fc) {
		zbn_trace(n, "nwk: %04x replay %u <= %u", nh.src, (unsigned int)sh.counter,
			  (unsigned int)c->rx_fc);
		return; /* a replay, or a counter that went back */
	}
	if (zb_sec_open(f, len, nh.len, n->nwk_key, &sh, &plen) != 0) {
		zbn_trace(n, "nwk: %04x MIC failed (counter %u)", nh.src, (unsigned int)sh.counter);
		return;
	}
	c->rx_fc = sh.counter;
	c->last_seen = k_uptime_get();

	if (nh.type == ZB_NWK_COMMAND) {
		uint8_t cmd = plen >= 1U ? f[nh.len + sh.len] : 0U;

		zbn_trace(n, "nwk: %04x command %02x", nh.src, cmd);
		if (cmd == ZB_NWKC_REJOIN_REQ) {
			handle_rejoin(n, c);
		}
		/* Link status and the rest only keep the neighbour alive for now. */
		return;
	}
	aps_rx(n, c, &nh, &f[nh.len + sh.len], plen, lqi);
}

/* ------------------------------------------------------------------------------------------ */
/* Trust center */

/* APS Transport-Key (standard network key) to a joiner, secured with the key-transport key. */
static int tc_send_key(struct zb_net *n, const struct zb_child *c)
{
	struct zb_nwk_hdr nh = {.type = ZB_NWK_DATA, .proto_ver = 2, .dst = c->nwk, .src = 0x0000,
				.radius = 1};
	uint8_t f[MAC_PAYLOAD_MAX], ktk[ZB_KEY_LEN];
	size_t o, tl;
	uint32_t fc;

	if (!n->keys_ok) {
		return -ENOTCONN;
	}
	nh.seq = take_nwk_seq(n);
	o = zb_nwk_build(&nh, f); /* the NWK header: not secured, the joiner has no key yet */
	k_mutex_lock(&n->lock, K_FOREVER);
	f[o] = 0x21; /* APS command frame with security */
	f[o + 1] = n->aps_counter++;
	k_mutex_unlock(&n->lock);
	zb_aps_transport_key_payload(&f[o + 2], n->nwk_key, n->nwk_key_seq, c->ext, n->eui);
	zb_key_transport_key(n->tc_link_key, ktk);
	if (next_aps_fc(n, &fc) != 0) {
		return -EIO;
	}
	tl = zb_sec_protect(&f[o], sizeof(f) - o, 2, 35, ktk, ZB_KEYID_TRANSPORT, 0, fc, n->eui);
	if (tl == 0U) {
		return -EMSGSIZE;
	}
	return zb_mac_send_data(n->mac, c->nwk, f, o + tl, true);
}

void zbn_tc_join(struct zb_net *n, int idx)
{
	struct zb_child *c = &n->children[idx];
	int r = -EIO;

	/* The joiner has just ACKed its association response: give it a moment to listen. */
	k_msleep(TC_FIRST_DELAY_MS);
	for (int t = 0; t < TC_TRIES && r != 0; t++) {
		r = tc_send_key(n, c);
		if (r != 0) {
			k_msleep(100);
		}
	}
	zbn_trace(n, "tc: network key to %04x: %s (%d)", c->nwk, r == 0 ? "sent" : "FAILED", r);
	if (r != 0) {
		return;
	}
	k_mutex_lock(&n->lock, K_FOREVER);
	c->authorized = true;
	c->rx_fc = 0;
	c->last_seen = k_uptime_get();
	zbn_child_persist(n, idx);
	k_mutex_unlock(&n->lock);
	if (n->cb.tc_join != NULL) {
		n->cb.tc_join(n->cb.ctx, c->nwk, c->ext, 0x0000);
	}
}

/* ------------------------------------------------------------------------------------------ */
/* Timers */

void zbn_tick(struct zb_net *n)
{
	int64_t now = k_uptime_get();

	if (!n->formed || !n->keys_ok) {
		return;
	}
	if (now >= n->next_link_status) {
		n->next_link_status = now + LINK_STATUS_MS;
		send_link_status(n);
	}
	for (int i = 0; i < ZB_APS_WAITS; i++) {
		struct zb_aps_wait *w = &n->waits[i];

		if (w->used && now >= w->deadline) {
			uint32_t tag = w->tag;

			k_mutex_lock(&n->lock, K_FOREVER);
			w->used = false;
			k_mutex_unlock(&n->lock);
			confirm(n, ZB_CONFIRM_APS_NO_ACK, tag);
		}
	}
}
