/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The network side of the transmit stream. Three parts:
 *
 * - The receive hook (tx_hook, interrupt, CPU 0, IRAM): a frame for our MAC, IPv4 or IPv6, UDP to
 *   port 4994 (TX_PORT), from the subscriber's address and port, with the session's stream ID in the
 *   VITA header, goes to tx_stream_put() and is ours; every other frame is the stack's. A frame for
 *   port 4994 that does not match is dropped here, not given to the stack (it would answer each with an
 *   ICMP port unreachable, at the packet rate of a sender that has not noticed the end), counted by
 *   reason. While no run takes data, the matching frames are dropped and counted too.
 * - The control thread (tx_ctl, CPU 0, flash): UDP 4992 through the stack as in apps/stream_eth/src/ctl.c
 *   (net_context, the source MAC from the frame), the cookie protocol of ctlproto.c unchanged, and
 *   TXGO, TXEN, TXPG (tx_proto.h) from the subscriber, with the state machine of tx_sess.c.
 * - The status thread (tx_stat, CPU 0, the lowest priority, spinning while a subscriber exists): TXST on
 *   every flip and every 250 us while a run exists, TXPO for each ping, from a prebuilt Ethernet, IP and UDP
 *   header, the record read raw from the stream's snapshot and copied into the packet. Frames go out
 *   through dwmac_send_frames() (no net_pkt).
 *
 * Who owns what: the hook's state `hk` is written by the control thread only while `hk.accept` is 0.
 * The subscriber and the run are the control thread's (under `plk` for the cookie protocol); the status
 * thread reads the template under `tlk`. The end of a run comes from the stream's control thread
 * (net_done) through atomics.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/ethernet/eth_dwmac_fast.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_context.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/atomic.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_place.h>

#include "ctlproto.h"
#include "netframe.h"
#include "tx_filter.h"
#include "tx_net.h"
#include "tx_proto.h"
#include "tx_sess.h"
#include "tx_shell.h"
#include "tx_stream.h"
#include "tx_tone.h"
#include "txs_flip.h"
#include "txs_limits.h"

LOG_MODULE_DECLARE(txstream, LOG_LEVEL_INF);

extern bool esp_sdr_ready;

#define SILENT_MS 5000 /* no renewal for this long ends the session */
#define MSG_MAX   32U
#define SLOTS     6U
#define PINGS     4U
/* The status thread wakes every kernel tick (CONFIG_SYS_CLOCK_TICKS_PER_SEC 10000: 100 us) and sends a
 * status when a flip published one, or ST_TICKS (200 us) after the last; after the end the ENDED status
 * every ENDED_TICKS (20 ms) until the subscription goes. Ticks, not the CPU's cycle counter (cyc(): 32 bit,
 * wraps in 13 s at 320 MHz). k_cycle_get_32() is the 16 MHz system timer: it wraps in 268 s. */
#define US_TICKS(us) ((us) * CONFIG_SYS_CLOCK_TICKS_PER_SEC / 1000000U)
BUILD_ASSERT(CONFIG_SYS_CLOCK_TICKS_PER_SEC == 10000, "the status thread's 100 us wake needs the 10 kHz tick (prj.conf)");
#define WAKE_TICKS  US_TICKS(100U)
#define ST_TICKS    US_TICKS(200U)
#define ENDED_TICKS US_TICKS(20000U)
#define ST_PRIO     6 /* above the net threads (8), the control thread (10) and the shell (13); it sleeps */
#define TX_MSGS_PER_S 3000U
#define MAX_ARM_MIN   120U
/* RF: the gate asks as for a tone at the edge of the band (k 175 of 351 is the Nyquist frequency): the sender's
 * samples are not clipped, so any of them may be there */
#define RF_K 175U
/* the longest run a TXGO may ask for, as the shell's */
#define MAX_RUN_S 3600U

enum { RUN_NONE, RUN_ON, RUN_ENDED };

/* ---- the hook's state: small, in group 0, read in the interrupt ---- */

static struct {
	atomic_t accept; /* a run takes data; `f` is written only while this is 0 */
	struct txf_state f;
	uintptr_t last_buf;
	uint32_t offered, matched, idle, bad_src, bad_sid, bad_len, refused;
} hk;

_Static_assert(TXF_PORT == TX_PORT, "the filter's port is the stream's");

/* Receive interrupt, every good frame. Not ours: the stack's (false). A frame to our UDP port: ours, the
 * packet is handed to the stream or dropped here (tx_filter.h says which). */
static ESP_SDR_CODE(EVENT, tx_hook) bool tx_hook(const struct device *dev,
						 const struct dwmac_rx_frame *f, void *user)
{
	uint16_t off, vlen;

	ARG_UNUSED(dev);
	ARG_UNUSED(user);
	hk.offered++;
	hk.last_buf = (uintptr_t)f->buf;
	switch (txf_classify(&hk.f, atomic_get(&hk.accept) != 0, f->data, f->len, &off, &vlen)) {
	case TXF_STACK:
		return false;
	case TXF_LEN:
		hk.bad_len++;
		break;
	case TXF_IDLE:
		hk.idle++;
		break;
	case TXF_SRC:
		hk.bad_src++;
		break;
	case TXF_SID:
		hk.bad_sid++;
		break;
	case TXF_TAKE:
		/* a buffer the stream holds counts as in flight until it is released */
		tx_stream_inflight_add(1);
		if (tx_stream_put(f->buf, (uint16_t)(f->data - f->buf->data) + off, vlen)) {
			hk.matched++;
			return true;
		}
		tx_stream_inflight_add(-1);
		hk.refused++;
		break;
	}
	net_buf_unref(f->buf);
	return true;
}

uint32_t tx_hook_offered(void)
{
	return hk.offered;
}

