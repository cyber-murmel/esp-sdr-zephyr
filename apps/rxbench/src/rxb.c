/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The receive hook (interrupt), the ring, and the consumer thread. See ../README.rst.
 *
 * Hand-over: the MAC driver calls rxb_hook() for each frame in one descriptor. A frame for the UDP
 * port is pushed on a lock-free single producer ring (the interrupt is the only producer) and
 * claimed, the buffer going back to the receive pool from the consumer; rxb_batch() wakes the
 * consumer once per interrupt. Frames not for the port are not claimed: the stack gets them as it
 * always does. A claimed frame is never passed on, even one the ring has no room for.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/ethernet/eth_dwmac_fast.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net_buf.h>

#include <esp_sdr/esp_sdr_place.h>

#include "rxb.h"
#include "rxb_parse.h"
#include "rxb_proto.h"
#include "rxb_ring.h"

LOG_MODULE_REGISTER(rxb, LOG_LEVEL_INF);

#define RING_SLOTS CONFIG_APP_RXB_RING
#define CONSUMER_STACK 2048
/* Items the consumer takes between two commits of its counters */
#define COMMIT_EVERY 32U

BUILD_ASSERT((RING_SLOTS & (RING_SLOTS - 1)) == 0, "CONFIG_APP_RXB_RING must be a power of two");
/* the MAC always holds DMA ring - 1 buffers; the ring and the stack's own need come on top */
BUILD_ASSERT(CONFIG_NET_BUF_RX_COUNT >=
	     CONFIG_DWMAC_NB_RX_DESCS + RING_SLOTS + CONFIG_APP_RXB_STACK_HEADROOM,
	     "CONFIG_NET_BUF_RX_COUNT: DMA descriptors + ring + headroom for the stack");

static struct rxb_item items[RING_SLOTS];
static struct rxb_ring ring;
static struct net_if *board_iface;

static K_THREAD_STACK_DEFINE(consumer_stack, CONSUMER_STACK);
static struct k_thread consumer_thread;
static K_SEM_DEFINE(wake, 0, 1);

/*
 * The interrupt writes the hook counts (hc) and the consumer its totals, both only ever counting
 * up. A start does not zero anything: it takes a baseline of both under the lock (nothing the
 * interrupt does needs the lock), and a snapshot is the difference.
 */
static atomic_t running;
static atomic_t full_check;
static atomic_t generation;	/* bumped by start: the consumer forgets the sequence */
static struct rxb_hook_counts hc, hc_base;
static struct rxb_totals totals, totals_base;
static struct rxb_seq pos;	/* the consumer's position, copied at each commit */
static uint32_t ring_high;
static uint64_t cons_cycles, cons_cycles_base;
static uint32_t cons_pkts, cons_pkts_base;
static int64_t start_ms;
static struct k_spinlock lock;

/* In the receive interrupt, for every good frame in one descriptor: take it or leave it. */
static ESP_SDR_CODE(EVENT, rxb_hook) bool rxb_hook(const struct device *dev,
						   const struct dwmac_rx_frame *f, void *user)
{
	struct rxb_hit hit;
	struct rxb_item it;
	int ver = rxb_parse(f->data, f->len, net_if_get_link_addr(board_iface)->addr, RXB_PORT, &hit);

	ARG_UNUSED(dev);
	ARG_UNUSED(user);
	if (ver == 0) {
		return false;
	}
	if (atomic_get(&running) == 0) {
		hc.idle++;
		net_buf_unref(f->buf);
		return true;
	}
	it.buf = f->buf;
	it.len = f->len;
	it.off = hit.off;
	it.plen = hit.plen;
	if (!rxb_ring_push(&ring, &it)) {
		hc.ring_full++;
		net_buf_unref(f->buf);
		return true;
	}
	hc.taken++;
	if (ver == 4) {
		hc.ip4++;
	} else {
		hc.ip6++;
	}
	return true;
}

/* After the interrupt's frames: wake the consumer once. */
static ESP_SDR_CODE(EVENT, rxb_batch) void rxb_batch(const struct device *dev, unsigned int n,
						     void *user)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(n);
	ARG_UNUSED(user);
	k_sem_give(&wake);
}

static ESP_SDR_CODE(EVENT, rxb_consume) __noinline void rxb_consume(struct rxb_seq *seq,
								    atomic_val_t *gen)
{
	struct rxb_totals loc;
	struct rxb_item it;
	uint32_t t0;
	uint64_t cycles = 0U;
	uint32_t waiting = 0U;
	bool full = atomic_get(&full_check) != 0;
	atomic_val_t now = atomic_get(&generation);
	unsigned int n = 0U;

	if (now != *gen) {
		/* a new start: the sequence begins again with the next packet */
		*gen = now;
		rxb_seq_reset(seq);
	}
	memset(&loc, 0, sizeof(loc));
	while (n < COMMIT_EVERY) {
		const uint8_t *frame;
		uint32_t s, cnt = rxb_ring_count(&ring);

		waiting = MAX(waiting, cnt);
		if (!rxb_ring_pop(&ring, &it)) {
			break;
		}
		frame = ((struct net_buf *)it.buf)->data;
		t0 = k_cycle_get_32();
		loc.pkts++;
		loc.frame_bytes += it.len;
		loc.payload_bytes += it.plen;
		if (rxb_check(frame + it.off, it.plen, full, &s)) {
			rxb_seq_track(seq, s, &loc.seq);
		} else {
			loc.bad++;
		}
		net_buf_unref(it.buf);
		cycles += k_cycle_get_32() - t0;
		n++;
	}
	if (n == 0U) {
		return;
	}
	K_SPINLOCK(&lock) {
		totals.pkts += loc.pkts;
		totals.frame_bytes += loc.frame_bytes;
		totals.payload_bytes += loc.payload_bytes;
		totals.bad += loc.bad;
		totals.seq.lost += loc.seq.lost;
		totals.seq.gaps += loc.seq.gaps;
		totals.seq.dups += loc.seq.dups;
		totals.seq.ooo += loc.seq.ooo;
		totals.seq.restarts += loc.seq.restarts;
		pos = *seq;
		ring_high = MAX(ring_high, waiting);
		cons_cycles += cycles;
		cons_pkts += n;
	}
}

