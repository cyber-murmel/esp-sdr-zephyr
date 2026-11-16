/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Coordinator MAC: see include/zigbee/zb_mac.h.
 */
#include <errno.h>
#include <string.h>

#include <zephyr/random/random.h>

#include <zigbee/zb_mac.h>

/* Unslotted CSMA-CA (802.15.4 macMinBE, macMaxBE, macMaxCSMABackoffs) and the unit backoff. */
#define CSMA_MIN_BE      3
#define CSMA_MAX_BE      5
#define CSMA_MAX_BACKOFF 4
#define UNIT_BACKOFF_US  320
/* A TX refused because a frame is in the air (-EIO) is retried this often, 0.5 ms apart. */
#define BUSY_RETRIES     8

#define ED_WINDOW_MS     16

static bool in_mac_thread(const struct zb_mac *m)
{
	return k_current_get() == m->tid;
}

/* Scan duration 15.36 ms * (2^exp + 1), rounded up to whole ms. */
static uint32_t scan_ms(uint8_t exp)
{
	return ((((uint32_t)1U << exp) + 1U) * 1536U + 99U) / 100U;
}

/* Vendor-defined 0..255 scale of the energy detect value: -95 dBm is 0, 3 counts per dB. */
static uint8_t ed_scale(int8_t dbm)
{
	int v = 3 * ((int)dbm + 95);

	return (uint8_t)CLAMP(v, 0, 255);
}

static void trace(struct zb_mac *m, bool tx, const uint8_t *f, size_t n, uint8_t lqi, int8_t rssi)
{
	if (m->up.trace != NULL) {
		m->up.trace(m->up.ctx, tx, f, n, lqi, rssi);
	}
}

/* One frame with unslotted CSMA-CA and, for an ACK request, the MAC's frame retries. */
static int tx_frame(struct zb_mac *m, const uint8_t *f, size_t n, bool ar)
{
	int nb = 0, be = CSMA_MIN_BE, retries = 0, busy = 0;

	trace(m, true, f, n, 0, 0);
	for (;;) {
		uint32_t us = (sys_rand32_get() & ((1U << be) - 1U)) * UNIT_BACKOFF_US;
		int r;

		if (us > 2000U) {
			k_usleep(us);
		} else if (us > 0U) {
			k_busy_wait(us);
		}
		r = m->radio->tx(m->rctx, f, n, true);
		if (r == 0) {
			return 0;
		}
		if (r == -EBUSY) {
			be = MIN(be + 1, CSMA_MAX_BE);
			if (++nb > CSMA_MAX_BACKOFF) {
				m->tx_fail++;
				return -EAGAIN;
			}
		} else if (r == -ENOMSG && ar) {
			nb = 0;
			be = CSMA_MIN_BE;
			if (++retries > ZB_MAC_MAX_RETRIES) {
				m->tx_fail++;
				return -ENOMSG;
			}
		} else if (r == -EIO) {
			if (++busy > BUSY_RETRIES) {
				m->tx_fail++;
				return -EIO;
			}
			k_busy_wait(500);
		} else {
			m->tx_fail++;
			return r;
		}
	}
}

/* Put the radio back to what the MAC is meant to do between jobs. */
static int restore_radio(struct zb_mac *m)
{
	static const uint16_t none = 0xFFFF;
	int r;

	if (m->started) {
		r = m->radio->set_ids(m->rctx, m->pan, 0x0000, m->eui);
		r |= m->radio->set_mode(m->rctx, true, false);
		r |= m->radio->set_channel(m->rctx, m->channel);
	} else {
		r = m->radio->set_ids(m->rctx, none, none, m->eui);
		r |= m->radio->set_mode(m->rctx, false, false);
	}
	if (m->txpower_set) {
		r |= m->radio->set_txpower(m->rctx, m->txpower);
	}
	r |= m->radio->start(m->rctx);
	return r != 0 ? -EIO : 0;
}

static struct zb_mac_pending *pending_alloc(struct zb_mac *m)
{
	for (int i = 0; i < ZB_MAC_PENDING_MAX; i++) {
		if (!m->pending[i].used) {
			return &m->pending[i];
		}
	}
	return NULL;
}

