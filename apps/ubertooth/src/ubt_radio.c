/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See ubt_radio.h. The ring thread, sink and sample ring follow
 * lib/ieee802154/ieee154_esp_sdr.c.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_ring.h>
#include <esp_sdr/esp_sdr_rx.h>

#include "ubt_attr.h"
#include "ubt_radio.h"

LOG_MODULE_REGISTER(ubt_radio, LOG_LEVEL_INF);

/* Samples in the ring between the ring sink and the DSP thread, power of two (1 ms = 4000). */
#define SRING_SAMPLES 4096U
#define RING_STACK    1536

/*
 * The internal RAM above the capture bank: the sample ring (the sink runs with interrupts masked at
 * 4 MS/s: PSRAM stores would cost it 20 cycles per sample) and the ring thread's stack.
 */
struct ubt_high {
	int16_t i[SRING_SAMPLES] __aligned(16);
	int16_t q[SRING_SAMPLES];
	K_KERNEL_STACK_MEMBER(ring_stack, RING_STACK);
};

static struct ubt_high high ESP_SDR_HIGH_RAM;

static struct k_thread ring_thread;
static K_MUTEX_DEFINE(radio_lock);
static K_SEM_DEFINE(run_sem, 0, 1);
static atomic_t waiters;
static atomic_t want_run;
static uint16_t run_channel;
static int run_gain = ESP_SDR_RX_GAIN_AUTO;
static bool run_started;

/* Sink side: stream index past the last sample written (also the writer position), generation. */
static atomic_t sring_w;
static atomic_t sring_gen;
static uint32_t sink_next;
static uint32_t sink_calls;
static uint32_t sink_gaps;
/* The latest hole the ring left in the stream: [gap_start, gap_end), counted by gap_seq. */
static atomic_t gap_start, gap_end, gap_seq;
/* Reader side. */
static uint32_t read_r;
static uint32_t read_gen;
static uint32_t read_gap_seq;
static uint64_t overrun_samples;

static uint32_t tune_us_last;
static uint32_t start_us_last;
static uint32_t stop_us_last;
static uint32_t runs;
static struct esp_sdr_ring_stats ring_st;

/* Cycle count at stream sample 0, from the ring position (refreshed by the reader). */
static uint64_t origin_cyc;
static bool origin_valid;

static UBT_HOT void ring_sink(void *user, const int16_t *i, const int16_t *q, size_t n,
			      uint64_t index)
{
	uint32_t at = (uint32_t)index;
	uint32_t s = at & (SRING_SAMPLES - 1U);
	size_t first = MIN(n, SRING_SAMPLES - s);

	ARG_UNUSED(user);
	sink_calls++;
	if (at != sink_next) {
		sink_gaps++;
		atomic_set(&gap_start, (atomic_val_t)sink_next);
		atomic_set(&gap_end, (atomic_val_t)at);
		atomic_inc(&gap_seq);
	}
	memcpy(&high.i[s], i, first * sizeof(int16_t));
	memcpy(&high.q[s], q, first * sizeof(int16_t));
	if (first < n) {
		memcpy(&high.i[0], &i[first], (n - first) * sizeof(int16_t));
		memcpy(&high.q[0], &q[first], (n - first) * sizeof(int16_t));
	}
	sink_next = at + (uint32_t)n;
	atomic_set(&sring_w, (atomic_val_t)sink_next);
}

static const struct esp_sdr_ring_cfg ring_cfg = {
	.decim = ESP_SDR_RING_DECIM_BOX4,
	.shift_fs4 = true,
	.sink = ring_sink,
	/* Summing four costs about 14 cycles per pair, over budget at 16 MS/s with the sink. */
	.subsample = true,
	/* Short units keep the latency of the sink low (64 us). */
	.unit_pairs = ESP_SDR_RING_UNIT_MIN,
};

/* A new run restarts the stream index at 0: a new generation for the reader. */
static void sring_new_gen(void)
{
	atomic_inc(&sring_gen);
	sink_next = 0;
	atomic_set(&sring_w, 0);
	atomic_set(&gap_start, 0);
	atomic_set(&gap_end, 0);
	atomic_set(&gap_seq, 0);
	atomic_inc(&sring_gen);
}

void *ubt_radio_scratch(size_t *bytes)
{
	*bytes = sizeof(high.i) + sizeof(high.q);
	return high.i;
}

