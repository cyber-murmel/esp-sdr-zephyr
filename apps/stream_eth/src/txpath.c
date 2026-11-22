/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The sender. It takes the finished cells (vrt_cells.h) in order and makes one frame of each:
 * Ethernet, IP, UDP and VRT headers from a template in a small buffer of its own, the cell's
 * samples and trailer behind it in place. The frames go to the MAC in batches, one descriptor
 * and two DMA buffers a frame, no net_pkt and no IP stack (dwmac_send_frames()); the MAC says
 * when the cell is free again. The stack's own traffic (ARP, ND, DHCP, ping) goes on beside it.
 *
 * Where the packets go is the subscriber's: its MAC, from the Ethernet header of the datagram that
 * asked, so there is nothing to resolve and a router in between is the next hop it came from.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/ethernet/eth_dwmac_fast.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <esp_sdr/esp_sdr_diag16.h>
#include <esp_sdr/esp_sdr_place.h>

#include "netframe.h"
#include "stream_eth.h"
#include "vrt_pkt.h"

LOG_MODULE_DECLARE(stream_eth, LOG_LEVEL_INF);

/* a slot is free once the MAC is done with its frame: no more frames wait than descriptors, and
 * a batch is built before it is submitted
 */
#define NHDR      (CONFIG_DWMAC_NB_TX_DESCS + CONFIG_APP_VRT_BATCH + 1)
#define HDR_SLOT  96U
#define BATCH     CONFIG_APP_VRT_BATCH
#define SENDER_STACK 3072
#define CTX_PERIOD_MS 1000

/* a frame the MAC has not reaped (coalesced interrupts) holds its cell: the cells must cover it */
BUILD_ASSERT(CONFIG_ETH_DWMAC_TX_IRQ_EVERY * 2 <= CONFIG_APP_VRT_CELLS,
	     "the transmit interrupt interval must be at most half the cells");

static struct net_if *iface;
static const struct device *eth_dev;

/* Who gets the stream, written by the control thread. */
static struct k_spinlock sub_lock;
static struct peer sub;
static bool sub_on;
static atomic_t sub_gen;
static atomic_t mtu_cap = ATOMIC_INIT(CONFIG_APP_VRT_MTU_CAP);

/* The sender's. */
static struct nf_tmpl tmpl;
static bool active;
static uint8_t hdr_pool[NHDR][HDR_SLOT] __aligned(64);
static unsigned int hslot;

/* the header slot of the next frame: wrapped here, not by the modulus of a counter that wraps */
static inline unsigned int hdr_next(void)
{
	unsigned int k = hslot;

	hslot = k + 1U == NHDR ? 0U : k + 1U;
	return k;
}
static uint8_t vcount;
static struct vrt_clock clk;
static bool clk_valid;
static uint32_t rd;
static uint32_t flush_to;
static bool flush_pending;
static bool ctx_due;
static uint32_t ctx_rate, ctx_gain_idx, ctx_run;
static uint8_t ctx_epoch;
static uint64_t ctx_freq;
static bool ctx_have;
static int64_t ctx_ms;
static uint8_t ctx_count;
static struct dwmac_tx_frame pending[BATCH];
static unsigned int npend;
static atomic_t inflight, waiting;
static struct tx_stats stats;

static K_SEM_DEFINE(work, 0, 1);
static K_THREAD_STACK_DEFINE(sender_stack, SENDER_STACK);
static struct k_thread sender_thread;

/* Frames made for one-off packets: context and acknowledges. */
struct ctl_slot {
	atomic_t busy;
	uint8_t hdr[HDR_SLOT] __aligned(8);
	uint8_t body[320] __aligned(8);
};
static struct ctl_slot ctl_slots[4];

unsigned int txpath_mtu(void)
{
	unsigned int m = net_if_get_mtu(iface);

	return MIN(m, (unsigned int)atomic_get(&mtu_cap));
}

bool txpath_zero_csum(void)
{
	k_spinlock_key_t key = k_spin_lock(&sub_lock);
	bool z = sub.nocsum;

	k_spin_unlock(&sub_lock, key);
	return z;
}

