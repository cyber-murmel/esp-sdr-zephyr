/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The receive engine: a cooperative thread pinned to CPU 1 runs esp_sdr_ring_run(), whose
 * sink copies into the capture buffer and ends the run when the capture is complete. The
 * state shared with the REPL's CPU lives in internal RAM: atomics in PSRAM are emulated and
 * not safe across the CPUs (doc/hardware-quirks.md, Memory). The buffer itself may be PSRAM.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/barrier.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_ring.h>
#include <esp_sdr/esp_sdr_rx.h>

#include <esp_attr.h>

#include "esdr_rx.h"

#define RX_CPU 1

/* The job the REPL hands over; written only while the engine is idle. */
static struct {
	int16_t *buf;
	size_t cap;
	uint64_t want;
	bool monitor;
	/* buf is a ring: no end at cap. */
	bool wrap;
	struct esp_sdr_ring_cfg cfg;
	/* A native rate (enum esp_sdr_rate) for bursts instead of the ring, else -1. */
	int burst;
} job;

/* A burst converted for the sink, a chunk at a time (CPU 1 is not time critical in bursts). */
#define BURST_CHUNK 256U
static EXT_RAM_BSS_ATTR int16_t burst_i[BURST_CHUNK], burst_q[BURST_CHUNK];

/* Sink state, touched only by the ring loop while it runs; stored also read by the REPL's CPU. */
static struct {
	uint64_t stored;
	uint64_t expect;
	size_t wpos;
} snk;

/*
 * Snapshots for displays (I, Q packed per word), taken on demand: writing every sample to PSRAM
 * from this CPU stalls behind the display's PSRAM traffic on the other one and the ring loses
 * input (250 kS/s with LVGL running). esdr_rx_latest() hands out the last finished snapshot and
 * arms the next one in the other buffer, so the sink writes only what displays read.
 * snap_state 1: the sink fills mon[snap_fill] up to snap_want samples, then publishes it as
 * snap_pub (samples << 1 | buffer, -1 none) and returns to 0. Only the REPL's CPU arms, while
 * idle.
 */
static EXT_RAM_BSS_ATTR uint32_t mon[2][ESDR_MON_SAMPLES];
static atomic_t mon_w;
static atomic_t snap_state, snap_pub;
static uint32_t snap_fill, snap_want, snap_got, snap_want_max;

static atomic_t busy;
static atomic_t stop_req;
static struct esdr_rx_stats stats;
/* The running ring's own counters (the engine thread's), read live by the REPL's CPU. */
static const struct esp_sdr_ring_stats *volatile ring_live;
static K_SEM_DEFINE(start_sem, 0, 1);
static K_SEM_DEFINE(done_sem, 0, 1);

static void sink(void *user, const int16_t *i, const int16_t *q, size_t n, uint64_t index)
{
	ARG_UNUSED(user);

	/* A stop asked for just before this run started was discarded by the ring. */
	if (atomic_get(&stop_req)) {
		esp_sdr_ring_stop();
		return;
	}
	if (index != snk.expect) {
		stats.gaps++;
		stats.gap_samples += index - snk.expect;
	}
	snk.expect = index + n;

	if (job.monitor) {
		atomic_add(&mon_w, (atomic_val_t)n);
		if (atomic_get(&snap_state) == 1) {
			uint32_t *m = &mon[snap_fill][snap_got];
			size_t take = MIN(n, (size_t)(snap_want - snap_got));

			for (size_t k = 0; k < take; k++) {
				m[k] = (uint32_t)(uint16_t)i[k] | ((uint32_t)(uint16_t)q[k] << 16);
			}
			snap_got += (uint32_t)take;
			if (snap_got >= snap_want) {
				atomic_set(&snap_pub, (atomic_val_t)((snap_got << 1) | snap_fill));
				atomic_set(&snap_state, 0);
			}
		}
	}

	uint64_t left = job.want - snk.stored;
	size_t m = n;

	if (left < m) {
		m = (size_t)left;
	}
	if (job.buf != NULL && job.wrap) {
		for (size_t k = 0; k < m; k++) {
			job.buf[2 * snk.wpos] = i[k];
			job.buf[2 * snk.wpos + 1] = q[k];
			if (++snk.wpos == job.cap) {
				snk.wpos = 0;
			}
		}
	} else if (job.buf != NULL) {
		size_t room = job.cap - (size_t)snk.stored;

		if (room < m) {
			m = room;
		}
		int16_t *out = &job.buf[2 * snk.stored];

		for (size_t k = 0; k < m; k++) {
			out[2 * k] = i[k];
			out[2 * k + 1] = q[k];
		}
	}
	/* The ring is filled before the count says so: the reader on the other CPU. */
	barrier_dmem_fence_full();
	snk.stored += m;
	if (snk.stored >= job.want ||
	    (job.buf != NULL && !job.wrap && snk.stored >= job.cap)) {
		atomic_set(&stop_req, 1);
		esp_sdr_ring_stop();
	}
}