void ubt_radio_take(void)
{
	atomic_inc(&waiters);
	/* A run clears stop requests made before it started: ask until it lets go. */
	while (k_mutex_lock(&radio_lock, K_MSEC(1)) != 0) {
		esp_sdr_ring_stop();
	}
}

void ubt_radio_give(void)
{
	atomic_dec(&waiters);
	k_mutex_unlock(&radio_lock);
}

static void ring_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		struct esp_sdr_ring_stats st = {0};
		uint64_t t0;
		int ret;

		if (atomic_get(&want_run) == 0) {
			k_sem_take(&run_sem, K_FOREVER);
			continue;
		}
		if (atomic_get(&waiters) != 0) {
			k_sleep(K_USEC(200));
			continue;
		}
		k_mutex_lock(&radio_lock, K_FOREVER);
		if (atomic_get(&want_run) == 0 || atomic_get(&waiters) != 0) {
			k_mutex_unlock(&radio_lock);
			continue;
		}
		t0 = k_cycle_get_64();
		ret = esp_sdr_set_freq((uint32_t)run_channel + UBT_RX_LO_ABOVE_MHZ);
		tune_us_last = (uint32_t)((k_cycle_get_64() - t0) / (CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / 1000000U));
		if (ret == 0) {
			ret = esp_sdr_rx_set_gain(run_gain);
		}
		if (ret != 0) {
			LOG_WRN("tune/gain failed (%d)", ret);
			atomic_set(&want_run, 0);
			k_mutex_unlock(&radio_lock);
			continue;
		}
		sring_new_gen();
		origin_valid = false;
		runs++;
		run_started = true;
		ret = esp_sdr_ring_run(&ring_cfg, &st);
		ring_st = st;
		k_mutex_unlock(&radio_lock);
		if (ret != 0) {
			LOG_WRN("ring run ended (%d, status %d)", ret, (int)st.status);
			/* A failed run is retried at once; a ring that cannot start, not so often. */
			k_sleep(ret == -EIO ? K_MSEC(1) : K_MSEC(100));
		}
	}
}

int ubt_radio_init(void)
{
	k_tid_t tid;
	int ret = esp_sdr_init();

	if (ret != 0) {
		LOG_ERR("radio init: %d", ret);
		return ret;
	}
	(void)esp_sdr_set_channel_bw(0);
	(void)esp_sdr_rx_set_bandwidth(13);
	tid = k_thread_create(&ring_thread, high.ring_stack, K_KERNEL_STACK_SIZEOF(high.ring_stack),
			      ring_thread_fn, NULL, NULL, NULL, K_PRIO_COOP(2), 0, K_FOREVER);
	k_thread_name_set(tid, "ubt_ring");
	/* The ring takes CPU 1 whole (interrupts masked); everything else runs on CPU 0. */
	(void)k_thread_cpu_pin(tid, 1);
	k_thread_start(tid);
	return 0;
}

int ubt_radio_rx_start(uint16_t channel_mhz, int gain)
{
	uint64_t t0 = k_cycle_get_64();

	ubt_radio_rx_stop();
	/* The ring is stopped: begin a new generation now, so the wait below sees the new run's samples. */
	sring_new_gen();
	run_channel = channel_mhz;
	run_gain = gain;
	run_started = false;
	read_gen = 0;
	read_r = 0;
	read_gap_seq = 0;
	overrun_samples = 0;
	sink_calls = 0;
	sink_gaps = 0;
	runs = 0;
	atomic_set(&want_run, 1);
	k_sem_give(&run_sem);
	/* The ring thread tunes (3 to 4 ms) and starts the stream; wait for the first samples. */
	for (int n = 0; n < 200 && atomic_get(&sring_w) == 0; n++) {
		k_sleep(K_MSEC(1));
	}
	start_us_last = (uint32_t)((k_cycle_get_64() - t0) / (CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / 1000000U));
	return atomic_get(&sring_w) != 0 ? 0 : -ETIMEDOUT;
}

void ubt_radio_rx_stop(void)
{
	uint64_t t0 = k_cycle_get_64();

	atomic_set(&want_run, 0);
	if (run_started || esp_sdr_ring_active()) {
		ubt_radio_take();
		ubt_radio_give();
	}
	run_started = false;
	stop_us_last = (uint32_t)((k_cycle_get_64() - t0) / (CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / 1000000U));
}