bool txpath_v6(void)
{
	k_spinlock_key_t key = k_spin_lock(&sub_lock);
	bool v6 = sub.v6;

	k_spin_unlock(&sub_lock, key);
	return v6;
}

uint32_t txpath_payload_bytes(void)
{
	return (uint32_t)vrt_payload_bytes(txpath_mtu(), txpath_v6());
}

void txpath_set_mtu(unsigned int mtu)
{
	atomic_set(&mtu_cap, (atomic_val_t)CLAMP(mtu, 576U, 1500U));
	if (txpath_active() && txpath_payload_bytes() != cells.pl) {
		radio_replan();
	}
}

const struct device *txpath_dev(void)
{
	return eth_dev;
}

uint32_t txpath_sub_gen(void)
{
	return (uint32_t)atomic_get(&sub_gen);
}

uint32_t txpath_underflows(void)
{
	return dwmac_tx_underflows(eth_dev);
}

void txpath_kick(void)
{
	k_sem_give(&work);
}

void txpath_context_due(void)
{
	ctx_due = true;
}

bool txpath_active(void)
{
	k_spinlock_key_t key = k_spin_lock(&sub_lock);
	bool on = sub_on;

	k_spin_unlock(&sub_lock, key);
	return on;
}

bool txpath_peer(struct peer *p)
{
	k_spinlock_key_t key = k_spin_lock(&sub_lock);
	bool on = sub_on;

	*p = sub;
	k_spin_unlock(&sub_lock, key);
	return on;
}

bool txpath_subscribe(const struct peer *p)
{
	k_spinlock_key_t key = k_spin_lock(&sub_lock);
	bool changed = !sub_on || memcmp(&sub, p, sizeof(sub)) != 0;

	if (changed) {
		sub = *p;
		sub_on = true;
		atomic_inc(&sub_gen);
	}
	k_spin_unlock(&sub_lock, key);
	if (changed) {
		k_sem_give(&work);
	}
	return changed;
}

void txpath_unsubscribe(void)
{
	k_spinlock_key_t key = k_spin_lock(&sub_lock);
	bool was = sub_on;

	sub_on = false;
	if (was) {
		atomic_inc(&sub_gen);
	}
	k_spin_unlock(&sub_lock, key);
	k_sem_give(&work);
}

void txpath_get_stats(struct tx_stats *s)
{
	*s = stats;
	stats.build_cycles = stats.build_n = stats.send_cycles = stats.send_n = 0U;
}

/* One frame of the control path: the MAC copies nothing, so the buffers stay until it is done. */
static ESP_SDR_CODE(PACED_CHUNK, ctl_done) void ctl_done(void *arg)
{
	atomic_clear(&((struct ctl_slot *)arg)->busy);
}

int txpath_send_ctl(const struct peer *to, const void *payload, size_t len)
{
	struct ctl_slot *slot = NULL;
	struct nf_tmpl t;
	struct dwmac_tx_frame f;

	if (len > sizeof(slot->body)) {
		return -EMSGSIZE;
	}
	for (size_t k = 0; k < ARRAY_SIZE(ctl_slots); k++) {
		if (atomic_cas(&ctl_slots[k].busy, 0, 1)) {
			slot = &ctl_slots[k];
			break;
		}
	}
	if (slot == NULL) {
		return -EBUSY;
	}
	nf_tmpl_init(&t, to->v6, to->mac, net_if_get_link_addr(iface)->addr, to->local, to->addr,
		     CONFIG_APP_VRT_PORT, to->port);
	memcpy(slot->hdr, t.b, t.len);
	memcpy(slot->body, payload, len);
	nf_seal(&t, slot->hdr, 0, slot->body, len);
	f = (struct dwmac_tx_frame){.b1 = slot->hdr, .l1 = t.len, .b2 = slot->body,
				    .l2 = (uint16_t)len, .done = ctl_done, .arg = slot};
	if (dwmac_send_frames(eth_dev, &f, 1) != 1) {
		atomic_clear(&slot->busy);
		stats.errors++;
		return -EAGAIN;
	}
	return 0;
}

