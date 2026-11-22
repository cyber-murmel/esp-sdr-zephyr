/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The injector (tx_inject.h). The tone has a period of 351 samples and a packet is 351 or 702 of
 * them, so every packet that starts at a multiple of 351 carries the same payload: a ring of
 * TX_BUFFERS buffers of the receive pool is made once with the payload in them, and a packet is the
 * index and count patched into the 16 byte header and a reference taken. The stream's thread gives
 * the reference back when it is done (a buffer is free again at a reference count of 1), so nothing
 * is copied or allocated while it runs. It sends as far as the credit allows: the pace is the board's.
 * It spins (a thread of the lowest priority on CPU 0, below the shell, which prj.conf puts one above,
 * and the network). After a
 * stall or an underrun it jumps to the index the board asks to restart at (rounded up to 351).
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/barrier.h>

#include <esp_sdr/esp_sdr_place.h>

#include "tx_inject.h"
#include "tx_stream.h"
#include "tx_tone.h"
#include "txs_limits.h"

/* Ethernet 14, IPv4 20, UDP 8: the VITA packet then sits at 2 mod 4 in a frame that starts aligned */
#define HDR_BYTES 42U
#define MAX_BATCH 8U
#define SPIN_US   5U
#define STALL_SLICE_MS 10U /* a stall sleeps in slices, to see the stop request */

static struct tx_inj_cfg cfg;
static struct tx_inj_stats stats;
static atomic_t run;
static K_THREAD_STACK_DEFINE(inj_stack, 2048);
/* In group 3 with the stack (place.h, uninitialized as all of group 3): group 0 has no room for its 464 B
 * beside the network side. tx_inject_start() zeroes it before each k_thread_create(), as .bss would.
 */
static struct k_thread inj_thread ESP_SDR_DATA_F(STATE, inj_thread, NOINIT);
static atomic_t alive;
static struct net_buf *ring[TX_BUFFERS];
static struct txs_wire_cfg wc;
static uint32_t pkt_words;

void tx_inject_set(const struct tx_inj_cfg *c)
{
	cfg = *c;
	cfg.reorder = MIN(cfg.reorder, MAX_BATCH);
	cfg.stall_ms = MIN(cfg.stall_ms, TXS_STALL_MAX);
}

void tx_inject_get(struct tx_inj_cfg *c)
{
	*c = cfg;
}

void tx_inject_stats(struct tx_inj_stats *s)
{
	/* the injector is on this core: its 64 bit counters tear only if it preempts the copy */
	const unsigned int key = irq_lock();

	*s = stats;
	irq_unlock(key);
}

/* The ring: buffers with the payload of a packet at a multiple of 351, header patched per send. */
static int ring_make(enum txs_fmt fmt)
{
	const uint32_t bytes = TXS_WIRE_HDR_BYTES + pkt_words * (fmt == TXS_FMT_IQ8 ? 2U : 4U);

	for (uint32_t i = 0; i < TX_BUFFERS; i++) {
		ring[i] = net_pkt_get_reserve_rx_data(HDR_BYTES + bytes, K_NO_WAIT);
		if (ring[i] == NULL) {
			for (uint32_t k = 0; k < i; k++) {
				net_buf_unref(ring[k]);
				ring[k] = NULL;
			}
			return -ENOMEM;
		}
		net_buf_add(ring[i], HDR_BYTES + bytes);
		txs_wire_header(ring[i]->data + HDR_BYTES, &wc, 0, 0, pkt_words);
		tx_tone_pack(ring[i]->data + HDR_BYTES + TXS_WIRE_HDR_BYTES, 0, pkt_words, fmt);
	}
	return 0;
}

static void ring_free(void)
{
	for (uint32_t i = 0; i < TX_BUFFERS; i++) {
		if (ring[i] != NULL) {
			net_buf_unref(ring[i]);
			ring[i] = NULL;
		}
	}
}

