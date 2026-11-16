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

#include <esp_attr.h>
#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_ring.h>
#include <esp_sdr/esp_sdr_rx.h>
#include <esp_sdr/esp_sdr_xtensa.h>

#include "ubt_attr.h"
#include "ubt_pack.h"
#include "ubt_radio.h"
#include "ubt_sring.h"

LOG_MODULE_REGISTER(ubt_radio, LOG_LEVEL_INF);

/*
 * Samples in the ring between the ring sink and the DSP thread, power of two: 2 ms at 4 MS/s. The
 * demodulator costs more than the stream's rate while its squelch is open (a packet, a Wi-Fi burst)
 * and much less while it is closed; the ring rides out the busy stretches. Stored as 8 bits (the
 * stream >> 6: the ADC's 10 bits without the lowest two, far below the noise at the modes' gain),
 * twice the samples of int16 in the same internal RAM. PSRAM is no option: its stores cost the
 * masked ring CPU some 13 cycles a pair, over the ring's budget.
 */
#define SRING_SAMPLES 8192U
#define SRING_SHIFT   6
#define RING_STACK    1536

/*
 * The internal RAM above the capture bank: the sample ring, which the modes without the stream use
 * as their scratch (spectrum analyser, transmit), and the ring thread's stack.
 */
struct ubt_high {
	union {
		struct {
			int8_t i[SRING_SAMPLES];
			int8_t q[SRING_SAMPLES];
		} ring;
		uint8_t scratch[2U * SRING_SAMPLES];
	} __aligned(16);
	K_KERNEL_STACK_MEMBER(ring_stack, RING_STACK);
};

static struct ubt_high high ESP_SDR_DATA_F(STATE, high, NOINIT);

static struct k_thread ring_thread;
static K_MUTEX_DEFINE(radio_lock);
static K_SEM_DEFINE(run_sem, 0, 1);
static atomic_t waiters;
static atomic_t want_run;
static uint16_t run_channel;
static int run_gain = ESP_SDR_RX_GAIN_AUTO;
static bool run_started;

/*
 * Sink side: stream index past the last sample written (the writer position), past the last one
 * it is about to write (published before it writes them: see ubt_sring.h), generation.
 */
static atomic_t sring_w;
static atomic_t sring_wmax;
static atomic_t sring_gen;
static uint32_t sink_next;
static uint32_t sink_calls;
static uint32_t sink_gaps;
/* The holes the ring left in the stream, in the order they came (ubt_sring.h). */
static struct ubt_sring_holes holes;
/* Reader side: the run it reads, its position and holes (the queued and lost counts since boot). */
static uint32_t read_gen;
static struct ubt_sring_rd rd;

static uint32_t tune_us_last;
static uint32_t start_us_last;
static uint32_t stop_us_last;
static uint32_t runs;
static struct esp_sdr_ring_stats ring_st;

/* Cycle count at stream sample 0, from the ring position (refreshed by the reader). */
static uint64_t origin_cyc;
static bool origin_valid;

/* The stream's int16 to the ring's 8 bits: ubt_pack() takes them all, on the vector unit. */
UBT_HOT(PACED_SAMPLE, pack)
static void pack(int8_t *di, int8_t *dq, const int16_t *i, const int16_t *q, size_t n)
{
	for (size_t k = ubt_pack(di, dq, i, q, n); k < n; k++) {
		di[k] = (int8_t)MIN(i[k] >> SRING_SHIFT, 127);
		dq[k] = (int8_t)MIN(q[k] >> SRING_SHIFT, 127);
	}
}

static inline void unpack(int16_t *i, int16_t *q, const int8_t *si, const int8_t *sq, size_t n)
{
	for (size_t k = 0; k < n; k++) {
		/* The middle of the floor's step: no bias at DC, where the channel sits. */
		i[k] = (int16_t)(si[k] * (1 << SRING_SHIFT) + (1 << (SRING_SHIFT - 1)));
		q[k] = (int16_t)(sq[k] * (1 << SRING_SHIFT) + (1 << (SRING_SHIFT - 1)));
	}
}