/* The MAC sent a data frame: its cell is the consumer's again. */
static ESP_SDR_CODE(PACED_CHUNK, data_done) void data_done(void *arg)
{
	vrt_cells_done(&cells, (uint32_t)(uintptr_t)arg);
	atomic_dec(&inflight);
	if (atomic_get(&waiting) != 0) {
		k_sem_give(&work);
	}
}

static void sub_rebuild(void)
{
	struct peer p;
	bool on = txpath_peer(&p);

	active = on;
	vcount = 0;
	clk_valid = false;
	ctx_due = true;
	ctx_have = false;
	/* cells made for the last destination go, and so do the frames not yet in the MAC */
	flush_to = (uint32_t)atomic_get(&cells.ready);
	rd = flush_to;
	flush_pending = true;
	for (unsigned int k = 0; k < npend; k++) {
		atomic_dec(&inflight);
	}
	npend = 0;
	if (on) {
		nf_tmpl_init(&tmpl, p.v6, p.mac, net_if_get_link_addr(iface)->addr, p.local,
			     p.addr, CONFIG_APP_VRT_PORT, p.port);
		tmpl.zero_csum = p.nocsum;
		LOG_INF("stream to %s port %u, packets of %u bytes", p.v6 ? "IPv6" : "IPv4", p.port,
			(unsigned int)vrt_payload_bytes(txpath_mtu(), p.v6));
	}
}

static void flush_check(void)
{
	if (flush_pending && atomic_get(&inflight) == 0) {
		if ((int32_t)(flush_to - (uint32_t)atomic_get(&cells.released)) > 0) {
			atomic_set(&cells.released, (atomic_val_t)flush_to);
		}
		flush_pending = false;
	}
}

/* Make the frame of cell `rd`. */
static ESP_SDR_CODE(PACED_CHUNK, build_frame) void build_frame(struct dwmac_tx_frame *f)
{
	const struct vrt_cell_meta *m = vrt_cell_meta(&cells, rd);
	uint8_t *body = vrt_cell_ptr(&cells, rd);
	uint8_t *h = hdr_pool[hdr_next()];
	size_t bytes = m->bytes;
	bool clip = vrt_clipped(body, bytes, CONFIG_APP_VRT_CLIP_STRIDE);

	if (!clk_valid || clk.index != m->index || clk.rate != m->rate) {
		/* the first packet, or after a loss: the time is the sample index's */
		vrt_clock_seek(&clk, m->rate, m->index);
		clk_valid = true;
	}
	vrt_put_trailer(body + bytes, vrt_trailer_word(radio_agc(), clip, m->loss));
	memcpy(h, tmpl.b, tmpl.len);
	vrt_data_header(h + tmpl.len, CONFIG_APP_VRT_STREAM_ID, vcount++, vrt_packet_words(bytes),
			clk.sec, vrt_clock_ps(&clk));
	vrt_clock_advance(&clk, (uint32_t)(bytes / 2U));
	/* IPv6: the cells summed the payload while they copied it */
	nf_seal2(&tmpl, h, VRT_DATA_HDR_BYTES, body, bytes + VRT_TRAILER_BYTES, m->sum,
		 tmpl.v6 && m->sum_valid ? bytes : 0U);
	*f = (struct dwmac_tx_frame){.b1 = h,
				     .l1 = (uint16_t)(tmpl.len + VRT_DATA_HDR_BYTES),
				     .b2 = body,
				     .l2 = (uint16_t)(bytes + VRT_TRAILER_BYTES),
				     .done = data_done,
				     .arg = (void *)(uintptr_t)rd};
	stats.bytes += tmpl.len + VRT_DATA_HDR_BYTES + bytes + VRT_TRAILER_BYTES;
	rd++;
	atomic_inc(&inflight);
}

