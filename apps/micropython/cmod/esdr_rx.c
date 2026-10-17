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

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_ring.h>

#include <esp_attr.h>

#include "esdr_rx.h"

#define RX_CPU 1

/* The job the REPL hands over; written only while the engine is idle. */
static struct {
	int16_t *buf;
	size_t cap;
	uint64_t want;
	bool monitor;
	struct esp_sdr_ring_cfg cfg;
} job;

/* Sink state, touched only by the ring loop while it runs. */
static struct {
	uint64_t stored;
	uint64_t expect;
} snk;

/* The latest samples for displays (I, Q packed per word), and how many were produced. */
static EXT_RAM_BSS_ATTR uint32_t mon[ESDR_MON_SAMPLES];
static atomic_t mon_w;

static atomic_t busy;
static atomic_t stop_req;
static struct esdr_rx_stats stats;
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
		uint32_t w = (uint32_t)atomic_get(&mon_w);

		for (size_t k = 0; k < n; k++) {
			mon[(w + k) & (ESDR_MON_SAMPLES - 1U)] =
				(uint32_t)(uint16_t)i[k] | ((uint32_t)(uint16_t)q[k] << 16);
		}
		atomic_set(&mon_w, (atomic_val_t)(w + n));
	}

	uint64_t left = job.want - snk.stored;
	size_t m = n;

	if (left < m) {
		m = (size_t)left;
	}
	if (job.buf != NULL) {
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
	snk.stored += m;
	if (snk.stored >= job.want || (job.buf != NULL && snk.stored >= job.cap)) {
		atomic_set(&stop_req, 1);
		esp_sdr_ring_stop();
	}
}

static void rx_thread_fn(void *a, void *b, void *c)
{
	struct esp_sdr_ring_stats rst;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		(void)k_sem_take(&start_sem, K_FOREVER);
		snk.stored = 0;
		while (!atomic_get(&stop_req)) {
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

int esdr_rx_start(unsigned int decim, bool shift_fs4, int16_t *buf, size_t cap, uint64_t count,
		  bool monitor)
{
	if (!atomic_cas(&busy, 0, 1)) {
		return -EBUSY;
	}
	job.buf = buf;
	job.cap = cap;
	job.want = count != 0U ? count : UINT64_MAX;
	job.monitor = monitor;
	atomic_set(&mon_w, 0);
	job.cfg = (struct esp_sdr_ring_cfg){
		.decim = decim, .shift_fs4 = shift_fs4, .sink = sink,
	};
	memset(&stats, 0, sizeof(stats));
	atomic_set(&stop_req, 0);
	k_sem_reset(&done_sem);
	k_sem_give(&start_sem);
	return 0;
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
	*out = stats;
	if (atomic_get(&busy)) {
		out->samples = snk.stored;
	}
}

uint64_t esdr_rx_latest(int16_t *dst, size_t n)
{
	uint32_t w = (uint32_t)atomic_get(&mon_w);
	uint32_t *out = (uint32_t *)dst;
	size_t lead = 0;

	if (n > ESDR_MON_SAMPLES) {
		n = ESDR_MON_SAMPLES;
	}
	/* Fewer samples than asked for so far: silence first. */
	if (w < n) {
		lead = n - w;
		memset(out, 0, lead * sizeof(*out));
	}
	for (size_t k = lead; k < n; k++) {
		out[k] = mon[(w - (uint32_t)n + (uint32_t)k) & (ESDR_MON_SAMPLES - 1U)];
	}
	return w;
}