static struct zb_mac_pending *pending_find_ext(struct zb_mac *m, const uint8_t ext[8])
{
	for (int i = 0; i < ZB_MAC_PENDING_MAX; i++) {
		struct zb_mac_pending *p = &m->pending[i];

		if (p->used && p->by_ext && memcmp(p->ext, ext, 8) == 0) {
			return p;
		}
	}
	return NULL;
}

static struct zb_mac_pending *pending_find_short(struct zb_mac *m, uint16_t s)
{
	for (int i = 0; i < ZB_MAC_PENDING_MAX; i++) {
		struct zb_mac_pending *p = &m->pending[i];

		if (p->used && !p->by_ext && p->short_addr == s) {
			return p;
		}
	}
	return NULL;
}

static void pending_free(struct zb_mac_pending *p)
{
	p->used = false;
}

/* Drop every queued transaction; the layer above learns that its responses never went out. */
static void pending_flush(struct zb_mac *m)
{
	for (int i = 0; i < ZB_MAC_PENDING_MAX; i++) {
		struct zb_mac_pending *p = &m->pending[i];

		if (p->used && p->assoc && m->up.assoc_done != NULL) {
			m->up.assoc_done(m->up.ctx, p->ext, p->short_addr, false);
		}
		p->used = false;
	}
}

static bool permit_open(const struct zb_mac *m)
{
	return m->permit_until == INT64_MAX || k_uptime_get() < m->permit_until;
}

static void send_beacon(struct zb_mac *m)
{
	uint8_t f[ZB_MAC_MAX_FRAME];
	struct zb_beacon_info b = m->bcn;
	size_t n;

	b.permit = permit_open(m);
	n = zb_mac_build_beacon(f, m->bsn++, &b);
	(void)tx_frame(m, f, n, false);
}

static void handle_assoc_req(struct zb_mac *m, const struct zb_mac_hdr *h, const uint8_t *f,
			     size_t n)
{
	struct zb_mac_pending *p;
	uint16_t short_addr = 0xFFFF;
	uint8_t status, cap;

	if (h->src_mode != ZB_ADDR_EXT || n < (size_t)h->len + 2U) {
		return;
	}
	/* Addressed to this coordinator (short 0x0000 or its extended address) in this PAN. */
	if (h->dst_pan != m->pan ||
	    !((h->dst_mode == ZB_ADDR_SHORT && h->dst_short == 0x0000) ||
	      (h->dst_mode == ZB_ADDR_EXT && memcmp(h->dst_ext, m->eui, 8) == 0))) {
		return;
	}
	cap = f[h->len + 1];

	/*
	 * A slot for the response first, so that nothing is allocated above that cannot be
	 * answered. A joiner that asks again before it polled replaces its own queued response.
	 */
	p = pending_find_ext(m, h->src_ext);
	if (p == NULL) {
		p = pending_alloc(m);
	}
	if (p == NULL) {
		m->tx_fail++;
		return;
	}

	if (!permit_open(m)) {
		status = ZB_ASSOC_DENIED;
	} else {
		status = m->up.assoc_req != NULL
				 ? m->up.assoc_req(m->up.ctx, h->src_ext, cap, &short_addr)
				 : ZB_ASSOC_DENIED;
	}
	if (status != ZB_ASSOC_OK) {
		short_addr = 0xFFFF;
	}

	memset(p, 0, sizeof(*p));
	p->used = true;
	p->assoc = true;
	p->by_ext = true;
	memcpy(p->ext, h->src_ext, 8);
	p->short_addr = short_addr;
	p->n = (uint8_t)zb_mac_build_assoc_rsp(p->f, m->dsn++, m->pan, h->src_ext, m->eui,
					       short_addr, status);
	p->expiry = k_uptime_get() + ZB_MAC_PERSISTENCE_MS;
}