uintptr_t tx_last_rx_buf(void)
{
	return hk.last_buf;
}

/* ---- the control side, in group 3 ---- */

struct ctlmsg {
	struct peer p;
	uint16_t len;
	uint8_t data[MSG_MAX];
	uint64_t ns; /* the board's clock when it came to the stack */
};

struct slot {
	uint8_t hdr[NF_HDR_MAX + 2U] __aligned(4);
	uint8_t body[TXP_ST_BYTES + 4U] __aligned(4);
	atomic_t busy;
};

struct ping {
	uint32_t seq;
	uint64_t t_ns, b_ns;
};

static struct {
	struct ctl_proto proto;
	struct peer sub;
	struct nf_tmpl tmpl; /* the status' header, for the subscriber */
	bool tmpl_ok;
	struct peer ack_owed_peer;
	bool ack_owed;
	struct slot slots[SLOTS];
	struct ping pings[PINGS];
	/* what the control thread knows about the run */
	uint32_t stream_id;
	bool rf;
	/* statistics of the status thread (the control thread's are in `nc`) */
	uint32_t pong_busy;
	uint32_t st_sent, st_busy, st_fail_r, st_fail_q, st_on_flip, st_cyc_max, st_cyc_n, pong_sent;
	/* from the stream publishing the record after a flip to that status being queued, in us */
	uint32_t fl_max_us;
	uint64_t fl_sum_us;
	/* cycles of a status by part (sums, and the worst whole one): header, record, seal, queue */
	uint64_t c_hdr, c_rec, c_seal, c_q, st_cyc_sum;
	bool rf_cut;
	uint32_t seq;
	uint16_t ip_id;
	int64_t tb_win;
	uint32_t tb_cnt;
} net ESP_SDR_DATA_F(STATE, net, NOINIT);

/* The control thread's and the receive thread's state and counters, cold (a few per second): in LP RAM (place.h:
 * not zeroed at boot, tx_net_init() starts it, no atomics, no DMA) */
static struct {
	struct tx_sess sess;
	uint32_t msgs, dropped, rejected, vrt_reply, go, go_ok, go_refused, en, ping, ping_drop, rf_stops;
} nc ESP_SDR_DATA_F(BULK, nc, NOINIT);

static atomic_t live;      /* a subscriber */
static atomic_t run_state; /* RUN_* */
static atomic_t ended_req; /* net_done() ran: the control thread tells the session */
static atomic_t ended_id;  /* the stream ID of the run it ran for (its status record has it; 0: never published) */
static atomic_t owned;     /* the run belongs to the subscriber: its end is reported (to a new one it is not) */
static atomic_t ping_head, ping_tail;
static atomic_t run_gen;   /* counts the runs started: the status thread starts its per run statistics over */
static uint32_t run_cyc;   /* k_cycle_get_32() before the run's start (written before run_gen rises) */
static atomic_t arm_until_s, arm_gen, sub_gen; /* arm_gen: the subscriber (sub_gen) the arming is for, 0: none */
static uint32_t arm_left_s(void);
static struct k_spinlock tlk, rlk;
static bool starting, done_early; /* under rlk: op_start() is between tx_stream_start() and the end of run_state */

static struct k_mutex plk ESP_SDR_DATA_F(STATE, plk, NOINIT);
static struct k_sem st_wake ESP_SDR_DATA_F(STATE, st_wake, NOINIT);
static struct k_msgq ctl_q ESP_SDR_DATA_F(STATE, ctl_q, NOINIT);
static char ctl_q_buf[4U * sizeof(struct ctlmsg)] __aligned(4) ESP_SDR_DATA_F(STATE, ctl_q_buf, NOINIT);
static struct k_thread ctl_thread ESP_SDR_DATA_F(STATE, ctl_thread, NOINIT);
static struct k_thread st_thread ESP_SDR_DATA_F(STATE, st_thread, NOINIT);

/* In group 3, not zeroed at boot: `tx net` before tx_net_init() (main waits for the network config first) reads
 * their stack_info */
static int net_threads_zero(void)
{
	memset(&ctl_thread, 0, sizeof(ctl_thread));
	memset(&st_thread, 0, sizeof(st_thread));
	return 0;
}
SYS_INIT(net_threads_zero, POST_KERNEL, 0);
/* the control thread: cold, its stack in PSRAM (place.h) */
ESP_SDR_STACK_DEFINE(ctl_stack, 2048);
static K_THREAD_STACK_DEFINE(st_stack, 1280);

static const struct device *eth_dev;
static struct net_if *iface;

static ALWAYS_INLINE uint32_t cyc(void)
{
	uint32_t v;

	__asm__ volatile("csrr %0, cycle" : "=r"(v));
	return v;
}

static uint64_t now_ns(void)
{
	return k_cyc_to_ns_floor64(k_cycle_get_64());
}

/* ---- sending: a slot holds the header and the body until the MAC is done with them ---- */

static ESP_SDR_CODE(EVENT, slot_done) void slot_done(void *arg)
{
	atomic_clear(&((struct slot *)arg)->busy);
}

static struct slot *slot_get(void)
{
	for (unsigned int k = 0; k < SLOTS; k++) {
		if (atomic_cas(&net.slots[k].busy, 0, 1)) {
			return &net.slots[k];
		}
	}
	return NULL;
}

/* The slot's header holds the template's bytes and its body `len` bytes: seal and queue. */
static int slot_send(struct slot *s, struct nf_tmpl *t, size_t len)
{
	struct dwmac_tx_frame f;

	nf_seal(t, s->hdr, 0, s->body, len);
	f = (struct dwmac_tx_frame){.b1 = s->hdr, .l1 = t->len, .b2 = s->body, .l2 = (uint16_t)len,
				    .done = slot_done, .arg = s};
	if (dwmac_send_frames(eth_dev, &f, 1) != 1) {
		atomic_clear(&s->busy);
		return -EAGAIN;
	}
	return 0;
}