static uint32_t ring_nfree(void)
{
	uint32_t n = 0U;

	for (uint32_t i = 0; i < TX_BUFFERS; i++) {
		n += __atomic_load_n(&ring[i]->ref, __ATOMIC_ACQUIRE) == 1U;
	}
	return n;
}

/* A free ring buffer (the stream is done with it), patched for the packet at idx; one reference more. */
static ESP_SDR_CODE(EVENT, inj_take) struct net_buf *inj_take(uint32_t *rr, uint64_t idx, uint8_t count)
{
	for (uint32_t i = 0; i < TX_BUFFERS; i++) {
		struct net_buf *b = ring[(*rr + i) % TX_BUFFERS];

		if (__atomic_load_n(&b->ref, __ATOMIC_ACQUIRE) == 1U) {
			*rr = (*rr + i + 1U) % TX_BUFFERS;
			txs_wire_header(b->data + HDR_BYTES, &wc, count, idx, pkt_words);
			b = net_buf_ref(b);
			return b;
		}
	}
	return NULL;
}

static uint32_t xorshift(uint32_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 17;
	*s ^= *s << 5;
	return *s;
}

static uint64_t round_up(uint64_t x, uint32_t m)
{
	return (x + m - 1U) / m * m;
}

static void inj_entry(void *a, void *b, void *c)
{
	const uint32_t n = pkt_words;
	struct txs_status s;
	struct net_buf *bufs[MAX_BATCH];
	uint32_t rng = 0x1234567U, last_under = 0U, rr = 0U;
	uint64_t next = 0U;
	int64_t t_stall, t_start = k_uptime_get();
	uint8_t count = 0U;
	bool have = false;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	t_stall = t_start + 1000;
	while (atomic_get(&run) != 0) {
		const uint32_t batch = MAX(cfg.reorder, 1U);
		uint64_t first;

		if (!tx_stream_status(&s) || s.restart == 0U) {
			k_busy_wait(SPIN_US);
			continue;
		}
		stats.ms = (uint32_t)(k_uptime_get() - t_start);
		if (!have) {
			next = round_up(s.restart, TX_TONE_PERIOD);
			last_under = s.underruns;
			have = true;
		}
		if (cfg.stall_ms != 0U && k_uptime_get() >= t_stall) {
			uint32_t left = MIN(cfg.stall_ms, TXS_STALL_MAX);

			stats.stalls++;
			while (left != 0U && atomic_get(&run) != 0) {
				const uint32_t slice = MIN(left, STALL_SLICE_MS);

				k_msleep(slice);
				left -= slice;
			}
			t_stall = k_uptime_get() + 1000;
			continue;
		}
		/* behind the play position, or the board closed a window short: restart where it says */
		if (s.underruns != last_under || next + n <= s.play) {
			last_under = s.underruns;
			if (next < s.restart) {
				next = round_up(s.restart, TX_TONE_PERIOD);
				stats.align_words += next - s.restart;
				stats.resyncs++;
			}
		}
		if (next + (uint64_t)n * batch > s.credit) {
			stats.credit_waits++;
			k_busy_wait(SPIN_US);
			continue;
		}
		/* a whole batch needs its buffers: a packet never goes unsent for want of one */
		if (ring_nfree() < batch) {
			stats.no_buf++;
			k_busy_wait(SPIN_US);
			continue;
		}
		first = next;
		for (uint32_t i = 0; i < batch; i++) {
			const bool drop = cfg.loss_pm != 0U && xorshift(&rng) % 1000U < cfg.loss_pm;

			bufs[i] = NULL;
			if (drop) {
				stats.dropped++;
				continue;
			}
			bufs[i] = inj_take(&rr, first + (uint64_t)i * n, count++);
			if (bufs[i] != NULL) {
				tx_stream_inflight_add(1);
			}
		}
		next += (uint64_t)n * batch;
		stats.next = next;
		/* a batch goes out last packet first */
		for (uint32_t i = 0; i < batch; i++) {
			struct net_buf *x = bufs[batch - 1U - i];

			if (x == NULL) {
				continue;
			}
			if (tx_stream_put(x, HDR_BYTES, (uint16_t)(x->len - HDR_BYTES))) {
				stats.sent++;
				stats.words += n;
				stats.reordered += batch > 1U && i != batch - 1U - i;
			} else {
				stats.ring_full++;
				net_buf_unref(x);
				tx_stream_inflight_add(-1);
			}
		}
		if (batch == 1U && bufs[0] == NULL && !(cfg.loss_pm != 0U)) {
			k_busy_wait(SPIN_US);
		}
	}
	atomic_set(&alive, 0);
}