static void handle_data_req(struct zb_mac *m, const struct zb_mac_hdr *h)
{
	struct zb_mac_pending *p = NULL;
	uint8_t f[ZB_MAC_MAX_FRAME];
	size_t n;

	if (h->src_mode == ZB_ADDR_EXT) {
		p = pending_find_ext(m, h->src_ext);
	} else if (h->src_mode == ZB_ADDR_SHORT) {
		p = pending_find_short(m, h->src_short);
	} else {
		return;
	}

	if (p != NULL) {
		/* The hardware ACKed the request with frame pending: deliver at once. */
		if (tx_frame(m, p->f, p->n, true) == 0) {
			if (p->assoc && m->up.assoc_done != NULL) {
				m->up.assoc_done(m->up.ctx, p->ext, p->short_addr, true);
			}
			pending_free(p);
		}
		return;
	}

	/* Nothing queued: the ACK promised a frame, so send an empty data frame. */
	if (h->src_mode == ZB_ADDR_SHORT) {
		n = zb_mac_build_empty_data(f, m->dsn++, m->pan, &h->src_short, NULL, 0x0000);
	} else {
		n = zb_mac_build_empty_data(f, m->dsn++, m->pan, NULL, h->src_ext, 0x0000);
	}
	(void)tx_frame(m, f, n, true);
}

/* The frame is addressed to this coordinator or to everyone in its PAN (or to any PAN). */
static bool for_us(const struct zb_mac *m, const struct zb_mac_hdr *h)
{
	if (h->dst_mode == ZB_ADDR_NONE || (h->dst_pan != m->pan && h->dst_pan != ZB_PAN_BROADCAST)) {
		return false;
	}
	if (h->dst_mode == ZB_ADDR_SHORT) {
		return h->dst_short == 0x0000 || h->dst_short == ZB_SHORT_BROADCAST;
	}
	return memcmp(h->dst_ext, m->eui, 8) == 0;
}

static void handle_rx(struct zb_mac *m, const struct zb_mac_rx_item *it)
{
	struct zb_mac_hdr h;

	trace(m, false, it->f, it->n, it->lqi, it->rssi);
	if (!m->started || zb_mac_parse(it->f, it->n, &h) != 0 || !for_us(m, &h)) {
		return;
	}

	switch (h.type) {
	case ZB_FT_DATA:
		if (h.dst_mode != ZB_ADDR_SHORT || m->up.data_ind == NULL) {
			return;
		}
		m->up.data_ind(m->up.ctx, &h, &it->f[h.len], it->n - h.len, it->lqi, it->rssi);
		return;
	case ZB_FT_CMD:
		if (it->n <= h.len) {
			return;
		}
		switch (it->f[h.len]) {
		case ZB_CMD_BEACON_REQ:
			send_beacon(m);
			return;
		case ZB_CMD_ASSOC_REQ:
			handle_assoc_req(m, &h, it->f, it->n);
			return;
		case ZB_CMD_DATA_REQ:
			handle_data_req(m, &h);
			return;
		default:
			return;
		}
	default:
		return;
	}
}

static void expire_pending(struct zb_mac *m)
{
	int64_t now = k_uptime_get();

	for (int i = 0; i < ZB_MAC_PENDING_MAX; i++) {
		struct zb_mac_pending *p = &m->pending[i];

		if (p->used && now >= p->expiry) {
			if (p->assoc && m->up.assoc_done != NULL) {
				m->up.assoc_done(m->up.ctx, p->ext, p->short_addr, false);
			}
			pending_free(p);
		}
	}
}

static int64_t next_wait_ms(const struct zb_mac *m)
{
	int64_t now = k_uptime_get(), wait = 1000;

	for (int i = 0; i < ZB_MAC_PENDING_MAX; i++) {
		if (m->pending[i].used) {
			wait = MIN(wait, MAX(m->pending[i].expiry - now, 1));
		}
	}
	return wait;
}

static void mac_thread(void *p1, void *p2, void *p3)
{
	struct zb_mac *m = p1;
	struct zb_mac_event ev;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	for (;;) {
		if (k_msgq_get(&m->q, &ev, K_MSEC(next_wait_ms(m))) == 0) {
			if (ev.is_job) {
				int r = ev.job.fn(m, ev.job.arg);

				if (ev.job.ret != NULL) {
					*ev.job.ret = r;
				}
				if (ev.job.done != NULL) {
					k_sem_give(ev.job.done);
				}
			} else {
				handle_rx(m, &ev.rx);
			}
		}
		expire_pending(m);
		if (m->up.tick != NULL) {
			m->up.tick(m->up.ctx);
		}
	}
}