/* A datagram to a host that sent one (the peer's MAC is from its frame): control replies. */
static int net_send(const struct peer *to, const void *payload, size_t len)
{
	struct nf_tmpl t;
	struct slot *s;

	if (len > sizeof(s->body)) {
		return -EMSGSIZE;
	}
	s = slot_get();
	if (s == NULL) {
		return -EBUSY;
	}
	nf_tmpl_init(&t, to->v6, to->mac, hk.f.mac, to->local, to->addr, TX_CTL_PORT, to->port);
	memcpy(s->hdr, t.b, t.len);
	memcpy(s->body, payload, len);
	return slot_send(s, &t, len);
}

/* A status or a pong: the subscriber's template, copied. */
static struct slot *slot_for_sub(struct nf_tmpl *t)
{
	struct slot *s = slot_get();
	k_spinlock_key_t key;
	bool ok;

	if (s == NULL) {
		return NULL;
	}
	key = k_spin_lock(&tlk);
	ok = net.tmpl_ok;
	if (ok) {
		*t = net.tmpl;
	}
	k_spin_unlock(&tlk, key);
	if (!ok) {
		atomic_clear(&s->busy);
		return NULL;
	}
	t->id = net.ip_id;
	memcpy(s->hdr, t->b, t->len);
	return s;
}

/* Seal the frame in the slot and queue it: *cs the cycles of the seal, *cq of the queueing. */
static int slot_send_timed(struct slot *s, struct nf_tmpl *t, size_t len, uint32_t *cs, uint32_t *cq)
{
	struct dwmac_tx_frame f;
	const uint32_t t0 = cyc();
	uint32_t t1;

	nf_seal(t, s->hdr, 0, s->body, len);
	t1 = cyc();
	f = (struct dwmac_tx_frame){.b1 = s->hdr, .l1 = t->len, .b2 = s->body, .l2 = (uint16_t)len,
				    .done = slot_done, .arg = s};
	if (dwmac_send_frames(eth_dev, &f, 1) != 1) {
		atomic_clear(&s->busy);
		*cs = t1 - t0;
		*cq = cyc() - t1;
		return -EAGAIN;
	}
	*cs = t1 - t0;
	*cq = cyc() - t1;
	return 0;
}

/* One status; its parts in cycles are added to the statistics (header, record, seal, queue). */
static bool send_status(void)
{
	struct nf_tmpl t;
	uint32_t flags = 0U, c_hdr, c_rec, c_seal, c_q;
	const uint32_t t0 = cyc();
	struct slot *s = slot_for_sub(&t);
	uint32_t t1 = cyc();

	if (s == NULL) {
		net.st_busy++;
		return false;
	}
	c_hdr = t1 - t0;
	memcpy(s->body, "TXST", 4);
	txp_put32(s->body + 4, net.seq++);
	if (!tx_stream_status_raw(s->body + 8)) {
		atomic_clear(&s->busy);
		net.st_fail_r++;
		return false;
	}
	c_rec = cyc() - t1;
	{
		const int st = (int)atomic_get(&run_state);

		/* RF is on from the chain's start, before the engine plays (an RF run in its prestart) */
		if (st == RUN_ON && net.rf) {
			flags |= TXP_ST_RF;
		}
		if (tx_stream_ready()) {
			flags |= TXP_ST_RUNNING;
		}
		if (st == RUN_ENDED) {
			flags = TXP_ST_ENDED;
		}
	}
	txp_put32(s->body + 72, flags);
	if (slot_send_timed(s, &t, TXP_ST_BYTES, &c_seal, &c_q) != 0) {
		net.st_fail_q++;
		return false;
	}
	net.ip_id = t.id;
	net.st_sent++;
	net.c_hdr += c_hdr;
	net.c_rec += c_rec;
	net.c_seal += c_seal;
	net.c_q += c_q;
	{
		const uint32_t c = c_hdr + c_rec + c_seal + c_q;

		net.st_cyc_sum += c;
		net.st_cyc_n++;
		net.st_cyc_max = MAX(net.st_cyc_max, c);
	}
	return true;
}

static void send_pong(const struct ping *p)
{
	struct nf_tmpl t;
	struct slot *s = slot_for_sub(&t);
	struct txp_ping m = {.seq = p->seq, .t_ns = p->t_ns, .b_ns = p->b_ns};

	if (s == NULL) {
		net.pong_busy++;
		return;
	}
	(void)txp_po_put(s->body, sizeof(s->body), &m);
	if (slot_send(s, &t, TXP_PO_BYTES) == 0) {
		net.ip_id = t.id;
		net.pong_sent++;
	}
}

/* ---- the status thread ---- */

/* Above the shell and the network threads, asleep between its wakes (a spinning thread of that priority would
 * starve them; at the lowest it was preempted for up to a millisecond). It wakes on an absolute tick, every
 * WAKE_TICKS (100 us), and polls the stream's flip counter then: a k_sem_give() from the flip thread on CPU 1
 * would take the scheduler's global lock and send an interrupt, and the flips are not to wait for either. A ping
 * wakes it at once. After the end it wakes every ENDED_TICKS. */