/* Benchmark: synthetic packets of the subscriber's kind, through the same builder and MAC path. */
static struct {
	uint32_t pkt_bytes, seconds, rate_pps;
	struct bench_result *out;
	bool run;
} bench_req;
static K_SEM_DEFINE(bench_done, 0, 1);
static uint8_t bench_cell[1472] __aligned(64);
static atomic_t bench_sent, bench_abort, bench_inflight;

static ESP_SDR_CODE(PACED_CHUNK, bench_done_cb) void bench_done_cb(void *arg)
{
	ARG_UNUSED(arg);
	atomic_inc(&bench_sent);
	atomic_dec(&bench_inflight);
	if (atomic_get(&waiting) != 0) {
		k_sem_give(&work);
	}
}

static void bench_run(void)
{
	struct bench_result *r = bench_req.out;
	uint32_t body = bench_req.pkt_bytes - VRT_DATA_HDR_BYTES;
	uint64_t t0 = k_cycle_get_64(), end, busy = 0, due;
	uint32_t hz = sys_clock_hw_cycles_per_sec(), per = 0;
	struct dwmac_tx_frame fr[BATCH];
	unsigned int nf = 0;
	struct vrt_clock bc;
	uint32_t sent = 0, late = 0, u0 = dwmac_tx_underflows(eth_dev);
	uint8_t cnt = 0;
	uint32_t gen0 = (uint32_t)atomic_get(&sub_gen);

	memset(bench_cell, 0x55, sizeof(bench_cell));
	vrt_clock_seek(&bc, 40000000U, 0);
	end = t0 + (uint64_t)bench_req.seconds * hz;
	if (bench_req.rate_pps != 0U) {
		per = hz / bench_req.rate_pps;
	}
	due = t0;
	atomic_set(&bench_sent, 0);
	while (k_cycle_get_64() < end && !atomic_get(&bench_abort) &&
	       (uint32_t)atomic_get(&sub_gen) == gen0) {
		uint64_t a = k_cycle_get_64();
		unsigned int n;

		if (per != 0U) {
			if (a < due) {
				continue;
			}
			if (a - due > per * 4ULL) {
				late++;
			}
		}
		while (nf < BATCH && (per == 0U || a >= due)) {
			uint8_t *h = hdr_pool[hdr_next()];
			uint8_t *pl = bench_cell;

			vrt_put_trailer(pl + body - VRT_TRAILER_BYTES, vrt_trailer_word(false, false, false));
			memcpy(h, tmpl.b, tmpl.len);
			vrt_data_header(h + tmpl.len, CONFIG_APP_VRT_STREAM_ID, cnt++,
					(uint16_t)(bench_req.pkt_bytes / 4U), bc.sec,
					vrt_clock_ps(&bc));
			vrt_clock_advance(&bc, (body - VRT_TRAILER_BYTES) / 2U);
			nf_seal(&tmpl, h, VRT_DATA_HDR_BYTES, pl, body);
			atomic_inc(&bench_inflight);
			fr[nf++] = (struct dwmac_tx_frame){.b1 = h,
				.l1 = (uint16_t)(tmpl.len + VRT_DATA_HDR_BYTES), .b2 = pl,
				.l2 = (uint16_t)body, .done = bench_done_cb};
			due += per;
			if (per != 0U) {
				break;
			}
		}
		n = (unsigned int)dwmac_send_frames(eth_dev, fr, nf);
		sent += n;
		busy += k_cycle_get_64() - a;
		if (n < nf) {
			memmove(fr, fr + n, (nf - n) * sizeof(fr[0]));
			nf -= n;
			stats.desc_waits++;
			r->desc_waits++;
			atomic_set(&waiting, 1);
			(void)k_sem_take(&work, K_USEC(200));
			atomic_set(&waiting, 0);
		} else {
			nf = 0;
		}
	}
	/* frames built and not taken by the MAC are not in flight */
	for (unsigned int k = 0; k < nf; k++) {
		atomic_dec(&bench_inflight);
	}
	/* let the MAC finish what it holds */
	for (int k = 0; k < 1000 && atomic_get(&bench_inflight) != 0; k++) {
		k_msleep(1);
	}
	if (atomic_get(&bench_inflight) != 0) {
		LOG_WRN("bench: %ld frames never completed", (long)atomic_get(&bench_inflight));
		atomic_set(&bench_inflight, 0);
	}
	r->packets = sent;
	r->cycles = busy;
	r->late = late;
	r->underflows = dwmac_tx_underflows(eth_dev) - u0;
	r->cycles_per_sec = hz;
}

