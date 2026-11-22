/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The chain of raw frames. The MAC driver calls chain_hook() in the EMAC interrupt for every
 * frame; ours (EtherType 0x88B5, magic) are answered at once from the hook, up to the hop count
 * the frame carries. The seme's burst timer starts each burst. Hook and timer both run on CPU 0
 * and neither nests in the other, so the state needs no lock: a guard flag only counts the case
 * where that assumption breaks.
 *
 * Frame, after the 14 byte Ethernet header, little endian, 40 bytes (padded to 60 on the wire):
 *   0 u16 magic 0x7c1a   2 u8 version 1   3 u8 flags
 *   4 u32 seq            8 u32 reply_to   12 u16 hops left   14 u16 0
 *  16 u32 rx_seq  20 u32 rx_sec  24 u32 rx_subsec   (sender's rx stamp of its last received frame)
 *  28 u32 tx_seq  32 u32 tx_sec  36 u32 tx_subsec   (sender's tx stamp of its previous frame)
 * flags: 0 reply (direct reply to reply_to), 1 sender is seme, 2 rx stamp valid, 3 tx stamp valid.
 * Each node numbers its own frames.
 */
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/ethernet/eth_dwmac_fast.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <esp_sdr/esp_sdr_place.h>

#include "chain.h"

#define ETYPE      0x88b5
#define HDR_LEN    14
#define MAGIC      0x7c1a
#define VERSION    1
#define FRAME_LEN  60
#define MAX_HOPS   1024
/* No new burst while a chain frame went in or out less than this ago */
#define QUIET_US   1000

#define P_MAGIC    0
#define P_VER      2
#define P_FLAGS    3
#define P_SEQ      4
#define P_REPLY_TO 8
#define P_HOPS     12
#define P_RX       16   /* seq, sec, subsec */
#define P_TX       28
#define P_END      40

#define F_REPLY    BIT(0)
#define F_SEME     BIT(1)
#define F_RX_OK    BIT(2)
#define F_TX_OK    BIT(3)

#define RING_SLOTS CONFIG_APP_CHAIN_RING
BUILD_ASSERT((RING_SLOTS & (RING_SLOTS - 1)) == 0, "CONFIG_APP_CHAIN_RING: a power of two");
BUILD_ASSERT(CONFIG_NET_BUF_RX_COUNT > CONFIG_DWMAC_NB_RX_DESCS, "receive buffers");

/*
 * Local always-inline accessors: the Zephyr ones (sys_get_le32, atomic_get) are static inline
 * functions that the compiler leaves as calls, to a copy in flash text.
 */
static __always_inline uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static __always_inline uint16_t get_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static __always_inline uint16_t get_be16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

static __always_inline void put_le32(uint32_t v, uint8_t *p)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static __always_inline void put_le16(uint16_t v, uint8_t *p)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static __always_inline void put_be16(uint16_t v, uint8_t *p)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static __always_inline atomic_val_t aget(const atomic_t *a)
{
	return __atomic_load_n(a, __ATOMIC_ACQUIRE);
}

static __always_inline void aset(atomic_t *a, atomic_val_t v)
{
	__atomic_store_n(a, v, __ATOMIC_RELEASE);
}

enum { SLOT_FREE, SLOT_QUEUED, SLOT_DONE };

struct slot {
	uint8_t buf[FRAME_LEN] __aligned(4);
	struct dwmac_tx_stamp stamp;
	atomic_t state;
	uint32_t seq;
	bool harvested;
};

struct chain_counters chain_cnt;
const struct device *chain_eth;

static struct slot slots[CHAIN_TXN];
static struct chain_pair pair;
static uint32_t tx_seq;
static uint8_t own_mac[6];
static const uint8_t bcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
/* Destination of our frames. peer_state: PEER_NONE, PEER_BUSY (bytes being written), PEER_KNOWN.
 * The bytes are written only in PEER_BUSY and published by the store of PEER_KNOWN.
 */
enum { PEER_NONE, PEER_BUSY, PEER_KNOWN };
static uint8_t peer_mac[6];
static atomic_t peer_state = PEER_NONE;
static atomic_t busy;
static uint32_t last_active_cyc;  /* k_cycle_get_32() at the last chain frame in or out */
static uint32_t quiet_cyc;       /* a burst starts only after this long without a chain frame */
static atomic_t role = CHAIN_IDLE;
static atomic_t rounds = 1;
static atomic_t interval_us = 7812;
static struct k_timer burst_timer;

static struct chain_xchg ring[RING_SLOTS];
static atomic_t ring_head, ring_tail;

static ESP_SDR_CODE(EVENT, chain_slot_done) void chain_slot_done(void *arg)
{
	struct slot *s = arg;

	aset(&s->state, SLOT_DONE);
}

/* Own frames the MAC has stamped since the last call: the stamps go to the pairing records. */
static ESP_SDR_CODE(EVENT, chain_harvest) void chain_harvest(void)
{
	for (unsigned int i = 0; i < CHAIN_TXN; i++) {
		struct slot *s = &slots[i];

		if (aget(&s->state) != SLOT_DONE || s->harvested) {
			continue;
		}
		s->harvested = true;
		if (s->stamp.valid) {
			struct chain_stamp st = {.sec = s->stamp.sec, .subsec = s->stamp.subsec,
						 .valid = true};

			chain_tx_stamp(&pair, s->seq, &st);
		} else {
			chain_cnt.tx_stamp_missing++;
		}
	}
}

static __always_inline void put_stamp(uint8_t *p, uint32_t seq, const struct chain_stamp *st)
{
	put_le32(seq, p);
	put_le32(st->sec, p + 4);
	put_le32(st->subsec, p + 8);
}

/* Queue the next own frame. It carries the rx stamp of the last received frame and the tx stamp of
 * the previous own frame.
 */
static ESP_SDR_CODE(EVENT, chain_send) bool chain_send(bool reply, uint32_t reply_to,
						       uint16_t hops)
{
	struct slot *s = &slots[tx_seq & (CHAIN_TXN - 1U)];
	struct dwmac_tx_frame f;
	struct chain_stamp pt;
	uint8_t *b = s->buf;
	uint8_t *p = b + HDR_LEN;
	uint8_t flags = 0;

	if (aget(&s->state) == SLOT_QUEUED) {
		chain_cnt.tx_busy++;
		return false;
	}
	if (aget(&peer_state) != PEER_KNOWN) {
		chain_cnt.tx_no_peer++;
		return false;
	}
	memset(b, 0, FRAME_LEN);
	memcpy(b, peer_mac, 6);
	if (aget(&peer_state) != PEER_KNOWN) {
		/* the thread changed the peer while we copied */
		chain_cnt.tx_no_peer++;
		return false;
	}
	memcpy(b + 6, own_mac, 6);
	put_be16(ETYPE, b + 12);
	put_le16(MAGIC, p + P_MAGIC);
	p[P_VER] = VERSION;
	if (reply) {
		flags |= F_REPLY;
	}
	if (aget(&role) == CHAIN_SEME) {
		flags |= F_SEME;
	}
	put_le32(tx_seq, p + P_SEQ);
	put_le32(reply_to, p + P_REPLY_TO);
	put_le16(hops, p + P_HOPS);
	if (pair.last.valid && pair.last.rx.valid) {
		put_stamp(p + P_RX, pair.last.seq, &pair.last.rx);
		flags |= F_RX_OK;
	}
	if (chain_tx_get(&pair, tx_seq - 1U, &pt)) {
		put_stamp(p + P_TX, tx_seq - 1U, &pt);
		flags |= F_TX_OK;
	}
	p[P_FLAGS] = flags;

	s->stamp.valid = false;
	s->seq = tx_seq;
	s->harvested = false;
	aset(&s->state, SLOT_QUEUED);
	chain_tx_register(&pair, tx_seq, reply_to, reply);
	f = (struct dwmac_tx_frame){.b1 = b, .l1 = FRAME_LEN, .b2 = NULL, .l2 = 0,
				    .done = chain_slot_done, .arg = s, .stamp = &s->stamp};
	if (dwmac_send_frames(chain_eth, &f, 1) != 1) {
		aset(&s->state, SLOT_FREE);
		chain_cnt.tx_fail++;
		return false;
	}
	tx_seq++;
	chain_cnt.tx_sent++;
	last_active_cyc = k_cycle_get_32();
	return true;
}

static __always_inline void ring_push(const struct chain_xchg *x)
{
	uint32_t h = (uint32_t)aget(&ring_head);

	if (h - (uint32_t)aget(&ring_tail) >= RING_SLOTS) {
		chain_cnt.ring_overflow++;
		return;
	}
	ring[h & (RING_SLOTS - 1U)] = *x;
	aset(&ring_head, (atomic_val_t)(h + 1U));
}

bool chain_ring_pop(struct chain_xchg *x)
{
	uint32_t t = (uint32_t)aget(&ring_tail);

	if (t == (uint32_t)aget(&ring_head)) {
		return false;
	}
	*x = ring[t & (RING_SLOTS - 1U)];
	aset(&ring_tail, (atomic_val_t)(t + 1U));
	return true;
}

static __always_inline void get_stamp(const uint8_t *p, uint32_t *seq, struct chain_stamp *st, bool ok)
{
	*seq = get_le32(p);
	st->sec = get_le32(p + 4);
	st->subsec = get_le32(p + 8);
	st->valid = ok;
}

static ESP_SDR_CODE(EVENT, chain_hook) bool chain_hook(const struct device *dev,
						       const struct dwmac_rx_frame *f, void *user)
{
	const uint8_t *p;
	struct chain_rx_in in;
	struct chain_rx_res res;
	uint8_t flags;
	uint16_t hops;

	ARG_UNUSED(dev);
	ARG_UNUSED(user);
	if (f->len < HDR_LEN + P_END || get_be16(f->data + 12) != ETYPE) {
		return false;
	}
	p = f->data + HDR_LEN;
	if (get_le16(p + P_MAGIC) != MAGIC || p[P_VER] != VERSION ||
	    memcmp(f->data + 6, own_mac, 6) == 0) {
		return false;
	}
	/* for us or broadcast (an older image still sends those) */
	if (memcmp(f->data, own_mac, 6) != 0 && memcmp(f->data, bcast_mac, 6) != 0) {
		return false;
	}
	/* the uke learns the peer from the first valid frame, and keeps it */
	if (atomic_cas(&peer_state, PEER_NONE, PEER_BUSY)) {
		memcpy(peer_mac, f->data + 6, 6);
		aset(&peer_state, PEER_KNOWN);
	}
	if (!atomic_cas(&busy, 0, 1)) {
		chain_cnt.reentry++;
		return false;
	}
	chain_harvest();
	flags = p[P_FLAGS];
	in.seq = get_le32(p + P_SEQ);
	in.is_reply = (flags & F_REPLY) != 0;
	in.reply_to = get_le32(p + P_REPLY_TO);
	get_stamp(p + P_RX, &in.rx_seq, &in.rx, (flags & F_RX_OK) != 0);
	get_stamp(p + P_TX, &in.tx_seq, &in.tx, (flags & F_TX_OK) != 0);
	in.local_rx = (struct chain_stamp){.sec = f->timestamp_sec, .subsec = f->timestamp_subsec,
					   .valid = f->timestamp_valid};
	hops = get_le16(p + P_HOPS);

	if ((flags & F_SEME) != 0) {
		/* two semes: the lower MAC stays */
		if (aget(&role) == CHAIN_SEME && memcmp(f->data + 6, own_mac, 6) < 0) {
			k_timer_stop(&burst_timer);
			aset(&role, CHAIN_UKE);
		} else if (aget(&role) == CHAIN_IDLE) {
			aset(&role, CHAIN_UKE);
		}
	}
	chain_cnt.rx_chain++;
	last_active_cyc = k_cycle_get_32();
	if (!f->timestamp_valid) {
		chain_cnt.rx_nostamp++;
	}
	chain_rx(&pair, &in, &res);
	chain_cnt.rx_lost += res.lost;
	if (res.stale) {
		chain_cnt.rx_stale++;
	} else {
		for (unsigned int i = 0; i < res.n; i++) {
			ring_push(&res.x[i]);
		}
		chain_cnt.exchanges += res.n;
		if (hops > 0) {
			chain_send(true, in.seq, (hops > MAX_HOPS ? MAX_HOPS : hops) - 1U);
		} else {
			chain_cnt.chain_end++;
		}
	}
	aset(&busy, 0);
	net_buf_unref(f->buf);
	return true;
}

/* Tick of the seme: the first frame of a burst. */
static ESP_SDR_CODE(EVENT, chain_burst) void chain_burst(struct k_timer *t)
{
	ARG_UNUSED(t);
	if (!atomic_cas(&busy, 0, 1)) {
		chain_cnt.reentry++;
		return;
	}
	if (k_cycle_get_32() - last_active_cyc < quiet_cyc) {
		chain_cnt.burst_skipped++;
		aset(&busy, 0);
		return;
	}
	chain_harvest();
	if (chain_send(false, 0, 2U * (uint32_t)aget(&rounds) - 1U)) {
		chain_cnt.bursts++;
	}
	aset(&busy, 0);
}

int chain_init(const struct device *eth, struct net_if *iface)
{
	chain_eth = eth;
	memcpy(own_mac, net_if_get_link_addr(iface)->addr, 6);
	chain_pair_init(&pair, CHAIN_DIGITAL);
	quiet_cyc = k_us_to_cyc_ceil32(QUIET_US);
	k_timer_init(&burst_timer, chain_burst, NULL);
	return dwmac_rx_hook_set(eth, chain_hook, NULL, NULL);
}

int chain_start(uint32_t us, uint32_t r)
{
	if (aget(&peer_state) != PEER_KNOWN) {
		return -ENOTCONN;
	}
	if (us != 0) {
		atomic_set(&interval_us, (atomic_val_t)us);
	}
	if (r != 0) {
		atomic_set(&rounds, (atomic_val_t)r);
	}
	atomic_set(&role, CHAIN_SEME);
	k_timer_start(&burst_timer, K_USEC((uint32_t)atomic_get(&interval_us)),
		      K_USEC((uint32_t)atomic_get(&interval_us)));
	return 0;
}

/* Take the peer for writing. The hook (ISR) never waits: it only tries PEER_NONE -> PEER_BUSY. */
static void peer_take(void)
{
	atomic_val_t old;

	do {
		old = aget(&peer_state);
	} while (old == PEER_BUSY || !atomic_cas(&peer_state, old, PEER_BUSY));
}

int chain_peer_set(const uint8_t mac[6])
{
	if (aget(&role) != CHAIN_IDLE) {
		return -EBUSY;
	}
	peer_take();
	memcpy(peer_mac, mac, 6);
	aset(&peer_state, PEER_KNOWN);
	return 0;
}

int chain_peer_clear(void)
{
	if (aget(&role) != CHAIN_IDLE) {
		return -EBUSY;
	}
	peer_take();
	aset(&peer_state, PEER_NONE);
	return 0;
}

bool chain_peer_get(uint8_t mac[6])
{
	if (aget(&peer_state) != PEER_KNOWN) {
		return false;
	}
	memcpy(mac, peer_mac, 6);
	return aget(&peer_state) == PEER_KNOWN;
}

void chain_stop(void)
{
	k_timer_stop(&burst_timer);
	atomic_set(&role, CHAIN_IDLE);
}

enum chain_role chain_role_get(void)
{
	return (enum chain_role)atomic_get(&role);
}

const uint8_t *chain_mac(void)
{
	return own_mac;
}

uint32_t chain_interval_us(void)
{
	return (uint32_t)atomic_get(&interval_us);
}

uint32_t chain_rounds(void)
{
	return (uint32_t)atomic_get(&rounds);
}