void zb_mac_init(struct zb_mac *m, const struct zb_radio_ops *radio, void *rctx,
		 const struct zb_mac_up *up, const uint8_t eui[8])
{
	memset(m, 0, sizeof(*m));
	m->radio = radio;
	m->rctx = rctx;
	m->up = *up;
	memcpy(m->eui, eui, 8);
	m->dsn = (uint8_t)sys_rand32_get();
	m->bsn = (uint8_t)sys_rand32_get();
	m->tid = &m->thread;
	k_msgq_init(&m->q, m->qbuf, sizeof(struct zb_mac_event), 16);
	k_msgq_init(&m->scanq, m->scanbuf, sizeof(struct zb_mac_rx_item), 16);
}

void zb_mac_run(struct zb_mac *m, k_thread_stack_t *stack, size_t stack_size, int prio)
{
	k_thread_create(&m->thread, stack, stack_size, mac_thread, m, NULL, NULL, prio, 0,
			K_NO_WAIT);
	k_thread_name_set(&m->thread, "zb_mac");
}

void zb_mac_rx(struct zb_mac *m, const uint8_t *f, uint8_t n, uint8_t lqi, int8_t rssi)
{
	struct zb_mac_event ev = {.is_job = false};
	struct zb_mac_rx_item *it = &ev.rx;

	if (n == 0U || n > ZB_MAC_MAX_FRAME) {
		return;
	}
	it->n = n;
	it->lqi = lqi;
	it->rssi = rssi;
	memcpy(it->f, f, n);
	if (atomic_get(&m->scanning) != 0) {
		if (k_msgq_put(&m->scanq, it, K_NO_WAIT) != 0) {
			m->rx_dropped++;
		}
	} else if (k_msgq_put(&m->q, &ev, K_NO_WAIT) != 0) {
		m->rx_dropped++;
	}
}

int zb_mac_post(struct zb_mac *m, int (*fn)(struct zb_mac *m, void *arg), void *arg)
{
	struct zb_mac_event ev = {.is_job = true, .job = {.fn = fn, .arg = arg}};

	return k_msgq_put(&m->q, &ev, K_SECONDS(1)) == 0 ? 0 : -EBUSY;
}

int zb_mac_call(struct zb_mac *m, int (*fn)(struct zb_mac *m, void *arg), void *arg)
{
	struct k_sem done;
	int ret = 0;
	struct zb_mac_event ev = {.is_job = true,
				  .job = {.fn = fn, .arg = arg, .done = &done, .ret = &ret}};

	if (in_mac_thread(m)) {
		return fn(m, arg);
	}
	k_sem_init(&done, 0, 1);
	if (k_msgq_put(&m->q, &ev, K_SECONDS(1)) != 0) {
		return -EBUSY;
	}
	k_sem_take(&done, K_FOREVER);
	return ret;
}

struct escan_args {
	uint32_t mask;
	uint8_t exp;
	uint8_t *ed;
};

static int energy_scan_job(struct zb_mac *m, void *arg)
{
	struct escan_args *a = arg;
	uint32_t total = scan_ms(a->exp);
	size_t i = 0;
	int ret = 0;

	/* Frames that arrive while the radio is not set up for the network are not for it. */
	atomic_set(&m->scanning, 1);
	for (uint8_t ch = 11; ch <= 26 && ret == 0; ch++) {
		int8_t best = INT8_MIN;

		if ((a->mask & BIT(ch)) == 0U) {
			continue;
		}
		ret = m->radio->set_channel(m->rctx, ch);
		/* The radio reports the mean over a window; the MAC scan wants the maximum. */
		for (uint32_t left = total; left > 0U && ret == 0;) {
			uint32_t win = MIN(left, ED_WINDOW_MS);
			int8_t dbm;

			ret = m->radio->ed(m->rctx, (uint16_t)win, &dbm);
			best = MAX(best, dbm);
			left -= win;
		}
		a->ed[i++] = ed_scale(best);
	}
	if (restore_radio(m) != 0 && ret == 0) {
		ret = -EIO;
	}
	k_msgq_purge(&m->scanq);
	atomic_set(&m->scanning, 0);
	return ret;
}