/* A context is due before this cell: after a subscription, for every new run (also when nothing
 * differs from the last), when the settings differ, once a second.
 */
static bool context_due(const struct vrt_cell_meta *m)
{
	return ctx_due || !ctx_have || m->run != ctx_run || m->rate != ctx_rate ||
	       m->freq_hz != ctx_freq || (uint32_t)(int)m->gain != ctx_gain_idx ||
	       m->epoch != ctx_epoch || k_uptime_get() - ctx_ms >= CTX_PERIOD_MS;
}

static bool send_context(const struct vrt_cell_meta *m)
{
	uint32_t buf[80];
	struct vrt_clock c;
	struct peer p;
	int words;
	bool changed = ctx_have && (m->rate != ctx_rate || m->freq_hz != ctx_freq ||
				    (uint32_t)(int)m->gain != ctx_gain_idx);

	vrt_clock_seek(&c, m->rate != 0U ? m->rate : 1U, m->index);
	words = ctl_build_context(buf, ARRAY_SIZE(buf), m->rate, m->freq_hz, m->gain, m->epoch, c.sec,
				  vrt_clock_ps(&c), changed, ctx_count);
	if (words < 0 || !txpath_peer(&p) || txpath_send_ctl(&p, buf, (size_t)words * 4U) != 0) {
		/* not sent: it stays due */
		return false;
	}
	ctx_count++;
	stats.context++;
	ctx_rate = m->rate;
	ctx_freq = m->freq_hz;
	ctx_gain_idx = (uint32_t)(int)m->gain;
	ctx_epoch = m->epoch;
	ctx_run = m->run;
	ctx_have = true;
	ctx_due = false;
	ctx_ms = k_uptime_get();
	return true;
}

static ESP_SDR_CODE(PACED_CHUNK, send_data) void send_data(void)
{
	unsigned int n;

	uint32_t t0 = k_cycle_get_32(), t1;

	while (npend < BATCH && vrt_cells_avail(&cells, rd) != 0U) {
		if (context_due(vrt_cell_meta(&cells, rd))) {
			/* in front of this cell: what is built goes first, so the context comes after the
			 * frames of the earlier run and before the first one of this
			 */
			if (npend != 0U) {
				break;
			}
			if (!send_context(vrt_cell_meta(&cells, rd))) {
				break;
			}
		}
		if (vrt_cell_meta(&cells, rd)->bytes > vrt_payload_bytes(txpath_mtu(), tmpl.v6)) {
			/* made for another destination or MTU: it does not fit the packets of this one */
			flush_to = (uint32_t)atomic_get(&cells.ready);
			rd = flush_to;
			flush_pending = true;
			clk_valid = false;
			stats.errors++;
			break;
		}
		build_frame(&pending[npend++]);
		stats.build_n++;
	}
	if (npend == 0U) {
		return;
	}
	t1 = k_cycle_get_32();
	stats.build_cycles += t1 - t0;
	n = (unsigned int)dwmac_send_frames(eth_dev, pending, npend);
	stats.send_cycles += k_cycle_get_32() - t1;
	stats.send_n += n;
	stats.packets += n;
	if (n < npend) {
		/* no descriptor: the frames wait here, the loop looks at the subscriber and the
		 * context before it tries again, a completion wakes it
		 */
		memmove(pending, pending + n, (npend - n) * sizeof(pending[0]));
		npend -= n;
		stats.desc_waits++;
	} else {
		npend = 0;
	}
}