static void st_entry(void *a, void *b, void *c)
{
	int64_t last = 0, ended_last = 0, next = k_uptime_ticks() + WAKE_TICKS;
	uint32_t flips = 0U;
	atomic_val_t gen = 0;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		int64_t now;
		int state = (int)atomic_get(&run_state);

		if (atomic_get(&live) == 0) {
			(void)k_sem_take(&st_wake, K_MSEC(100));
			next = k_uptime_ticks() + WAKE_TICKS;
			continue;
		}
		if (state == RUN_ON) {
			(void)k_sem_take(&st_wake, K_TIMEOUT_ABS_TICKS(next));
		} else {
			(void)k_sem_take(&st_wake, state == RUN_ENDED ? K_TICKS(ENDED_TICKS) : K_MSEC(50));
		}
		now = k_uptime_ticks();
		if (now >= next) {
			next += WAKE_TICKS;
			if (next <= now) {
				next = now + WAKE_TICKS;
			}
		}
		/* the pings first: the host's round trip is the least of them */
		while (atomic_get(&ping_tail) != atomic_get(&ping_head)) {
			const atomic_val_t t = atomic_get(&ping_tail);

			send_pong(&net.pings[t % PINGS]);
			atomic_set(&ping_tail, t + 1);
		}
		state = (int)atomic_get(&run_state);
		if (state == RUN_ON && atomic_get(&run_gen) != gen) {
			/* a new run: its flip counter starts at 0 and the flip-to-status figures are its own, not the
			 * last run's (a stamp from before this run's start gave 19.8 s at most) */
			gen = atomic_get(&run_gen);
			flips = tx_stream_flips();
			net.st_on_flip = 0U;
			net.fl_sum_us = 0U;
			net.fl_max_us = 0U;
		}
		if (state == RUN_ON) {
			const uint32_t fl = tx_stream_flips();
			const bool flip = fl != flips;

			if (flip || now - last >= ST_TICKS) {
				flips = fl;
				last = now;
				if (send_status() && flip) {
					const uint32_t ts = tx_stream_flip_ts(), nowc = k_cycle_get_32();

					/* a stamp from before this run's start is a stale one: not counted. The cycle counter
					 * (16 MHz) wraps in 268 s: the stamp must lie between the start and now (modulo
					 * 2^32); a run more than 67 s old has no stale stamp left, the flip counter was
					 * started over with it */
					if ((uint32_t)(ts - run_cyc) <= (uint32_t)(nowc - run_cyc) ||
					    (uint32_t)(nowc - run_cyc) > 0x40000000U) {
						const uint32_t us = k_cyc_to_us_floor32(nowc - ts);

						net.st_on_flip++;
						net.fl_sum_us += us;
						net.fl_max_us = MAX(net.fl_max_us, us);
					}
				}
			}
		} else if (state == RUN_ENDED && now - ended_last >= ENDED_TICKS) {
			ended_last = now;
			(void)send_status();
		}
	}
}

/* ---- the control thread ---- */

/* The cookie of a TX message: the protocol's, current or previous epoch. */
static bool cookie_ok(const struct peer *p, const uint8_t *ck, int64_t now)
{
	const uint32_t epoch = (uint32_t)(now / CTL_EPOCH_MS);
	uint8_t want[8];

	for (uint32_t e = epoch;; e = epoch - 1U) {
		ctl_proto_cookie(&net.proto, p, e, want);
		if (memcmp(want, ck, 8) == 0) {
			return true;
		}
		if (e != epoch || epoch == 0U) {
			return false;
		}
	}
}

static bool addr_local_ok(const struct peer *p)
{
	struct net_if *i = NULL;

	if (p->v6) {
		return net_if_ipv6_addr_lookup((struct net_in6_addr *)p->local, &i) != NULL && i == iface;
	}
	return net_if_ipv4_addr_lookup((struct net_in_addr *)p->local, &i) != NULL && i == iface;
}

static bool peer_valid(const struct peer *p)
{
	bool all_zero_mac = true;

	for (int k = 0; k < 6; k++) {
		all_zero_mac &= p->mac[k] == 0U;
	}
	if ((p->mac[0] & 1U) != 0U || all_zero_mac || p->port == 0U) {
		return false;
	}
	if (p->v6) {
		struct net_in6_addr *a = (struct net_in6_addr *)p->addr;

		if (net_ipv6_is_addr_mcast(a) || net_ipv6_is_addr_unspecified(a)) {
			return false;
		}
		if (!net_ipv6_is_ll_addr(a) && net_if_ipv6_prefix_lookup(iface, a, 64) == NULL) {
			return false;
		}
	} else {
		struct net_in_addr *a = (struct net_in_addr *)p->addr;

		if (net_ipv4_is_addr_mcast(a) || net_ipv4_is_addr_bcast(iface, a) ||
		    net_ipv4_is_addr_unspecified(a)) {
			return false;
		}
		if (!net_ipv4_is_ll_addr(a) && !net_if_ipv4_addr_mask_cmp(iface, a)) {
			return false;
		}
	}
	return addr_local_ok(p);
}

static bool is_sub(const struct peer *p)
{
	return net.proto.live && ctl_peer_same(&net.proto.sub, p);
}

/* Called by the RX thread: copy out what is needed, give the packet back. */
static void recv_cb(struct net_context *ctx, struct net_pkt *p, union net_ip_header *ip,
		    union net_proto_header *proto_hdr, int status, void *user)
{
	struct ctlmsg m;
	struct net_linkaddr *ll;
	size_t len;
	bool tx, ok;
	const int64_t now = k_uptime_get();

