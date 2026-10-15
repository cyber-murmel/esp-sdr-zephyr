/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * BLE connection following state machine, see le_conn.h. The model: event ev has a
 * predicted master anchor `anchor`; the sniffer listens in [anchor - w, anchor + spread + w]
 * where w grows with the time since the master's timing was last observed (the sleep
 * clock accuracy of the CONNECT_REQ plus our own), and `spread` is the transmit window of
 * the first event or of a connection update instant. The first packet of an event is the
 * master's; a pair of packets 150 us apart proves it.
 *
 * The logic is ported from the Project Ubertooth firmware (bluetooth_rxtx.c, Copyright
 * 2010-2013 Michael Ossmann and 2011-2013 Dominic Spill; le_phy.c, Copyright 2017 Mike Ryan),
 * released under the GPL version 2 or later.
 */

#include <string.h>

#include "le_conn.h"

#define ADV_AA       0x8E89BED6U
#define ADV_CRC_INIT 0x555555U

#define CONNECT_REQ_TYPE 5U
#define CONNECT_REQ_LEN  34U

/* The master's sleep clock accuracy field of the CONNECT_REQ, in ppm. */
static const uint16_t sca_ppm[8] = {500, 250, 150, 100, 75, 50, 30, 20};

static inline unsigned int le16(const uint8_t *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static inline uint32_t le24(const uint8_t *p)
{
	return (uint32_t)le16(p) | ((uint32_t)p[2] << 16);
}

static inline uint32_t le32(const uint8_t *p)
{
	return le24(p) | ((uint32_t)p[3] << 24);
}

static uint16_t chan_mhz(unsigned int idx)
{
	if (idx < 11U) {
		return (uint16_t)(2404U + 2U * idx);
	}
	if (idx < 37U) {
		return (uint16_t)(2428U + 2U * (idx - 11U));
	}
	return idx == 37U ? 2402U : (idx == 38U ? 2426U : 2480U);
}

/* ---- channel map and selection ---- */

static void chm_set(struct le_conn_chm *m, const uint8_t *bytes)
{
	unsigned int i, j;

	memcpy(m->map, bytes, 5);
	m->map[4] &= 0x1fU; /* channels 37 to 39 do not exist in the map */
	m->n = 0;
	for (i = 0; i < 5U; i++) {
		for (j = 0; j < 8U; j++) {
			m->n = (uint8_t)(m->n + ((m->map[i] >> j) & 1U));
		}
	}
}

static inline bool chm_used(const struct le_conn_chm *m, unsigned int ch)
{
	return ((m->map[ch >> 3] >> (ch & 7U)) & 1U) != 0U;
}

/* The k-th (from 0) channel in use. */
static unsigned int chm_nth(const struct le_conn_chm *m, unsigned int k)
{
	unsigned int ch;

	for (ch = 0; ch < 37U; ch++) {
		if (chm_used(m, ch)) {
			if (k == 0U) {
				return ch;
			}
			k--;
		}
	}
	return 0;
}

static uint16_t csa2_perm(uint16_t x)
{
	x = (uint16_t)(((x & 0xf0f0U) >> 4) | ((x & 0x0f0fU) << 4));
	x = (uint16_t)(((x & 0xccccU) >> 2) | ((x & 0x3333U) << 2));
	x = (uint16_t)(((x & 0xaaaaU) >> 1) | ((x & 0x5555U) << 1));
	return x;
}

uint16_t le_conn_csa2_prn(uint16_t counter, uint32_t aa)
{
	uint16_t cid = (uint16_t)((aa >> 16) ^ aa);
	uint16_t p = counter ^ cid;
	unsigned int i;

	for (i = 0; i < 3U; i++) {
		p = csa2_perm(p);
		p = (uint16_t)(17U * p + cid);
	}
	return p ^ cid;
}

unsigned int le_conn_chan_idx(const struct le_conn *c, uint32_t ev)
{
	unsigned int u;

	if (c->csa2) {
		uint16_t prn = le_conn_csa2_prn((uint16_t)ev, c->aa);

		u = prn % 37U;
		if (chm_used(&c->chm, u)) {
			return u;
		}
		return chm_nth(&c->chm, ((uint32_t)c->chm.n * prn) >> 16);
	}
	u = (unsigned int)((c->unmapped0 + (uint64_t)(uint32_t)(ev - c->ev0) * c->hop) % 37U);
	if (chm_used(&c->chm, u)) {
		return u;
	}
	return chm_nth(&c->chm, u % c->chm.n);
}

/* ---- actions ---- */

static struct le_conn_action act_adv(struct le_conn *c, enum le_conn_reason reason)
{
	struct le_conn_action a;

	memset(&a, 0, sizeof(a));
	c->state = LE_CONN_ADV;
	c->cur_mhz = c->cfg.adv_mhz;
	a.act = LE_ACT_ADV;
	a.reason = (uint8_t)reason;
	a.mhz = c->cfg.adv_mhz;
	a.aa = ADV_AA;
	a.crc_init = ADV_CRC_INIT;
	return a;
}

static inline uint64_t max64(uint64_t a, uint64_t b)
{
	return a > b ? a : b;
}

/* Listening window of the planned event: the packet start lies in [*lo, *hi]. */
static void window(const struct le_conn *c, uint64_t *lo, uint64_t *hi, uint64_t *ww_out)
{
	uint64_t elapsed = c->anchor > c->ref_t ? c->anchor - c->ref_t : 0U;
	uint64_t ww = elapsed * c->ppm / 1000000U + c->cfg.jitter_ticks;
	uint64_t half = c->interval / 2U;
	uint64_t cap = half > 20000U ? half - 10000U : half / 2U; /* stay clear of the next event */
	uint64_t early;

	if (ww > cap) {
		ww = max64(cap, c->cfg.jitter_ticks);
	}
	early = ww + c->cfg.pre_ticks;
	if (c->anchored && !c->verified) {
		early += c->cfg.guard_ticks; /* the anchor may be the slave's packet */
	}
	*lo = c->anchor > early ? c->anchor - early : 0U;
	*hi = c->anchor + c->spread + ww;
	if (ww_out != NULL) {
		*ww_out = ww;
	}
}

/* Move the plan to the next event, applying the instants that fall on it. */
static void advance(struct le_conn *c)
{
	uint32_t next = c->ev + 1U;

	if (c->upd_pending && c->counter_known && (uint16_t)next == c->upd_instant) {
		c->anchor += c->interval + c->upd_offset;
		c->interval = c->upd_interval;
		c->timeout = c->upd_timeout;
		c->spread = c->upd_size;
		c->anchored = false;
		c->upd_pending = false;
		c->stats.updates++;
	} else {
		c->anchor += c->interval;
	}
	if (c->chm_pending && c->counter_known && (uint16_t)next == c->chm_instant) {
		c->chm = c->chm_new;
		c->chm_pending = false;
		c->stats.chm_updates++;
	}
	if (c->phy_pending && c->counter_known && (uint16_t)next == c->phy_instant) {
		c->phy_pending = false;
		c->phy_lost = true;
	}
	c->ev = next;
}

/* Plan the radio for the planned event, skipping events the retune cannot reach. */
static struct le_conn_action plan(struct le_conn *c, uint64_t now)
{
	uint64_t free_t = max64(now, c->radio_free);
	uint32_t skipped = 0;
	struct le_conn_action a;

	for (;;) {
		unsigned int idx;

		if (c->phy_lost) {
			return act_adv(c, LE_REASON_PHY);
		}
		idx = le_conn_chan_idx(c, c->ev);
		uint16_t mhz = chan_mhz(idx);
		uint64_t lo, hi;
		bool same = mhz == c->cur_mhz;

		window(c, &lo, &hi, NULL);
		if (free_t + (same ? 0U : c->cfg.retune_ticks) <= lo) {
			memset(&a, 0, sizeof(a));
			a.act = LE_ACT_TUNE;
			a.mhz = mhz;
			a.aa = c->aa;
			a.crc_init = c->crc_init;
			a.event = (uint16_t)c->ev;
			a.skipped = skipped;
			a.t_listen = lo;
			a.t_retune = same ? lo : max64(free_t, lo - c->cfg.retune_ticks);
			a.t_end = hi + c->cfg.detect_ticks + c->cfg.slave_ticks;
			c->deadline = a.t_end;
			c->cur_mhz = mhz;
			c->npk = 0;
			c->stats.planned++;
			return a;
		}
		if (++skipped > c->cfg.max_skip) {
			return act_adv(c, LE_REASON_TOO_FAST);
		}
		c->stats.skipped++;
		advance(c);
	}
}

/* ---- events ---- */

static bool gap_ok(uint64_t end_prev, uint64_t t_next)
{
	uint64_t gap = t_next > end_prev ? t_next - end_prev : 0U;

	return gap + 150U >= LE_T_IFS_TICKS && gap <= LE_T_IFS_TICKS + 150U;
}

/* The planned event is over: take its anchor, check the supervision timer, plan the next. */
static struct le_conn_action close_event(struct le_conn *c, uint64_t now)
{
	if (c->npk > 0U) {
		uint64_t lo, hi;
		bool pair = c->npk >= 2U && gap_ok(c->pk[0].end, c->pk[1].t);
		bool master;

		window(c, &lo, &hi, NULL);
		master = pair || (c->pk[0].t >= lo && c->pk[0].t <= hi);
		if (master) {
			c->anchor = c->pk[0].t;
			c->ref_t = c->pk[0].t;
			c->spread = 0;
			if (!c->anchored) {
				c->anchored = true;
				c->verified = pair;
			}
			c->state = LE_CONN_TRACK;
		}
		if (pair) {
			c->verified = true;
		}
		c->stats.followed++;
	} else {
		c->stats.missed++;
	}
	c->radio_free = now;
	if (c->terminating) {
		return act_adv(c, LE_REASON_TERMINATE);
	}
	if (c->state == LE_CONN_FIRST ? now > c->first_deadline
				      : now > c->last_valid + c->timeout) {
		return act_adv(c, LE_REASON_TIMEOUT);
	}
	advance(c);
	return plan(c, now);
}

/* LL control PDUs that move the schedule. */
static void control_pdu(struct le_conn *c, const struct le_conn_pkt *p)
{
	const uint8_t *d = p->pdu;
	uint8_t len = d[1];

	if ((d[0] & 0x03U) != 0x03U || len == 0U || c->encrypted || p->len < 3U) {
		return;
	}
	switch (d[2]) {
	case 0x00: /* LL_CONNECTION_UPDATE_IND */
		if (len == 12U && p->len >= 14U && c->counter_known) {
			unsigned int interval = le16(&d[6]), timeout = le16(&d[10]);
			uint16_t instant = (uint16_t)le16(&d[12]);
			uint16_t ahead = (uint16_t)(instant - (uint16_t)c->ev);

			if (interval >= 6U && interval <= 3200U && timeout >= 10U &&
			    timeout <= 3200U && le16(&d[4]) <= interval && ahead >= 1U &&
			    ahead < 0x8000U) {
				c->upd_size = (uint64_t)d[3] * LE_TICKS_PER_UNIT;
				c->upd_offset = (uint64_t)le16(&d[4]) * LE_TICKS_PER_UNIT;
				c->upd_interval = (uint64_t)interval * LE_TICKS_PER_UNIT;
				c->upd_timeout = (uint64_t)timeout * LE_TICKS_PER_10MS;
				c->upd_instant = instant;
				c->upd_pending = true;
			}
		}
		break;
	case 0x01: /* LL_CHANNEL_MAP_IND */
		if (len == 8U && p->len >= 10U && c->counter_known) {
			uint16_t instant = (uint16_t)le16(&d[8]);
			uint16_t ahead = (uint16_t)(instant - (uint16_t)c->ev);
			struct le_conn_chm m;

			chm_set(&m, &d[3]);
			if (m.n >= 2U && ahead >= 1U && ahead < 0x8000U) {
				c->chm_new = m;
				c->chm_instant = instant;
				c->chm_pending = true;
			}
		}
		break;
	case 0x02: /* LL_TERMINATE_IND */
		if (len == 2U) {
			c->terminating = true;
		}
		break;
	case 0x05: /* LL_START_ENC_REQ: everything after is encrypted */
		c->encrypted = true;
		break;
	case 0x18: /* LL_PHY_UPDATE_IND: LE 2M and Coded cannot be followed (0: no change, 1: 1M) */
		if (len == 5U && p->len >= 7U && c->counter_known && (d[3] > 1U || d[4] > 1U)) {
			uint16_t instant = (uint16_t)le16(&d[5]);
			uint16_t ahead = (uint16_t)(instant - (uint16_t)c->ev);

			if (ahead >= 1U && ahead < 0x8000U) {
				c->phy_instant = instant;
				c->phy_pending = true;
			}
		}
		break;
	default:
		break;
	}
}

static struct le_conn_action start_connect(struct le_conn *c, const struct le_conn_pkt *p,
					   uint64_t now)
{
	const uint8_t *d = p->pdu;
	unsigned int win_size = d[21], win_offset = le16(&d[22]), interval = le16(&d[24]);
	unsigned int latency = le16(&d[26]), timeout = le16(&d[28]), hop = d[35] & 0x1fU;
	struct le_conn_chm m;
	uint64_t connect_end = p->t + LE_PKT_TICKS(CONNECT_REQ_LEN);

	chm_set(&m, &d[30]);
	/* the limits of Core Vol 6 Part B 2.3.3.1 */
	if (interval < 6U || interval > 3200U || win_offset > interval || win_size < 1U ||
	    win_size > 8U || win_size > interval - 1U || timeout < 10U || timeout > 3200U ||
	    latency > 499U || m.n < 2U || hop == 0U) {
		return act_adv(c, LE_REASON_BAD_PARAMS);
	}
	memset(&c->chm_new, 0, sizeof(c->chm_new));
	c->csa2 = (d[0] & 0x20U) != 0U;
	c->aa = le32(&d[14]);
	c->crc_init = le24(&d[18]);
	c->hop = (uint8_t)hop;
	c->ev0 = 0;
	c->unmapped0 = (uint8_t)(hop % 37U);
	c->chm = m;
	c->chm_pending = false;
	c->upd_pending = false;
	c->terminating = false;
	c->encrypted = false;
	c->phy_pending = false;
	c->phy_lost = false;
	c->counter_known = true;
	c->ppm = sca_ppm[d[35] >> 5] + (uint32_t)c->cfg.own_ppm;
	c->interval = (uint64_t)interval * LE_TICKS_PER_UNIT;
	c->timeout = (uint64_t)timeout * LE_TICKS_PER_10MS;
	c->spread = (uint64_t)win_size * LE_TICKS_PER_UNIT;
	/* transmitWindowDelay 1.25 ms after the CONNECT_REQ, then WinOffset */
	c->anchor = connect_end + LE_TICKS_PER_UNIT + (uint64_t)win_offset * LE_TICKS_PER_UNIT;
	c->ref_t = connect_end;
	c->last_valid = connect_end;
	c->first_deadline = c->anchor + c->spread + 6U * c->interval;
	c->anchored = false;
	c->verified = false;
	c->ev = 0;
	c->radio_free = max64(now, connect_end);
	c->cur_mhz = p->mhz;
	c->state = LE_CONN_FIRST;
	c->stats.connects++;
	return plan(c, now);
}

/* ---- API ---- */

void le_conn_cfg_default(struct le_conn_cfg *cfg)
{
	cfg->retune_ticks = 40U * 1000U; /* 4 ms */
	cfg->pre_ticks = 20U * LE_TICKS_PER_US;
	cfg->jitter_ticks = 32U * LE_TICKS_PER_US;
	cfg->detect_ticks = 64U * LE_TICKS_PER_US; /* preamble and AA take 40 us */
	cfg->slave_ticks = 240U * LE_TICKS_PER_US; /* empty packet and T_IFS: 230 us */
	cfg->ifs_slack = 40U * LE_TICKS_PER_US;
	cfg->guard_ticks = 300U * LE_TICKS_PER_US;
	cfg->promisc_timeout = 50U * 1000U * 1000U; /* 5 s */
	cfg->own_ppm = 50;
	cfg->adv_mhz = 2402;
	cfg->max_skip = 64;
	cfg->connect_crc = true;
}

void le_conn_init(struct le_conn *c, const struct le_conn_cfg *cfg)
{
	memset(c, 0, sizeof(*c));
	if (cfg != NULL) {
		c->cfg = *cfg;
	} else {
		le_conn_cfg_default(&c->cfg);
	}
	c->state = LE_CONN_ADV;
	c->cur_mhz = c->cfg.adv_mhz;
}

struct le_conn_action le_conn_packet(struct le_conn *c, const struct le_conn_pkt *p, uint64_t now)
{
	struct le_conn_action a;
	uint8_t plen;
	uint64_t end;
	bool md;

	memset(&a, 0, sizeof(a));
	if (c->state == LE_CONN_ADV) {
		if (p->len >= 36U && (p->pdu[0] & 0x0fU) == CONNECT_REQ_TYPE &&
		    p->pdu[1] == CONNECT_REQ_LEN && (p->crc_ok || !c->cfg.connect_crc)) {
			return start_connect(c, p, now);
		}
		return a;
	}
	if (p->mhz != c->cur_mhz || p->len < 2U) {
		c->stats.stale++;
		return a;
	}
	if (!p->crc_ok) {
		c->stats.bad_crc++;
		return a;
	}
	plen = p->pdu[1];
	end = p->t + LE_PKT_TICKS(plen);
	md = ((p->pdu[0] >> 4) & 1U) != 0U;
	if (c->npk < 4U) {
		c->pk[c->npk].t = p->t;
		c->pk[c->npk].end = end;
		c->pk[c->npk].md = md;
	}
	if (c->npk < 255U) {
		c->npk++;
	}
	c->last_valid = p->t;
	c->stats.packets++;
	control_pdu(c, p);
	/* An exchange is a packet of each side; the event goes on while either has more data. */
	if ((c->npk & 1U) == 0U && c->npk <= 4U && !c->pk[c->npk - 1U].md &&
	    !c->pk[c->npk - 2U].md) {
		return close_event(c, now);
	}
	if ((c->npk & 1U) == 0U && c->npk > 4U && !md) {
		return close_event(c, now); /* long events: judge by the last packet only */
	}
	c->deadline = end + LE_T_IFS_TICKS + c->cfg.ifs_slack + c->cfg.detect_ticks;
	a.act = LE_ACT_WAIT;
	a.mhz = c->cur_mhz;
	a.aa = c->aa;
	a.crc_init = c->crc_init;
	a.t_end = c->deadline;
	return a;
}

struct le_conn_action le_conn_tick(struct le_conn *c, uint64_t now)
{
	struct le_conn_action none;

	if (c->state == LE_CONN_ADV || now < c->deadline) {
		memset(&none, 0, sizeof(none));
		return none;
	}
	return close_event(c, now);
}

struct le_conn_action le_conn_cancel(struct le_conn *c)
{
	return act_adv(c, LE_REASON_CANCEL);
}

struct le_conn_action le_conn_start_known(struct le_conn *c, const struct le_conn_known *k,
					  uint64_t now)
{
	static const uint8_t all[5] = {0xff, 0xff, 0xff, 0xff, 0x1f};

	if (k->interval < 6U || k->interval > 3200U || k->hop == 0U || k->chan_idx > 36U) {
		return act_adv(c, LE_REASON_BAD_PARAMS);
	}
	c->csa2 = false;
	c->aa = k->aa;
	c->crc_init = k->crc_init;
	c->hop = k->hop;
	c->ev0 = 0;
	c->unmapped0 = k->chan_idx;
	chm_set(&c->chm, all);
	c->chm_pending = false;
	c->upd_pending = false;
	c->terminating = false;
	c->encrypted = false;
	c->phy_pending = false;
	c->phy_lost = false;
	c->counter_known = false;
	c->ppm = sca_ppm[0] + (uint32_t)c->cfg.own_ppm;
	c->interval = (uint64_t)k->interval * LE_TICKS_PER_UNIT;
	c->timeout = max64(c->cfg.promisc_timeout, 10U * c->interval);
	c->spread = 0;
	c->ev = 0;
	c->anchor = k->t;
	c->ref_t = k->t;
	c->last_valid = k->t;
	c->anchored = true;
	c->verified = false;
	c->radio_free = max64(now, k->t);
	c->cur_mhz = chan_mhz(k->chan_idx);
	c->state = LE_CONN_TRACK;
	c->stats.connects++;
	advance(c);
	return plan(c, now);
}
