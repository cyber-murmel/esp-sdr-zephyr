/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "ctlproto.h"

#define ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))
#define SIPROUND                                                                                   \
	do {                                                                                       \
		v0 += v1; v1 = ROTL(v1, 13); v1 ^= v0; v0 = ROTL(v0, 32);                          \
		v2 += v3; v3 = ROTL(v3, 16); v3 ^= v2;                                             \
		v0 += v3; v3 = ROTL(v3, 21); v3 ^= v0;                                             \
		v2 += v1; v1 = ROTL(v1, 17); v1 ^= v2; v2 = ROTL(v2, 32);                          \
	} while (0)

static uint64_t le64(const uint8_t *p)
{
	uint64_t v = 0;

	for (int k = 7; k >= 0; k--) {
		v = (v << 8) | p[k];
	}
	return v;
}

uint64_t siphash24(const uint8_t key[16], const uint8_t *in, size_t len)
{
	uint64_t k0 = le64(key), k1 = le64(key + 8);
	uint64_t v0 = 0x736f6d6570736575ULL ^ k0, v1 = 0x646f72616e646f6dULL ^ k1;
	uint64_t v2 = 0x6c7967656e657261ULL ^ k0, v3 = 0x7465646279746573ULL ^ k1;
	uint64_t b = (uint64_t)len << 56;
	size_t n = len & ~(size_t)7;

	for (size_t i = 0; i < n; i += 8) {
		uint64_t m = le64(in + i);

		v3 ^= m;
		SIPROUND;
		SIPROUND;
		v0 ^= m;
	}
	for (size_t i = 0; i < (len & 7U); i++) {
		b |= (uint64_t)in[n + i] << (8U * i);
	}
	v3 ^= b;
	SIPROUND;
	SIPROUND;
	v0 ^= b;
	v2 ^= 0xff;
	SIPROUND;
	SIPROUND;
	SIPROUND;
	SIPROUND;
	return v0 ^ v1 ^ v2 ^ v3;
}

void ctl_proto_init(struct ctl_proto *c, const uint8_t key[16], uint32_t ifindex, int64_t silent_ms)
{
	memset(c, 0, sizeof(*c));
	memcpy(c->key, key, 16);
	c->ifindex = ifindex;
	c->silent_ms = silent_ms;
}

void ctl_proto_cookie(const struct ctl_proto *c, const struct peer *p, uint32_t epoch, uint8_t out[8])
{
	uint8_t in[49] = {0};
	uint64_t h;

	in[0] = p->v6 ? 6 : 4;
	memcpy(in + 1, p->addr, p->v6 ? 16 : 4);
	in[17] = (uint8_t)(p->port >> 8);
	in[18] = (uint8_t)p->port;
	for (int k = 0; k < 4; k++) {
		in[19 + k] = (uint8_t)(c->ifindex >> (8 * k));
		in[23 + k] = (uint8_t)(epoch >> (8 * k));
	}
	/* the MAC the stream will go to and the address it will come from: the cookie that proves
	 * a host can receive is for these, so a renewal from another MAC is a stranger's
	 */
	memcpy(in + 27, p->mac, 6);
	memcpy(in + 33, p->local, 16);
	h = siphash24(c->key, in, sizeof(in));
	for (int k = 0; k < 8; k++) {
		out[k] = (uint8_t)(h >> (8 * k));
	}
}

bool ctl_peer_same(const struct peer *a, const struct peer *b)
{
	/* the MAC and the board address are part of who a subscriber is: two hosts may claim one IP, and
	 * a renewal to another address of the board is not the same stream
	 */
	return a->v6 == b->v6 && a->port == b->port && memcmp(a->mac, b->mac, 6) == 0 &&
	       memcmp(a->addr, b->addr, a->v6 ? 16 : 4) == 0 &&
	       memcmp(a->local, b->local, a->v6 ? 16 : 4) == 0;
}

static bool limit(int64_t *win, uint32_t *cnt, int64_t now, uint32_t max)
{
	if (now - *win >= 1000) {
		*win = now;
		*cnt = 0;
	}
	return ++*cnt <= max;
}

static struct ctl_src *src_get(struct ctl_proto *c, const struct peer *p, int64_t now)
{
	struct ctl_src *lru = &c->src[0];

	for (int k = 0; k < CTL_NSRC; k++) {
		struct ctl_src *s = &c->src[k];

		if (s->used && s->v6 == p->v6 && memcmp(s->addr, p->addr, p->v6 ? 16 : 4) == 0) {
			s->seen = now;
			return s;
		}
		if (!s->used || s->seen < lru->seen) {
			lru = s;
		}
	}
	memset(lru, 0, sizeof(*lru));
	lru->used = true;
	lru->v6 = p->v6;
	memcpy(lru->addr, p->addr, p->v6 ? 16 : 4);
	lru->win = lru->seen = now;
	return lru;
}

static bool src_limit(struct ctl_src *s, int64_t now, uint32_t *cnt, uint32_t max)
{
	if (now - s->win >= 1000) {
		s->win = now;
		s->n_adm = s->n_c = s->n_q = 0;
	}
	return ++*cnt <= max;
}