	ARG_UNUSED(ctx);
	ARG_UNUSED(user);
	if (p == NULL) {
		return;
	}
	m.ns = now_ns();
	if (status != 0) {
		goto out;
	}
	memset(&m.p, 0, sizeof(m.p));
	m.p.v6 = net_pkt_family(p) == NET_AF_INET6;
	if (m.p.v6) {
		memcpy(m.p.addr, ip->ipv6->src, 16);
		memcpy(m.p.local, ip->ipv6->dst, 16);
	} else {
		memcpy(m.p.addr, ip->ipv4->src, 4);
		memcpy(m.p.local, ip->ipv4->dst, 4);
	}
	m.p.port = net_ntohs(proto_hdr->udp->src_port);
	ll = net_pkt_lladdr_src(p);
	if (ll->len != 6U) {
		goto out;
	}
	memcpy(m.p.mac, ll->addr, 6);
	if (!peer_valid(&m.p)) {
		nc.rejected++;
		goto out;
	}
	len = net_pkt_remaining_data(p);
	if (len < 4U || len > MSG_MAX || net_pkt_read(p, m.data, len) != 0) {
		nc.dropped++;
		goto out;
	}
	/* the subscriber's own TX messages (pings every few ms) have a budget of their own; everyone else
	 * the protocol's (20 a second a source, 300 in all) */
	tx = m.data[0] == 'T' && m.data[1] == 'X';
	k_mutex_lock(&plk, K_FOREVER);
	if (tx && is_sub(&m.p)) {
		if (now - net.tb_win >= 1000) {
			net.tb_win = now;
			net.tb_cnt = 0U;
		}
		ok = ++net.tb_cnt <= TX_MSGS_PER_S;
	} else {
		ok = ctl_proto_admit(&net.proto, &m.p, now);
	}
	k_mutex_unlock(&plk);
	if (!ok) {
		nc.dropped++;
		goto out;
	}
	m.len = (uint16_t)len;
	if (k_msgq_put(&ctl_q, &m, K_NO_WAIT) != 0) {
		nc.dropped++;
	}
out:
	net_pkt_unref(p);
}

static int open_ctx(net_sa_family_t family)
{
	struct net_context *ctx;
	int ret = net_context_get(family, NET_SOCK_DGRAM, NET_IPPROTO_UDP, &ctx);

	if (ret != 0) {
		return ret;
	}
	if (family == NET_AF_INET6) {
		struct net_sockaddr_in6 a = {.sin6_family = NET_AF_INET6, .sin6_port = net_htons(TX_CTL_PORT)};

		ret = net_context_bind(ctx, (struct net_sockaddr *)&a, sizeof(a));
	} else {
		struct net_sockaddr_in a = {.sin_family = NET_AF_INET, .sin_port = net_htons(TX_CTL_PORT)};

		ret = net_context_bind(ctx, (struct net_sockaddr *)&a, sizeof(a));
	}
	if (ret == 0) {
		ret = net_context_recv(ctx, recv_cb, K_NO_WAIT, NULL);
	}
	return ret;
}

static void log_peer(const char *what, const struct peer *p)
{
	char s[NET_INET6_ADDRSTRLEN];

	(void)net_addr_ntop(p->v6 ? NET_AF_INET6 : NET_AF_INET, p->addr, s, sizeof(s));
	LOG_INF("%s %s port %u", what, s, p->port);
}

/* ---- the run ---- */

/* The run takes no more data; a running stream drains (or, before its engine started, ends). */
static void run_end_request(void)
{
	atomic_set(&hk.accept, 0);
	if (atomic_get(&run_state) == RUN_ON) {
		(void)tx_stream_stop(false);
	}
}

/* The stream's control thread, at the end of a run. A run that ends before op_start() has finished
 * (starting) is told to it. */
static void net_done(const struct tx_stats *s)
{
	k_spinlock_key_t key;

	atomic_set(&hk.accept, 0);
	tx_shell_done(s);
	key = k_spin_lock(&rlk);
	atomic_set(&ended_id, (atomic_val_t)s->status.session);
	if (starting) {
		done_early = true;
	} else {
		atomic_set(&run_state, atomic_get(&owned) != 0 ? RUN_ENDED : RUN_NONE);
		atomic_set(&ended_req, 1);
	}
	k_spin_unlock(&rlk, key);
}

static bool op_rf_ok(void *ctx, uint8_t rate)
{
	ARG_UNUSED(ctx);
	return tx_net_armed_s() != 0U && tx_shell_inited() && esp_sdr_ready &&
	       txs_rf_ok(esp_sdr_get_freq(), esp_sdr_get_freq_offset(), RF_K, rate);
}

/* Nothing of the session changes until the stream has started: a refused start leaves the last run as it was. */
static int op_start(void *ctx, const struct txp_go *go, uint32_t id, bool rf)
{
	const bool verify = (go->flags & TXP_GO_VERIFY) != 0U;
	struct tx_cfg c = {.src = TX_SRC_NET, .stream_id = id, .msps = go->rate_msps,
			   .seconds = MIN(go->seconds != 0U ? go->seconds : MAX_RUN_S, MAX_RUN_S), .rf = rf,
			   .verify = verify, .fmt = go->fmt == TXP_FMT_IQ8 ? TXS_FMT_IQ8 : TXS_FMT_WORD32};
	const struct peer *p = &net.sub;
	k_spinlock_key_t key;
	int ret;

	ARG_UNUSED(ctx);
	if (IS_ENABLED(CONFIG_APP_TXS_INJECT)) {
		/* the injector build has two receive buffers less: the hold of tx_hold.h does not fit */
		return -ENOTSUP;
	}
	tx_shell_cfg(&c);
	if (verify) {
		/* the bank is read back against the board's own tone: vrt_tx --source board sends it; the tables are
		 * set and the run started under the tone lock, so a `tx tone` cannot set others in between */
		tx_tone_lock();
		if (tx_stream_active()) {
			tx_tone_unlock();
			return -EBUSY;
		}
		tx_tone_set(TXTG_K, TXTG_AMP);
	}
	key = k_spin_lock(&rlk);
	starting = true;
	done_early = false;
	k_spin_unlock(&rlk, key);
	run_cyc = k_cycle_get_32();
	ret = tx_stream_start(&c, net_done);
	if (verify) {
		tx_tone_unlock();
	}
	key = k_spin_lock(&rlk);
	starting = false;
	if (ret != 0) {
		/* the end of the last run (still tearing down: -EBUSY) came while this start looked: it is that run's */
		if (done_early) {
			atomic_set(&run_state, atomic_get(&owned) != 0 ? RUN_ENDED : RUN_NONE);
			atomic_set(&ended_req, 1);
			done_early = false;
		}
		k_spin_unlock(&rlk, key);
		return ret;
	}
	/* what the hook compares with (it takes nothing yet) */
	memset(hk.f.src, 0, sizeof(hk.f.src));
	memset(hk.f.dst, 0, sizeof(hk.f.dst));
	hk.f.v6 = p->v6;
	memcpy(hk.f.src, p->addr, p->v6 ? 16 : 4);
	memcpy(hk.f.dst, p->local, p->v6 ? 16 : 4);
	hk.f.sport = p->port;
	txp_put32(hk.f.sid, id);
	net.stream_id = id;
	net.rf = rf;
	net.rf_cut = false;
	atomic_inc(&run_gen);
	atomic_set(&owned, 1);
	if (done_early) {
		atomic_set(&run_state, RUN_ENDED);
		atomic_set(&ended_req, 1);
	} else {
		atomic_set(&run_state, RUN_ON);
		atomic_set(&hk.accept, 1);
	}
	k_spin_unlock(&rlk, key);
	return 0;
}