UBT_HOT(PACED_SAMPLE, ring_sink)
static void ring_sink(void *user, const int16_t *i, const int16_t *q, size_t n,
			      uint64_t index)
{
	uint32_t at = (uint32_t)index;
	uint32_t s = at & (SRING_SAMPLES - 1U);
	size_t first = MIN(n, SRING_SAMPLES - s);

	ARG_UNUSED(user);
	sink_calls++;
	/*
	 * Plain stores with the barriers they need (memw, s32i, and for wmax memw after it, so the
	 * slots are written after it): atomic_set() is a compare and swap loop through xtensa_cas(),
	 * some 30 cycles, against this CPU's 15 a pair.
	 */
	__atomic_store_n(&sring_wmax, (atomic_val_t)(at + (uint32_t)n), __ATOMIC_SEQ_CST);
	if (at != sink_next) {
		sink_gaps++;
		ubt_sring_hole_put(&holes, sink_next, at);
	}
	pack(&high.ring.i[s], &high.ring.q[s], i, q, first);
	if (first < n) {
		pack(high.ring.i, high.ring.q, &i[first], &q[first], n - first);
	}
	sink_next = at + (uint32_t)n;
	__atomic_store_n(&sring_w, (atomic_val_t)sink_next, __ATOMIC_RELEASE);
}

/*
 * Short units keep the latency of the sink low: 192 us (in 2048 pairs about one unit a second was
 * abandoned, in 3072 none).
 */
static struct esp_sdr_ring_cfg ring_cfg = {
	.decim = ESP_SDR_RING_DECIM_BOX4,
	.shift_fs4 = true,
	.sink = ring_sink,
};

uint32_t ubt_radio_unit_pairs;
int ubt_radio_lo_offset_mhz;

static int lo_offset(void)
{
	return ubt_radio_lo_offset_mhz != 0 ? ubt_radio_lo_offset_mhz : UBT_RX_LO_OFFSET_MHZ;
}

/* A new run restarts the stream index at 0: a new generation for the reader. */
static void sring_new_gen(void)
{
	atomic_inc(&sring_gen);
	sink_next = 0;
	atomic_set(&sring_w, 0);
	atomic_set(&sring_wmax, 0);
	ubt_sring_holes_reset(&holes);
	atomic_inc(&sring_gen);
}

void *ubt_radio_scratch(size_t *bytes)
{
	*bytes = sizeof(high.scratch);
	return high.scratch;
}

void ubt_radio_take(void)
{
	atomic_inc(&waiters);
	/*
	 * Ask first (waiting 1 ms before the first request cost every retune that much), and again
	 * until it lets go: a run clears the stop requests made before it started.
	 */
	do {
		esp_sdr_ring_stop();
	} while (k_mutex_lock(&radio_lock, K_MSEC(1)) != 0);
}

void ubt_radio_give(void)
{
	atomic_dec(&waiters);
	k_mutex_unlock(&radio_lock);
}

ESP_SDR_CODE(EVENT, ring_thread_fn)
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
		ret = esp_sdr_set_freq((uint32_t)((int)run_channel + lo_offset()));
		tune_us_last = (uint32_t)((k_cycle_get_64() - t0) / (CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / 1000000U));
		if (ret == 0 && run_gain != esp_sdr_rx_get_gain()) {
			/*
			 * A retune of its own (settling and all, some 4 ms): only when the gain changes.
			 * set_freq()'s retune applies the gain already set.
			 */
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
		ring_cfg.unit_pairs = ubt_radio_unit_pairs != 0U ? ubt_radio_unit_pairs : 3072U;
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
	ubt_sring_rd_reset(&rd);
	rd.overrun = 0;
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
	return (uint32_t)atomic_get(&sring_w) - rd.r;
}

/*
 * The reader's bookkeeping before a copy: the run, a lap, the ring's holes (ubt_sring.h). Returns
 * the samples to take now from rd.r on (0: none), with *first and *gap set. The copy reaches back
 * hist samples before rd.r: a lap is taken where those would be too old for rd_commit(), which
 * otherwise drops the chunk and the whole ring with it (some 8000 samples against a lap's 2000).
 */