static bool is_sub(const struct ctl_proto *c, const struct peer *p)
{
	return c->live && ctl_peer_same(&c->sub, p);
}

/* The counters of this source: the subscriber's own for the full match, else the table's (by IP). */
static struct ctl_src *budget(struct ctl_proto *c, const struct peer *p, int64_t now)
{
	if (is_sub(c, p)) {
		if (!c->subs.used) {
			c->subs.used = true;
			c->subs.win = now;
		}
		return &c->subs;
	}
	return src_get(c, p, now);
}

bool ctl_proto_admit(struct ctl_proto *c, const struct peer *p, int64_t now)
{
	struct ctl_src *s = budget(c, p, now);
	bool sub = is_sub(c, p);

	if (!src_limit(s, now, &s->n_adm, sub ? 60U : 20U)) {
		return false;
	}
	return sub || limit(&c->awin, &c->acnt, now, 300U);
}

static bool cookie_ok(const struct ctl_proto *c, const struct peer *p, const uint8_t *ck, int64_t now)
{
	uint32_t epoch = (uint32_t)(now / CTL_EPOCH_MS);
	uint8_t want[8];

	for (uint32_t e = epoch; ; e = epoch - 1U) {
		ctl_proto_cookie(c, p, e, want);
		if (memcmp(want, ck, 8) == 0) {
			return true;
		}
		if (e != epoch || epoch == 0U) {
			return false;
		}
	}
}

static void reply_cookie(struct ctl_proto *c, const struct peer *p, int64_t now, struct ctl_out *o)
{
	/* 5 a second to a source, 100 a second in all: the subscriber's refresh is not in the 100 */
	struct ctl_src *s = budget(c, p, now);

	if (!src_limit(s, now, &s->n_c, 5U) || (!is_sub(c, p) && !limit(&c->gwin, &c->gcnt, now, 100U))) {
		return;
	}
	memcpy(o->reply, "VRTC", 4);
	ctl_proto_cookie(c, p, (uint32_t)(now / CTL_EPOCH_MS), o->reply + 4);
	o->reply_len = 12;
	o->act = CTL_REPLY;
}

void ctl_proto_dgram(struct ctl_proto *c, const struct peer *p, const uint8_t *d, size_t len,
		     int64_t now, struct ctl_out *o)
{
	memset(o, 0, sizeof(*o));
	o->act = CTL_IGNORE;
	if (len == 4U && memcmp(d, "VRTS", 4) == 0) {
		reply_cookie(c, p, now, o);
	} else if ((len == 12U || len == 13U) &&
		   (memcmp(d, "VRTS", 4) == 0 || memcmp(d, "VRTT", 4) == 0 || memcmp(d, "VRTX", 4) == 0)) {
		bool sub = memcmp(d, "VRTS", 4) == 0, take = memcmp(d, "VRTT", 4) == 0;

		if (!cookie_ok(c, p, d + 4, now)) {
			/* a stale cookie (or a forged one): a fresh one for those who can read it */
			if (sub) {
				reply_cookie(c, p, now, o);
			}
		} else if (c->blocked) {
			/* nothing */
		} else if (!sub && !take) {
			if (c->live && ctl_peer_same(&c->sub, p)) {
				c->live = false;
				o->evt = CTL_EVT_STOPPED;
			}
		} else if (c->live && !ctl_peer_same(&c->sub, p) && !take) {
			memcpy(o->reply, "VRTB", 4);
			o->reply_len = 4;
			o->act = CTL_REPLY;
		} else {
			bool fresh = !c->live || !ctl_peer_same(&c->sub, p);

			o->nocsum = len == 13U && (d[12] & 1U) != 0U;
			o->evt = !fresh ? CTL_EVT_RENEWED : (c->live ? CTL_EVT_TAKEOVER : CTL_EVT_SUBSCRIBED);
			if (fresh) {
				memset(&c->subs, 0, sizeof(c->subs));
			}
			c->sub = *p;
			c->live = true;
			o->nocsum = len == 13U && (d[12] & 1U) != 0U;
			c->sub.nocsum = o->nocsum;
			c->last_ms = now;
			if (fresh) {
				memcpy(o->reply, "VRTK", 4);
				o->reply_len = 4;
				o->act = CTL_REPLY;
			}
		}
	} else if (len >= 8U && (d[0] >> 4) == 6U && len % 4U == 0U) {
		o->act = CTL_CMD;
		o->is_sub = is_sub(c, p);
	}
}

bool ctl_proto_expire(struct ctl_proto *c, int64_t now)
{
	if (c->live && now - c->last_ms > c->silent_ms) {
		c->live = false;
		return true;
	}
	return false;
}

bool ctl_proto_query_ok(struct ctl_proto *c, const struct peer *p, int64_t now)
{
	struct ctl_src *s = budget(c, p, now);

	return src_limit(s, now, &s->n_q, 2U) && limit(&c->qwin, &c->qcnt, now, 10U);
}