static void op_stop(void *ctx)
{
	ARG_UNUSED(ctx);
	run_end_request();
}

static uint32_t op_rand(void *ctx)
{
	ARG_UNUSED(ctx);
	return sys_rand32_get();
}

static const struct tx_sess_ops sess_ops = {.rf_ok = op_rf_ok, .start = op_start, .stop = op_stop,
					    .rand32 = op_rand};

/* The subscriber is gone or replaced (control thread, `plk` held): nothing of its run goes on. */
static void sub_off(void)
{
	k_spinlock_key_t key;

	run_end_request();
	/* the arming was for this subscriber */
	tx_net_disarm();
	atomic_set(&owned, 0);
	if (atomic_get(&run_state) == RUN_ENDED) {
		atomic_set(&run_state, RUN_NONE);
	}
	key = k_spin_lock(&tlk);
	net.tmpl_ok = false;
	k_spin_unlock(&tlk, key);
	atomic_set(&live, 0);
	tx_sess_reset(&nc.sess);
}

static void sub_on(const struct peer *p)
{
	k_spinlock_key_t key;
	struct nf_tmpl t;

	const atomic_val_t gen = atomic_inc(&sub_gen) + 1;

	ARG_UNUSED(gen);
	/* an arming is for the subscriber that was there when it was typed: a new one needs `tx arm` again */
	tx_net_disarm();
	net.sub = *p;
	nf_tmpl_init(&t, p->v6, p->mac, hk.f.mac, p->local, p->addr, TX_CTL_PORT, p->port);
	key = k_spin_lock(&tlk);
	net.tmpl = t;
	net.tmpl_ok = true;
	k_spin_unlock(&tlk, key);
	atomic_set(&live, 1);
	k_sem_give(&st_wake);
}

/* The protocol decided: apply it. */
static void apply_event(const struct ctl_out *o, const struct peer *p)
{
	switch (o->evt) {
	case CTL_EVT_SUBSCRIBED:
	case CTL_EVT_TAKEOVER:
		if (atomic_get(&live) != 0) {
			sub_off();
		}
		sub_on(p);
		log_peer(o->evt == CTL_EVT_TAKEOVER ? "subscriber replaced by" : "subscriber", p);
		break;
	case CTL_EVT_STOPPED:
		log_peer("stop from", p);
		sub_off();
		break;
	default:
		break;
	}
}

/* The stream's report of the end of its run, into the session (control thread). */
static void sync_ended(void)
{
	if (atomic_cas(&ended_req, 1, 0)) {
		const uint32_t id = (uint32_t)atomic_get(&ended_id);

		/* the end of an earlier run is not the end of this one */
		if (id == 0U || id == net.stream_id) {
			tx_sess_ended(&nc.sess);
		}
	}
}

static void handle_tx(const struct ctlmsg *m, char kind, int64_t now)
{
	struct txp_ak ak;
	uint8_t out[TXP_AK_BYTES];
	bool sub, ok;

	sync_ended();
	k_mutex_lock(&plk, K_FOREVER);
	ok = cookie_ok(&m->p, m->data + 4, now);
	sub = is_sub(&m->p);
	k_mutex_unlock(&plk);
	if (!ok) {
		return;
	}
	if (kind == 'P') {
		struct txp_ping pg;
		const atomic_val_t h = atomic_get(&ping_head);

		if (!sub || txp_pg_get(&pg, m->data, m->len) != 0) {
			return;
		}
		nc.ping++;
		if (h - atomic_get(&ping_tail) >= (atomic_val_t)PINGS) {
			nc.ping_drop++;
			return;
		}
		net.pings[h % PINGS] = (struct ping){.seq = pg.seq, .t_ns = pg.t_ns, .b_ns = m->ns};
		atomic_set(&ping_head, h + 1);
		k_sem_give(&st_wake);
		return;
	}
	if (kind == 'G') {
		struct txp_go go;

		if (txp_go_get(&go, m->data, m->len) != 0) {
			return;
		}
		nc.go++;
		if (!sub) {
			memset(&ak, 0, sizeof(ak));
			ak.nonce = go.nonce;
			ak.code = TXP_NOT_SUB;
		} else {
			tx_sess_go(&nc.sess, &sess_ops, NULL, &go, &ak);
			if (ak.code == TXP_OK) {
				nc.go_ok++;
				LOG_INF("run: %u MS/s %s%s, stream ID 0x%08x", go.rate_msps,
					go.fmt == TXP_FMT_IQ8 ? "iq8" : "w32", ak.flags != 0U ? ", RF" : "",
					ak.stream_id);
			} else {
				nc.go_refused++;
				LOG_INF("run refused: code %u", ak.code);
			}
		}
	} else {
		struct txp_en en;

		if (txp_en_get(&en, m->data, m->len) != 0) {
			return;
		}
		nc.en++;
		if (!sub) {
			memset(&ak, 0, sizeof(ak));
			ak.nonce = en.nonce;
			ak.code = TXP_NOT_SUB;
		} else {
			tx_sess_en(&nc.sess, &sess_ops, NULL, &en, &ak);
		}
	}
	(void)txp_ak_put(out, sizeof(out), &ak);
	(void)net_send(&m->p, out, sizeof(out));
}