UBT_HOT(CHUNK, rd_begin)
static uint32_t rd_begin(size_t max, uint32_t hist, uint32_t *first, uint32_t *gap)
{
	uint32_t gen = (uint32_t)atomic_get(&sring_gen);
	uint32_t w = (uint32_t)atomic_get(&sring_w);
	uint32_t wmax = (uint32_t)atomic_get(&sring_wmax);
	uint32_t count = (uint32_t)atomic_get(&holes.count);

	*gap = 0;
	if ((gen & 1U) != 0U || (uint32_t)atomic_get(&sring_gen) != gen) {
		/* A run begins, or began while the writer and the holes were read: nothing yet. */
		return 0;
	}
	if (gen != read_gen) {
		/* A new run: its stream starts at 0. */
		read_gen = gen;
		ubt_sring_rd_reset(&rd);
	}
	return ubt_sring_begin(&rd, &holes, w, wmax, count, SRING_SAMPLES - hist, max, first, gap);
}

UBT_HOT(CHUNK, rd_commit)
static bool rd_commit(uint32_t from, uint32_t n, uint32_t *gap)
{
	uint32_t wmax = (uint32_t)atomic_get(&sring_wmax);

	return ubt_sring_commit(&rd, wmax, (uint32_t)atomic_get(&sring_w), SRING_SAMPLES, from, n,
				gap);
}

ESP_SDR_CODE(CHUNK, ubt_radio_rx_read)
size_t ubt_radio_rx_read(int16_t *i, int16_t *q, size_t max, uint32_t *first, uint32_t *gap)
{
	uint32_t n = rd_begin(max, 0, first, gap);
	uint32_t s, a;

	if (n == 0U) {
		return 0;
	}
	s = rd.r & (SRING_SAMPLES - 1U);
	a = MIN(n, SRING_SAMPLES - s);
	unpack(i, q, &high.ring.i[s], &high.ring.q[s], a);
	if (a < n) {
		unpack(&i[a], &q[a], high.ring.i, high.ring.q, n - a);
	}
	return rd_commit(rd.r, n, gap) ? n : 0U;
}

/* The vector unit's copy (ubt_pie_copy.S): len a multiple of 16, both 16 byte aligned. */
void ubt_pie_copy(void *dst, const void *src, uint32_t len);

UBT_HOT(CHUNK, ubt_radio_rx_read8)
size_t ubt_radio_rx_read8(int8_t *bi, int8_t *bq, size_t max, uint32_t hist,
				  uint32_t *first, uint32_t *gap, size_t *off)
{
	uint32_t n = rd_begin(max, hist + 15U, first, gap);
	uint32_t from, len, done = 0;
	unsigned int key;

	if (n == 0U) {
		return 0;
	}
	/*
	 * bi[0] is stream sample from: a multiple of 16, at least hist before the first sample, so
	 * every piece is 16 byte aligned on both sides (the ring is). The vector registers are not
	 * part of the thread context: interrupts stay locked for the copy (some 0.2 us).
	 */
	from = (rd.r & ~15U) - hist;
	len = (rd.r - from + n + 15U) & ~15U;
	key = arch_irq_lock();
	esp_sdr_pie_enable();
	while (done < len) {
		uint32_t r = (from + done) & (SRING_SAMPLES - 1U);
		uint32_t take = MIN(len - done, SRING_SAMPLES - r);

		ubt_pie_copy(&bi[done], &high.ring.i[r], take);
		ubt_pie_copy(&bq[done], &high.ring.q[r], take);
		done += take;
	}
	arch_irq_unlock(key);
	*off = rd.r - from;
	return rd_commit(from, n, gap) ? n : 0U;
}

uint64_t ubt_radio_rx_cycles(uint64_t index)
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
	return origin_cyc + index * UBT_RX_CYCLES_PER_SMP;
}

void ubt_radio_rx_get_stats(struct ubt_rx_stats *st)
{
	memset(st, 0, sizeof(*st));
	st->sink_calls = sink_calls;
	st->samples = (uint32_t)atomic_get(&sring_w);
	st->overrun_samples = rd.overrun;
	st->holes_queued = rd.queued;
	st->holes_lost = rd.lost;
	st->gaps = sink_gaps;
	st->ring_units = ring_st.units;
	st->ring_abandoned = ring_st.abandoned;
	st->ring_lost_pairs = ring_st.lost_pairs;
	st->ring_status = (uint32_t)ring_st.status;
	st->ring_cycles_x100 = ring_st.cycles_x100;
	st->ring_irq_windows = ring_st.irq_windows;
	st->ring_irq_lines = ring_st.irq_lines;
	st->ring_late_max = ring_st.late_max;
	st->runs = runs;
	st->tune_us = tune_us_last;
	st->start_us = start_us_last;
	st->stop_us = stop_us_last;
}