static void sender(void *a, void *b, void *c)
{
	uint32_t gen_seen = UINT32_MAX;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		atomic_set(&waiting, npend != 0U);
		if (k_sem_take(&work, npend != 0U ? K_MSEC(1) : K_MSEC(100)) != 0 &&
		    dwmac_tx_free(eth_dev) < CONFIG_DWMAC_NB_TX_DESCS - 1) {
			/* frames are queued and nothing woke us: the MAC may have stopped, start it */
			dwmac_tx_poll(eth_dev);
		}
		atomic_set(&waiting, 0);
		if (bench_req.run) {
			bench_run();
			bench_req.run = false;
			k_sem_give(&bench_done);
			continue;
		}
		if ((uint32_t)atomic_get(&sub_gen) != gen_seen) {
			gen_seen = (uint32_t)atomic_get(&sub_gen);
			sub_rebuild();
		}
		flush_check();
		if (!active) {
			rd = (uint32_t)atomic_get(&cells.ready);
			flush_to = rd;
			flush_pending = true;
			continue;
		}
		send_data();
	}
}

int txpath_init(struct net_if *i)
{
	iface = i;
	eth_dev = net_if_get_device(i);
	stats.underflow_base = dwmac_tx_underflows(eth_dev);
	k_thread_create(&sender_thread, sender_stack, K_THREAD_STACK_SIZEOF(sender_stack), sender,
			NULL, NULL, NULL, K_PRIO_PREEMPT(1), 0, K_FOREVER);
	k_thread_name_set(&sender_thread, "vrt_send");
#if defined(CONFIG_SMP) && defined(CONFIG_SCHED_CPU_MASK)
	if (CONFIG_APP_VRT_SENDER_CPU >= 0 && arch_num_cpus() > 1) {
		int ret = k_thread_cpu_pin(&sender_thread, CONFIG_APP_VRT_SENDER_CPU);

		if (ret != 0) {
			LOG_ERR("pinning the sender to CPU %d: %d", CONFIG_APP_VRT_SENDER_CPU, ret);
		}
	}
#endif
	k_thread_start(&sender_thread);
	return 0;
}


int txpath_bench(uint32_t pkt_bytes, uint32_t seconds, uint32_t rate_pps, struct bench_result *out)
{
	uint32_t max = (uint32_t)vrt_payload_bytes(txpath_mtu(), txpath_v6()) + VRT_DATA_HDR_BYTES +
		       VRT_TRAILER_BYTES;
	uint32_t gen0 = (uint32_t)atomic_get(&sub_gen);

	if (!txpath_active()) {
		return -ENOTCONN;
	}
	pkt_bytes = CLAMP(pkt_bytes & ~3U, 64U, max);
	memset(out, 0, sizeof(*out));
	out->pkt_bytes = pkt_bytes;
	out->seconds = seconds;
	/* the radio stops for the run: the cells are not the bench's. It has stopped when the ring is
	 * not running and the sender has seen the stop.
	 */
	radio_enable(false);
	for (int k = 0; k < 200 && esp_sdr_diag16_active(); k++) {
		k_msleep(10);
	}
	if (esp_sdr_diag16_active()) {
		ctl_resume(gen0);
		return -EBUSY;
	}
	k_msleep(50);
	atomic_set(&bench_abort, 0);
	bench_req.pkt_bytes = pkt_bytes;
	bench_req.seconds = seconds;
	bench_req.rate_pps = rate_pps;
	bench_req.out = out;
	bench_req.run = true;
	k_sem_give(&work);
	if (k_sem_take(&bench_done, K_SECONDS(seconds + 5U)) != 0) {
		/* the sender is stuck or slow to leave: tell it, wait for it */
		atomic_set(&bench_abort, 1);
		(void)k_sem_take(&bench_done, K_SECONDS(5));
	}
	/* the stream goes on for the subscriber that was there, if it still is, and not stopped: the
	 * decision and the enable under the control lock
	 */
	ctl_resume((uint32_t)gen0);
	return 0;
}

void txpath_bench_abort(void)
{
	atomic_set(&bench_abort, 1);
}