int zb_mac_energy_scan(struct zb_mac *m, uint32_t mask, uint8_t exp, uint8_t *ed)
{
	struct escan_args a = {.mask = mask, .exp = exp, .ed = ed};

	if (exp > ZB_MAC_SCAN_EXP_MAX) {
		return -EINVAL;
	}
	return zb_mac_call(m, energy_scan_job, &a);
}

struct ascan_args {
	uint32_t mask;
	uint8_t exp;
	struct zb_beacon_info *out;
	size_t max, *count;
};

static void scan_add(struct ascan_args *a, const struct zb_beacon_info *b)
{
	for (size_t i = 0; i < *a->count; i++) {
		const struct zb_beacon_info *o = &a->out[i];

		if (o->channel == b->channel && o->pan == b->pan && o->src_short == b->src_short &&
		    memcmp(o->epid, b->epid, 8) == 0) {
			return;
		}
	}
	if (*a->count < a->max) {
		a->out[(*a->count)++] = *b;
	}
}

static int active_scan_job(struct zb_mac *m, void *arg)
{
	static const uint16_t any = 0xFFFF;
	struct ascan_args *a = arg;
	uint32_t dwell = scan_ms(a->exp);
	int ret = 0;

	*a->count = 0;
	/* Frames received from here to the end of the restore are scan traffic, not the network's. */
	atomic_set(&m->scanning, 1);
	/* Promiscuous (every beacon, whatever PAN) and no auto ACK: only for the scan. */
	ret = m->radio->set_ids(m->rctx, any, any, m->eui);
	ret |= m->radio->set_mode(m->rctx, false, true);
	for (uint8_t ch = 11; ch <= 26 && ret == 0; ch++) {
		struct zb_mac_rx_item it;
		uint8_t req[8];
		int64_t end;

		if ((a->mask & BIT(ch)) == 0U) {
			continue;
		}
		ret = m->radio->set_channel(m->rctx, ch);
		ret |= m->radio->start(m->rctx);
		k_msgq_purge(&m->scanq);
		(void)tx_frame(m, req, zb_mac_build_beacon_req(req, m->dsn++), false);
		end = k_uptime_get() + dwell;
		for (int64_t left = dwell; left > 0; left = end - k_uptime_get()) {
			struct zb_mac_hdr h;
			struct zb_beacon_info b;

			if (k_msgq_get(&m->scanq, &it, K_MSEC(left)) != 0) {
				break;
			}
			if (zb_mac_parse(it.f, it.n, &h) == 0 && h.type == ZB_FT_BEACON &&
			    zb_mac_parse_beacon(&h, &it.f[h.len], it.n - h.len, &b) == 0) {
				b.channel = ch;
				b.lqi = it.lqi;
				b.rssi = it.rssi;
				scan_add(a, &b);
			}
		}
	}
	if (restore_radio(m) != 0) {
		ret = -EIO;
	}
	k_msgq_purge(&m->scanq);
	atomic_set(&m->scanning, 0);
	return ret != 0 ? -EIO : 0;
}

int zb_mac_active_scan(struct zb_mac *m, uint32_t mask, uint8_t exp, struct zb_beacon_info *out,
		       size_t max, size_t *count)
{
	struct ascan_args a = {.mask = mask, .exp = exp, .out = out, .max = max, .count = count};

	if (exp > ZB_MAC_SCAN_EXP_MAX) {
		return -EINVAL;
	}
	return zb_mac_call(m, active_scan_job, &a);
}

struct start_args {
	uint16_t pan;
	uint8_t channel;
	const struct zb_beacon_info *bcn;
};