/*
 * Native rates: single-bank bursts, contiguous within a burst only. Captures into a buffer take
 * full bursts back to back (each new one after the first is a gap); display-only runs capture
 * only when a display has armed a snapshot.
 */
static void burst_loop(void)
{
	while (!atomic_get(&stop_req)) {
		struct esp_sdr_rx_burst b;
		size_t n = ESP_SDR_SAMPLES_MAX;

		if (job.buf == NULL && job.want == UINT64_MAX) {
			if (atomic_get(&snap_state) != 1) {
				k_sleep(K_MSEC(2));
				continue;
			}
			n = CLAMP(snap_want, ESP_SDR_SAMPLES_MIN, ESP_SDR_SAMPLES_MAX);
		}
		int ret = esp_sdr_rx_capture((enum esp_sdr_rate)job.burst, n, &b);

		if (ret != 0) {
			stats.errors++;
			stats.last_status = ret;
			k_sleep(K_MSEC(2));
			continue;
		}
		if (stats.runs > 0U && job.buf != NULL) {
			stats.gaps++;
		}
		stats.runs++;
		for (size_t k = 0; k < b.count && !atomic_get(&stop_req); k += BURST_CHUNK) {
			size_t m = MIN(BURST_CHUNK, b.count - k);

			for (size_t j = 0; j < m; j++) {
				burst_i[j] = (int16_t)(esp_sdr_rx_i(b.words[k + j]) * 32);
				burst_q[j] = (int16_t)(esp_sdr_rx_q(b.words[k + j]) * 32);
			}
			sink(NULL, burst_i, burst_q, m, snk.expect);
		}
	}
}

static void rx_thread_fn(void *a, void *b, void *c)
{
	struct esp_sdr_ring_stats rst = {};

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	/* For esdr_rx_get_stats(): this frame lives as long as the thread, forever. */
	ring_live = &rst;
	for (;;) {
		(void)k_sem_take(&start_sem, K_FOREVER);
		snk.stored = 0;
		snk.expect = 0;
		snk.wpos = 0;
		if (job.burst >= 0) {
			burst_loop();
		}
		while (job.burst < 0 && !atomic_get(&stop_req)) {
			/* Each run counts its output from 0. */
			snk.expect = 0;
			stats.runs++;
			int ret = esp_sdr_ring_run(&job.cfg, &rst);

			if (ret != 0) {
				stats.errors++;
				stats.last_status = rst.status;
				stats.last_detail = rst.detail;
				if (ret != -EIO) {
					break;
				}
				/* A late bank switch or the like: the capture goes on after a hole. */
				k_sleep(K_MSEC(1));
			}
		}
		stats.samples = snk.stored;
		atomic_set(&busy, 0);
		k_sem_give(&done_sem);
	}
}

/* Above the capture bank: the DRAM image must end below the ring banks. */
ESP_SDR_HIGH_STACK_DEFINE(esdr_rx_stack, 2560);
static struct k_thread rx_thread;

int esdr_rx_init(void)
{
	static bool started;
	int ret;

	/* The Wi-Fi driver brings the PHY up shortly after boot. */
	for (int tries = 0; (ret = esp_sdr_init()) == -EAGAIN && tries < 100; tries++) {
		k_sleep(K_MSEC(50));
	}
	if (ret != 0) {
		return ret;
	}
	if (!started) {
		k_thread_create(&rx_thread, esdr_rx_stack, K_KERNEL_STACK_SIZEOF(esdr_rx_stack),
				rx_thread_fn, NULL, NULL, NULL, K_PRIO_COOP(2), K_FP_REGS, K_FOREVER);
		k_thread_name_set(&rx_thread, "esdr_rx");
#if defined(CONFIG_SCHED_CPU_MASK)
		if (arch_num_cpus() > 1) {
			k_thread_cpu_pin(&rx_thread, RX_CPU);
		}
#endif
		k_thread_start(&rx_thread);
		started = true;
	}
	return 0;
}