static void consumer_entry(void *a, void *b, void *c)
{
	struct rxb_seq seq;
	atomic_val_t gen = atomic_get(&generation);

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	rxb_seq_reset(&seq);
	for (;;) {
		while (rxb_ring_count(&ring) != 0U) {
			rxb_consume(&seq, &gen);
		}
		/* An outage is no reason to forget the sequence: what did not come is lost. A
		 * sender that started again is seen by its number (rxb_seq.h).
		 */
		k_sem_take(&wake, K_FOREVER);
	}
}

int rxb_init(const struct device *eth, struct net_if *iface)
{
	int ret;

	board_iface = iface;
	rxb_ring_init(&ring, items, RING_SLOTS);

	/* created stopped, pinned, then started: a running thread's pin fails silently */
	k_thread_create(&consumer_thread, consumer_stack, K_THREAD_STACK_SIZEOF(consumer_stack),
			consumer_entry, NULL, NULL, NULL, K_PRIO_PREEMPT(CONFIG_APP_RXB_CONSUMER_PRIO),
			0, K_FOREVER);
	k_thread_name_set(&consumer_thread, "rxb_consumer");
#if defined(CONFIG_SMP) && defined(CONFIG_SCHED_CPU_MASK)
	if (CONFIG_APP_RXB_CONSUMER_CPU >= 0 && arch_num_cpus() > 1) {
		ret = k_thread_cpu_pin(&consumer_thread, CONFIG_APP_RXB_CONSUMER_CPU);
		if (ret != 0) {
			LOG_ERR("pinning the consumer to CPU %d: %d", CONFIG_APP_RXB_CONSUMER_CPU,
				ret);
			return ret;
		}
	}
#endif
	k_thread_start(&consumer_thread);

	ret = dwmac_rx_hook_set(eth, rxb_hook, rxb_batch, NULL);
	if (ret != 0) {
		LOG_ERR("receive hook: %d", ret);
	}
	return ret;
}

void rxb_start(bool full)
{
	K_SPINLOCK(&lock) {
		totals_base = totals;
		hc_base = hc;
		cons_cycles_base = cons_cycles;
		cons_pkts_base = cons_pkts;
		ring_high = 0U;
		start_ms = k_uptime_get();
	}
	atomic_set(&full_check, full ? 1 : 0);
	atomic_inc(&generation);
	atomic_set(&running, 1);
	k_sem_give(&wake);
}

void rxb_stop(void)
{
	atomic_set(&running, 0);
}

void rxb_snapshot(struct rxb_snap *s)
{
	uint64_t cyc;
	uint32_t pk;

	s->running = atomic_get(&running) != 0;
	s->full_check = atomic_get(&full_check) != 0;
	s->ring_slots = RING_SLOTS;
	K_SPINLOCK(&lock) {
		s->start_ms = start_ms;
		s->t.pkts = totals.pkts - totals_base.pkts;
		s->t.frame_bytes = totals.frame_bytes - totals_base.frame_bytes;
		s->t.payload_bytes = totals.payload_bytes - totals_base.payload_bytes;
		s->t.bad = totals.bad - totals_base.bad;
		s->t.seq.lost = totals.seq.lost - totals_base.seq.lost;
		s->t.seq.gaps = totals.seq.gaps - totals_base.seq.gaps;
		s->t.seq.dups = totals.seq.dups - totals_base.seq.dups;
		s->t.seq.ooo = totals.seq.ooo - totals_base.seq.ooo;
		s->t.seq.restarts = totals.seq.restarts - totals_base.seq.restarts;
		s->pos = pos;
		s->h.taken = hc.taken - hc_base.taken;
		s->h.ip4 = hc.ip4 - hc_base.ip4;
		s->h.ip6 = hc.ip6 - hc_base.ip6;
		s->h.ring_full = hc.ring_full - hc_base.ring_full;
		s->h.idle = hc.idle - hc_base.idle;
		s->ring_high = ring_high;
		ring_high = 0U;
		cyc = cons_cycles - cons_cycles_base;
		pk = cons_pkts - cons_pkts_base;
	}
	s->consumer_us_per_pkt_x10 =
		pk == 0U ? 0U : (uint32_t)(cyc * 10U * USEC_PER_SEC / sys_clock_hw_cycles_per_sec() / pk);
}