static int start_job(struct zb_mac *m, void *arg)
{
	struct start_args *a = arg;

	pending_flush(m);
	m->pan = a->pan;
	m->channel = a->channel;
	m->bcn = *a->bcn;
	m->bcn.pan = a->pan;
	m->bcn.src_short = 0x0000;
	m->bcn.pan_coord = true;
	m->bcn.stack_profile = 2;
	m->bcn.proto_ver = 2;
	m->bcn.depth = 0;
	m->permit_until = 0;
	m->started = true;
	return restore_radio(m);
}

int zb_mac_start(struct zb_mac *m, uint16_t pan, uint8_t channel, const struct zb_beacon_info *bcn)
{
	struct start_args a = {.pan = pan, .channel = channel, .bcn = bcn};

	return zb_mac_call(m, start_job, &a);
}

static int stop_job(struct zb_mac *m, void *arg)
{
	ARG_UNUSED(arg);
	m->started = false;
	m->permit_until = 0;
	pending_flush(m);
	return restore_radio(m);
}

int zb_mac_stop(struct zb_mac *m)
{
	return zb_mac_call(m, stop_job, NULL);
}

static int permit_job(struct zb_mac *m, void *arg)
{
	uint8_t s = *(uint8_t *)arg;

	m->permit_until = s == 0U ? 0 : s == 0xFFU ? INT64_MAX : k_uptime_get() + s * 1000LL;
	return 0;
}

int zb_mac_permit(struct zb_mac *m, uint8_t seconds)
{
	return zb_mac_call(m, permit_job, &seconds);
}

bool zb_mac_permit_open(struct zb_mac *m)
{
	return permit_open(m);
}

struct cap_args {
	bool router, ed;
};

static int cap_job(struct zb_mac *m, void *arg)
{
	struct cap_args *a = arg;

	m->bcn.router_cap = a->router;
	m->bcn.ed_cap = a->ed;
	return 0;
}

int zb_mac_set_capacity(struct zb_mac *m, bool router_cap, bool ed_cap)
{
	struct cap_args a = {.router = router_cap, .ed = ed_cap};

	return zb_mac_call(m, cap_job, &a);
}

struct net_args {
	const uint8_t *epid;
	uint8_t upd;
};

static int net_job(struct zb_mac *m, void *arg)
{
	struct net_args *a = arg;

	memcpy(m->bcn.epid, a->epid, 8);
	m->bcn.upd = a->upd;
	return 0;
}

int zb_mac_set_network(struct zb_mac *m, const uint8_t epid[8], uint8_t upd)
{
	struct net_args a = {.epid = epid, .upd = upd};

	return zb_mac_call(m, net_job, &a);
}

static int txpower_job(struct zb_mac *m, void *arg)
{
	m->txpower = *(int8_t *)arg;
	m->txpower_set = true;
	/* Reaches the hardware at the next start(), which a running network does now. */
	return m->started ? restore_radio(m) : m->radio->set_txpower(m->rctx, m->txpower);
}

int zb_mac_set_txpower(struct zb_mac *m, int8_t dbm)
{
	return zb_mac_call(m, txpower_job, &dbm);
}

static int eui_job(struct zb_mac *m, void *arg)
{
	memcpy(m->eui, arg, 8);
	return 0;
}

int zb_mac_set_eui(struct zb_mac *m, const uint8_t eui[8])
{
	uint8_t copy[8];

	memcpy(copy, eui, 8);
	return zb_mac_call(m, eui_job, copy);
}

struct data_args {
	uint16_t dst;
	const uint8_t *payload;
	size_t n;
	bool ar;
};

static int data_job(struct zb_mac *m, void *arg)
{
	struct data_args *a = arg;
	uint8_t f[ZB_MAC_MAX_FRAME];
	size_t n;

	if (!m->started || a->n > ZB_MAC_MAX_FRAME - 9U) {
		return -EINVAL;
	}
	n = zb_mac_build_data(f, m->dsn++, m->pan, a->dst, 0x0000, a->ar, a->payload, a->n);
	return tx_frame(m, f, n, a->ar);
}

int zb_mac_send_data(struct zb_mac *m, uint16_t dst_short, const uint8_t *payload, size_t n,
		     bool ar)
{
	struct data_args a = {.dst = dst_short, .payload = payload, .n = n, .ar = ar};

	return zb_mac_call(m, data_job, &a);
}