bool ubt_radio_rx_running(void)
{
	return atomic_get(&want_run) != 0;
}

size_t ubt_radio_rx_available(void)
{
	return (uint32_t)atomic_get(&sring_w) - read_r;
}

size_t ubt_radio_rx_read(int16_t *i, int16_t *q, size_t max, uint32_t *first, uint32_t *gap)
{
	uint32_t gen = (uint32_t)atomic_get(&sring_gen);
	uint32_t w = (uint32_t)atomic_get(&sring_w);
	uint32_t seq = (uint32_t)atomic_get(&gap_seq);
	uint32_t avail;
	uint32_t n;
	uint32_t s;

	*gap = 0;
	if ((gen & 1U) != 0U) {
		return 0;
	}
	if (gen != read_gen) {
		/* A new run: its stream starts at 0. */
		read_gen = gen;
		read_r = 0;
		read_gap_seq = 0;
	}
	avail = w - read_r;
	if (avail > SRING_SAMPLES) {
		/* The reader was lapped: skip to the oldest sample that is still intact. */
		uint32_t keep = SRING_SAMPLES - SRING_SAMPLES / 4U;

		*gap = avail - keep;
		overrun_samples += *gap;
		read_r = w - keep;
		avail = keep;
	}
	if (seq != read_gap_seq) {
		/* The ring left a hole [start, end) in the stream: deliver what precedes it, then skip it. */
		uint32_t start = (uint32_t)atomic_get(&gap_start);
		uint32_t end = (uint32_t)atomic_get(&gap_end);

		if ((int32_t)(start - read_r) > 0) {
			avail = MIN(avail, start - read_r);
		} else {
			if ((int32_t)(end - read_r) > 0) {
				*gap += end - read_r;
				read_r = end;
				avail = w - read_r;
			}
			read_gap_seq = seq;
		}
	}
	n = MIN((uint32_t)max, avail);
	if (n == 0U) {
		return 0;
	}
	*first = read_r;
	s = read_r & (SRING_SAMPLES - 1U);
	{
		uint32_t a = MIN(n, SRING_SAMPLES - s);

		memcpy(i, &high.i[s], a * sizeof(int16_t));
		memcpy(q, &high.q[s], a * sizeof(int16_t));
		if (a < n) {
			memcpy(&i[a], &high.i[0], (n - a) * sizeof(int16_t));
			memcpy(&q[a], &high.q[0], (n - a) * sizeof(int16_t));
		}
	}
	/* The writer lapped the reader while it copied: the chunk is damaged, drop it. */
	if ((uint32_t)atomic_get(&sring_w) - read_r > SRING_SAMPLES) {
		*gap += (uint32_t)atomic_get(&sring_w) - read_r;
		overrun_samples += (uint32_t)atomic_get(&sring_w) - read_r;
		read_r = (uint32_t)atomic_get(&sring_w);
		return 0;
	}
	read_r += n;
	return n;
}

uint64_t ubt_radio_rx_cycles(uint32_t index)
{
	if (!origin_valid) {
		uint64_t pair;
		uint64_t c0 = k_cycle_get_64();
		bool ok = esp_sdr_ring_now(&pair);
		uint64_t c1 = k_cycle_get_64();

		if (ok) {
			/* Pair p is being written now: sample p / 4, at cycle (c0 + c1) / 2. */
			origin_cyc = (c0 + c1) / 2U - (pair / 4U) * UBT_RX_CYCLES_PER_SMP;
			origin_valid = true;
		} else {
			return k_cycle_get_64();
		}
	}
	return origin_cyc + (uint64_t)index * UBT_RX_CYCLES_PER_SMP;
}

void ubt_radio_rx_get_stats(struct ubt_rx_stats *st)
{
	memset(st, 0, sizeof(*st));
	st->sink_calls = sink_calls;
	st->samples = (uint32_t)atomic_get(&sring_w);
	st->overrun_samples = overrun_samples;
	st->gaps = sink_gaps;
	st->ring_units = ring_st.units;
	st->ring_abandoned = ring_st.abandoned;
	st->ring_lost_pairs = ring_st.lost_pairs;
	st->ring_status = (uint32_t)ring_st.status;
	st->ring_cycles_x100 = ring_st.cycles_x100;
	st->runs = runs;
	st->tune_us = tune_us_last;
	st->start_us = start_us_last;
	st->stop_us = stop_us_last;
}