static void handle(const struct ctlmsg *m)
{
	const int64_t now = k_uptime_get();
	const char kind = txp_kind(m->data, m->len);
	struct ctl_out o;

	nc.msgs++;
	if (kind == 'G' || kind == 'E' || kind == 'P') {
		handle_tx(m, kind, now);
		return;
	}
	k_mutex_lock(&plk, K_FOREVER);
	ctl_proto_dgram(&net.proto, &m->p, m->data, m->len, now, &o);
	apply_event(&o, &m->p);
	k_mutex_unlock(&plk);
	if (o.act == CTL_REPLY) {
		nc.vrt_reply++;
		/* a VRTK that could not go is owed: the next renewal brings it */
		if (net_send(&m->p, o.reply, o.reply_len) != 0 && o.reply_len == 4U && o.reply[3] == 'K') {
			net.ack_owed = true;
			net.ack_owed_peer = m->p;
		}
		return;
	}
	if (o.evt == CTL_EVT_RENEWED && net.ack_owed && ctl_peer_same(&net.ack_owed_peer, &m->p) &&
	    net_send(&m->p, "VRTK", 4) == 0) {
		net.ack_owed = false;
	}
}

/* Every pass of the control thread: the 5 s silence, a lost address, the end of a run, the arming. */
static void periodic(void)
{
	bool gone;

	sync_ended();
	k_mutex_lock(&plk, K_FOREVER);
	gone = ctl_proto_expire(&net.proto, k_uptime_get());
	if (net.proto.live && !addr_local_ok(&net.proto.sub)) {
		LOG_WRN("the address of the run is gone: stopped");
		net.proto.live = false;
		gone = true;
	}
	if (gone) {
		LOG_INF("no renewal for %d s (or its address went): session ended", SILENT_MS / 1000);
		sub_off();
	}
	k_mutex_unlock(&plk);
	/* an RF run does not outlive the arming */
	if (atomic_get(&run_state) == RUN_ON && net.rf && !net.rf_cut && tx_net_armed_s() == 0U) {
		LOG_WRN("disarmed or the arming ran out: the RF run drains");
		net.rf_cut = true;
		nc.rf_stops++;
		run_end_request();
	}
}

static void ctl_entry(void *a, void *b, void *c)
{
	struct ctlmsg m;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		if (k_msgq_get(&ctl_q, &m, K_MSEC(100)) == 0) {
			handle(&m);
		}
		periodic();
	}
}

/* ---- arming, init, report ---- */

/* The arming belongs to one subscriber: `tx arm` with a subscriber is for that one, without it for the next
 * one that subscribes; a takeover, a new subscriber or the end of the subscription disarms. */
/* Under plk, so that a takeover cannot come between the look at the subscriber and the arming. */
int tx_net_arm(const struct shell *sh, uint32_t minutes)
{
	struct peer p;
	char addr[NET_INET6_ADDRSTRLEN] = "-";

	k_mutex_lock(&plk, K_FOREVER);
	if (atomic_get(&live) == 0) {
		k_mutex_unlock(&plk);
		shell_error(sh, "tx arm: no subscriber (start vrt_tx --rf first: it waits for the arming)");
		return -ENOENT;
	}
	p = net.sub;
	atomic_set(&arm_gen, atomic_get(&sub_gen));
	atomic_set(&arm_until_s, (atomic_val_t)((uint32_t)(k_uptime_get() / 1000) +
						MIN(minutes, MAX_ARM_MIN) * 60U));
	k_mutex_unlock(&plk);
	(void)net_addr_ntop(p.v6 ? NET_AF_INET6 : NET_AF_INET, p.addr, addr, sizeof(addr));
	shell_print(sh, "tx arm: armed for %u minutes for the subscriber %s port %u, MAC %02x:%02x:%02x:%02x:%02x:%02x"
		    " (a takeover, a new subscriber or the end of the subscription disarms)", MIN(minutes, MAX_ARM_MIN),
		    addr, p.port, p.mac[0], p.mac[1], p.mac[2], p.mac[3], p.mac[4], p.mac[5]);
	return 0;
}

void tx_net_disarm(void)
{
	atomic_set(&arm_until_s, 0);
	atomic_set(&arm_gen, 0);
}

static uint32_t arm_left_s(void)
{
	const uint32_t until = (uint32_t)atomic_get(&arm_until_s), now = (uint32_t)(k_uptime_get() / 1000);

	return until > now ? until - now : 0U;
}

/* seconds left of an arming that is for the current subscriber */
uint32_t tx_net_armed_s(void)
{
	return atomic_get(&live) != 0 && atomic_get(&arm_gen) == atomic_get(&sub_gen) ? arm_left_s() : 0U;
}