static int start_job(unsigned int decim, bool shift_fs4, int burst, int16_t *buf, size_t cap,
		     uint64_t count, bool monitor, bool wrap)
{
	if (!atomic_cas(&busy, 0, 1)) {
		return -EBUSY;
	}
	job.buf = buf;
	job.cap = cap;
	job.wrap = wrap && buf != NULL;
	job.want = count != 0U ? count : UINT64_MAX;
	job.monitor = monitor;
	atomic_set(&mon_w, 0);
	atomic_set(&snap_state, 0);
	atomic_set(&snap_pub, -1);
	snap_want_max = 0;
	job.cfg = (struct esp_sdr_ring_cfg){
		.decim = decim, .shift_fs4 = shift_fs4, .sink = sink,
	};
	job.burst = burst;
	memset(&stats, 0, sizeof(stats));
	atomic_set(&stop_req, 0);
	k_sem_reset(&done_sem);
	k_sem_give(&start_sem);
	return 0;
}

int esdr_rx_start(unsigned int decim, bool shift_fs4, int16_t *buf, size_t cap, uint64_t count,
		  bool monitor, bool wrap)
{
	return start_job(decim, shift_fs4, -1, buf, cap, count, monitor, wrap);
}

uint64_t esdr_rx_count(void)
{
	/* Two 32-bit halves: read until they agree with a second read. */
	uint64_t a, b;

	do {
		a = snk.stored;
		barrier_dmem_fence_full();
		b = snk.stored;
	} while (a != b);
	return a;
}

int esdr_rx_start_burst(int rate, int16_t *buf, size_t cap, uint64_t count, bool monitor)
{
	if (esp_sdr_rx_rate_hz((enum esp_sdr_rate)rate) == 0U) {
		return -EINVAL;
	}
	return start_job(0, false, rate, buf, cap, count, monitor, false);
}

void esdr_rx_stop(void)
{
	if (atomic_get(&busy)) {
		atomic_set(&stop_req, 1);
		esp_sdr_ring_stop();
	}
}

bool esdr_rx_busy(void)
{
	return atomic_get(&busy) != 0;
}

bool esdr_rx_wait(int32_t timeout_ms)
{
	if (!atomic_get(&busy)) {
		return true;
	}
	if (k_sem_take(&done_sem, timeout_ms < 0 ? K_FOREVER : K_MSEC(timeout_ms)) == 0) {
		/* For the next waiter too. */
		k_sem_give(&done_sem);
		return true;
	}
	return !atomic_get(&busy);
}

void esdr_rx_get_stats(struct esdr_rx_stats *out)
{
	const struct esp_sdr_ring_stats *r = ring_live;

	*out = stats;
	if (atomic_get(&busy)) {
		out->samples = snk.stored;
	}
	if (r != NULL && job.burst < 0) {
		/* Single words, each consistent; the set is a snapshot only while idle. */
		out->units = r->units;
		out->abandoned = r->abandoned;
		out->lost_pairs = r->lost_pairs;
		out->late_max = r->late_max;
		out->slice_max = r->slice_max;
		out->stalls = r->stalls;
		out->cycles_x100 = r->cycles_x100;
	}
}

uint64_t esdr_rx_latest(int16_t *dst, size_t n)
{
	uint32_t *out = (uint32_t *)dst;
	atomic_val_t pub = atomic_get(&snap_pub);
	int ready = pub < 0 ? -1 : (int)(pub & 1);
	size_t have = pub < 0 ? 0U : (size_t)(pub >> 1);
	size_t lead = 0;

	if (n > ESDR_MON_SAMPLES) {
		n = ESDR_MON_SAMPLES;
	}
	/* The last finished snapshot, its newest samples; silence first if it is shorter. */
	if (have < n) {
		lead = n - have;
		memset(out, 0, lead * sizeof(*out));
	}
	if (ready >= 0) {
		memcpy(&out[lead], &mon[ready][have - (n - lead)], (n - lead) * sizeof(*out));
	}
	/* Arm the next one in the other buffer, as long as the largest display asked for. */
	snap_want_max = MAX(snap_want_max, (uint32_t)n);
	if (atomic_get(&busy) && job.monitor && atomic_get(&snap_state) == 0) {
		snap_fill = ready < 0 ? 0U : (uint32_t)(1 - ready);
		snap_want = snap_want_max;
		snap_got = 0;
		atomic_set(&snap_state, 1);
	}
	return (uint32_t)atomic_get(&mon_w);
}