int tx_inject_start(void)
{
	const struct txs_wire_cfg *w = tx_stream_wire();
	k_tid_t t;
	int ret;

	if (atomic_get(&alive) != 0) {
		return -EBUSY;
	}
	memset(&stats, 0, sizeof(stats));
	wc = *w;
	pkt_words = w->max_words;
	ret = ring_make(w->fmt);
	if (ret != 0) {
		return ret;
	}
	atomic_set(&run, 1);
	atomic_set(&alive, 1);
	/* no injector thread lives here (alive was 0): the object is free to clear */
	memset(&inj_thread, 0, sizeof(inj_thread));
	t = k_thread_create(&inj_thread, inj_stack, K_THREAD_STACK_SIZEOF(inj_stack), inj_entry, NULL, NULL,
			    NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, K_FOREVER);
	k_thread_name_set(t, "tx_inject");
	ret = k_thread_cpu_pin(t, 0);
	if (ret != 0) {
		k_thread_abort(t);
		atomic_set(&alive, 0);
		ring_free();
		return ret;
	}
	k_thread_start(t);
	return 0;
}

bool tx_inject_stop(void)
{
	bool clean = true;

	atomic_set(&run, 0);
	if (atomic_get(&alive) != 0 && k_thread_join(&inj_thread, K_MSEC(200)) != 0) {
		/* it did not end (a stall is sliced, a send is short): end it, its buffers are not touched
		 * after this */
		k_thread_abort(&inj_thread);
		atomic_set(&alive, 0);
		stats.aborted++;
		clean = false;
	}
	/* what the stream still holds is unreferenced by it; ours go back to the pool at the last reference */
	ring_free();
	return clean;
}

/* How fast the packets can be made alone: patch, reference, give back, no stream and no credit. */
int tx_inject_bench(enum txs_fmt fmt, uint32_t ms, uint64_t *words_per_s, uint32_t *cyc_per_pkt)
{
	uint64_t n = 0U;
	uint32_t rr = 0U, c0, c1;
	int64_t t0, t1;

	if (tx_stream_active() || atomic_get(&alive) != 0) {
		return -EBUSY;
	}
	wc = (struct txs_wire_cfg){.stream_id = TX_STREAM_ID, .port = (uint16_t)TX_PORT, .fmt = fmt,
				   .max_words = fmt == TXS_FMT_IQ8 ? 702U : 351U};
	pkt_words = wc.max_words;
	if (ring_make(fmt) != 0) {
		return -ENOMEM;
	}
	t0 = k_uptime_get();
	__asm__ volatile("csrr %0, cycle" : "=r"(c0));
	do {
		for (uint32_t i = 0; i < 256U; i++) {
			struct net_buf *b = inj_take(&rr, (uint64_t)n * pkt_words, (uint8_t)n);

			if (b != NULL) {
				net_buf_unref(b);
				n++;
			}
		}
		t1 = k_uptime_get();
	} while (t1 - t0 < (int64_t)ms);
	__asm__ volatile("csrr %0, cycle" : "=r"(c1));
	ring_free();
	*words_per_s = n * pkt_words * 1000U / (uint64_t)MAX(t1 - t0, 1);
	*cyc_per_pkt = n != 0U ? (c1 - c0) / (uint32_t)MIN(n, 0xffffffffULL) : 0U;
	return 0;
}