int tx_net_init(const struct device *eth, struct net_if *netif)
{
	uint8_t key[16];
	k_tid_t t;
	int ret;

	eth_dev = eth;
	iface = netif;
	memset(&net, 0, sizeof(net));
	memset(&nc, 0, sizeof(nc));
	memcpy(hk.f.mac, net_if_get_link_addr(netif)->addr, 6);
	atomic_set(&live, 0);
	atomic_set(&run_state, RUN_NONE);
	atomic_set(&owned, 0);
	k_mutex_init(&plk);
	k_sem_init(&st_wake, 0, 1);
	k_msgq_init(&ctl_q, ctl_q_buf, sizeof(struct ctlmsg), 4);
	sys_rand_get(key, sizeof(key));
	ctl_proto_init(&net.proto, key, (uint32_t)net_if_get_by_iface(netif), SILENT_MS);
	tx_sess_init(&nc.sess, TX_BUFFERS, TXS_GROUP_WORDS);
	ret = open_ctx(NET_AF_INET);
	if (ret != 0) {
		LOG_ERR("IPv4 control port: %d", ret);
	}
	ret = open_ctx(NET_AF_INET6);
	if (ret != 0) {
		LOG_ERR("IPv6 control port: %d", ret);
	}
	t = k_thread_create(&ctl_thread, ctl_stack, K_THREAD_STACK_SIZEOF(ctl_stack), ctl_entry, NULL, NULL,
			    NULL, 10, 0, K_FOREVER);
	k_thread_name_set(t, "tx_ctl");
	(void)k_thread_cpu_pin(t, 0);
	k_thread_start(t);
	t = k_thread_create(&st_thread, st_stack, K_THREAD_STACK_SIZEOF(st_stack), st_entry, NULL, NULL, NULL,
			    ST_PRIO, 0, K_FOREVER);
	k_thread_name_set(t, "tx_stat");
	(void)k_thread_cpu_pin(t, 0);
	k_thread_start(t);
	ret = dwmac_rx_hook_set(eth, tx_hook, NULL, NULL);
	if (ret != 0) {
		LOG_ERR("receive hook: %d", ret);
	}
	LOG_INF("transmit stream control on UDP %u, samples on UDP %u", TX_CTL_PORT, TX_PORT);
	return ret;
}

void tx_net_print(const struct shell *sh)
{
	static const char *const states[] = {"no run", "run", "ended"};
	size_t cu = 0U, su = 0U;
	char addr[NET_INET6_ADDRSTRLEN] = "-";
	const struct peer *p = &net.sub;
	const int st = (int)atomic_get(&run_state);
	const uint32_t armed = tx_net_armed_s(), n = MAX(net.st_cyc_n, 1U);
	struct tx_stats ts;

	if (atomic_get(&live) != 0) {
		(void)net_addr_ntop(p->v6 ? NET_AF_INET6 : NET_AF_INET, p->addr, addr, sizeof(addr));
	}
	tx_stream_stats(&ts);
	shell_print(sh, "tx net: subscriber %s port %u, %s, stream ID 0x%08x, RF %s, %s", addr,
		    atomic_get(&live) != 0 ? p->port : 0U, states[MIN(st, 2)], net.stream_id,
		    net.rf && st != RUN_NONE ? "on" : "off", armed != 0U ? "armed for this subscriber" : "not armed");
	if (armed != 0U) {
		shell_print(sh, "tx net: %u s of the arming left", armed);
	}
	shell_print(sh, "tx net: kernel of the last run %s (net runs take PIE when built, else fast; "
		    "`tx kern` overrides); verify: read back %llu words, %llu wrong, %llu zero",
		    ts.kern != NULL ? ts.kern : "-", (unsigned long long)ts.ver_words,
		    (unsigned long long)ts.ver_bad, (unsigned long long)ts.ver_zero);
	shell_print(sh, "tx net: hook: offered %u frames, matched %u (to the stream), dropped for: no run %u, "
		    "source %u, stream ID %u, length %u, stream busy %u; accepting %d", hk.offered, hk.matched,
		    hk.idle, hk.bad_src, hk.bad_sid, hk.bad_len, hk.refused, (int)atomic_get(&hk.accept));
	shell_print(sh, "tx net: control: %u messages (dropped %u, rejected %u), TXGO %u (%u ok, %u refused), "
		    "TXEN %u, pings %u (answered %u, no room %u, no slot %u), RF runs cut %u",
		    nc.msgs, nc.dropped, nc.rejected, nc.go, nc.go_ok, nc.go_refused, nc.en, nc.ping,
		    net.pong_sent, nc.ping_drop, net.pong_busy, nc.rf_stops);
	shell_print(sh, "tx net: statuses sent %u in all (%u at a flip in this run), no slot %u, record not read %u, not queued %u",
		    net.st_sent, net.st_on_flip, net.st_busy, net.st_fail_r, net.st_fail_q);
	shell_print(sh, "tx net: status cycles on average %u: header %u, record %u, seal %u, queue %u; worst %u "
		    "(wall clock: an interrupt in between counts)", (uint32_t)(net.st_cyc_sum / n),
		    (uint32_t)(net.c_hdr / n), (uint32_t)(net.c_rec / n), (uint32_t)(net.c_seal / n),
		    (uint32_t)(net.c_q / n), net.st_cyc_max);
	if (ctl_thread.stack_info.size != 0U) {
		(void)k_thread_stack_space_get(&ctl_thread, &cu);
		(void)k_thread_stack_space_get(&st_thread, &su);
	}
	shell_print(sh, "tx net: flip to status sent (the record published after it, queued): %u us on average, %u us at most, over %u",
		    net.st_on_flip != 0U ? (uint32_t)(net.fl_sum_us / net.st_on_flip) : 0U, net.fl_max_us, net.st_on_flip);
	shell_print(sh, "tx net: stack bytes never used: control thread %u of %u, status thread %u of %u",
		    (unsigned int)cu, (unsigned int)K_THREAD_STACK_SIZEOF(ctl_stack), (unsigned int)su,
		    (unsigned int)K_THREAD_STACK_SIZEOF(st_stack));
}
