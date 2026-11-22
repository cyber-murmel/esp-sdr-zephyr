/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Hardware integration test of the CPU-paced transmit (esp_sdr_paced.h): the argument
 * contract, the vendor tone and the stream excluding each other, a one-second stream at
 * 250 kS/s x4 with interrupts locked keeping its rate with no late write, missing input
 * counted, the TX DC register as it was after stop, and the receiver afterwards. With
 * CONFIG_ESP_SDR_PACED_TX_BLOCK (the .smp scenario) the DAC words come from a thread on the other
 * core: more rates, restarts and the refused second start. The samples
 * are a quiet tone, 250 of 32767 (36 dB below the S31's 16384 full scale), the tone generator
 * runs at its weakest setting: keep the power low, nothing listens on purpose.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_paced.h>
#include <esp_sdr/esp_sdr_place.h>
#include <esp_sdr/esp_sdr_rx.h>
#include <esp_sdr/esp_sdr_tx.h>

#if defined(CONFIG_SOC_SERIES_ESP32S31)
/* The TX DC value register (lib/esp_sdr/src/esp_sdr_priv.h): I in bits 9:0, Q in 19:10. */
#define TXDC_REG 0x20100c08U
/* In the S31's transmit range (2200 to 2900 MHz), away from the panel's PSRAM lines. */
#define TX_MHZ  2470U
/* Outside it: the first of these the receiver tunes must refuse to transmit. */
#define NO_TX_MHZ 2100U, 2150U, 2950U, 3000U
#define RX_RATE ESP_SDR_RATE_20MSPS
#else
#error "the TX DC register and frequencies for this SoC"
#endif

#define RATE_HZ 250000U
#define INTERP  4U
#define POWER   2U

#if defined(CONFIG_ESP_SDR_PACED_TX_BLOCK)
/* Inputs the callback may be ahead of the DAC: the blocks of the ring (INTERP words an input). */
#define LOOKAHEAD (CONFIG_ESP_SDR_PACED_BLOCKS * CONFIG_ESP_SDR_PACED_BLOCK_WORDS / INTERP + 2U)
#else
#define LOOKAHEAD 0U
#endif

/*
 * One period of a tone at RATE_HZ / 16, copied in at the suite's setup. A static that is never
 * written is made read-only data in flash by the compiler, and the IRAM callback then stalled on
 * cache misses (late writes of about 2000 cycles in the per-word path, first stream on CPU 1).
 */
static int16_t tone_cos[16];
static const int16_t tone_init[16] = {250, 231, 177, 96, 0, -96, -177, -231,
				      -250, -231, -177, -96, 0, 96, 177, 231};

/* With the block path the callback runs on the fill thread: read calls after the stop. */
struct feed {
	uint32_t calls;
	bool give;
	/* The first call takes this long (a callback that blocks). */
	uint32_t slow_us;
};

ESP_SDR_CODE(PACED_SAMPLE, tone_input)
static bool tone_input(void *user, int16_t *i, int16_t *q)
{
	struct feed *f = user;
	uint32_t k;

	if (f->slow_us != 0U && f->calls == 0U) {
		k_busy_wait(f->slow_us);
	}
	k = f->calls++ & 15U;

	if (!f->give) {
		return false;
	}
	*i = tone_cos[k];
	*q = tone_cos[(k + 12U) & 15U];
	return true;
}

/*
 * The stream for @p cycles CPU cycles, everything it runs in IRAM, interrupts locked: the local
 * mask, irq_lock() under SMP is the global lock. @return the sync's result (nothing ran if not 0).
 */
ESP_SDR_CODE(PACED_CHUNK, run_locked)
static __noinline int run_locked(uint32_t cycles)
{
	unsigned int key = arch_irq_lock();
	int ret = esp_sdr_paced_sync();
	uint32_t c0 = esp_sdr_paced_cycles();

	while (ret == 0 && esp_sdr_paced_cycles() - c0 < cycles) {
		(void)esp_sdr_tx_paced_poll();
	}
	arch_irq_unlock(key);
	return ret;
}

/*
 * The floor's power: the third lowest of nine captures' mean square about their mean, in 1/256
 * of a raw unit squared. Wi-Fi frames only add power and may cover several of the 0.2 ms
 * captures; a receiver left in a wrong state raises all of them. All nine are printed.
 */
#define FLOOR_CAPTURES 9

static uint32_t floor_power(const char *tag)
{
	static struct esp_sdr_iq16 iq[4096];
	uint32_t p[FLOOR_CAPTURES];

	for (int c = 0; c < FLOOR_CAPTURES; c++) {
		int64_t si = 0, sq = 0, ss = 0, n = ARRAY_SIZE(iq);
		uint64_t t;

		zassert_ok(esp_sdr_rx_capture_iq(RX_RATE, ARRAY_SIZE(iq), iq, &t));
		for (size_t k = 0; k < ARRAY_SIZE(iq); k++) {
			si += iq[k].i;
			sq += iq[k].q;
			ss += (int32_t)iq[k].i * iq[k].i + (int32_t)iq[k].q * iq[k].q;
		}
		p[c] = (uint32_t)((256 * (ss * n - si * si - sq * sq)) / (n * n));
	}
	TC_PRINT("floor %s:", tag);
	for (int c = 0; c < FLOOR_CAPTURES; c++) {
		TC_PRINT(" %u", p[c]);
	}
	TC_PRINT("\n");
	for (int a = 0; a < FLOOR_CAPTURES; a++) {
		for (int b = a + 1; b < FLOOR_CAPTURES; b++) {
			if (p[b] < p[a]) {
				uint32_t t = p[a];

				p[a] = p[b];
				p[b] = t;
			}
		}
	}
	return p[2];
}

static void *suite_setup(void)
{
	memcpy(tone_cos, tone_init, sizeof(tone_cos));
	zassert_ok(esp_sdr_init(), "esp_sdr_init() failed");
	zassert_ok(esp_sdr_set_freq(TX_MHZ), "set_freq failed");
	return NULL;
}

static void after(void *f)
{
	ARG_UNUSED(f);
	esp_sdr_tx_paced_stop();
	esp_sdr_tx_tone_stop();
	(void)esp_sdr_set_freq(TX_MHZ);
}

ZTEST_SUITE(esp_sdr_tx_paced_api, NULL, suite_setup, NULL, after, NULL);

ZTEST(esp_sdr_tx_paced_api, test_rejects_bad_args)
{
	static const unsigned int no_tx[] = {NO_TX_MHZ};
	struct feed f = {.give = true};

	zassert_equal(esp_sdr_tx_paced_start(0, 1, POWER, tone_input, &f), -EINVAL, "rate 0");
	zassert_equal(esp_sdr_tx_paced_start(RATE_HZ, 3, POWER, tone_input, &f), -EINVAL, "x3");
	zassert_equal(esp_sdr_tx_paced_start(RATE_HZ, 16, POWER, tone_input, &f), -EINVAL, "x16");
	/* 48 kS/s: no whole number of CPU cycles a sample. */
	zassert_equal(esp_sdr_tx_paced_start(48000, 1, POWER, tone_input, &f), -EINVAL, "48 kS/s");
	zassert_equal(esp_sdr_tx_paced_start(RATE_HZ, 1, esp_sdr_tx_paced_power_max() + 1U,
					     tone_input, &f), -EINVAL, "power past the maximum");
	zassert_equal(esp_sdr_tx_paced_start(RATE_HZ, 1, POWER, NULL, &f), -EINVAL, "no input");
	/*
	 * esp_sdr_set_freq() keeps the setting even when the S31's PLL does not lock there (-ERANGE,
	 * which is what it does outside 2200 to 2900 MHz on the boards seen): the stream refuses it
	 * either way.
	 */
	ARRAY_FOR_EACH(no_tx, k) {
		int ret = esp_sdr_set_freq(no_tx[k]);

		zassert_true(ret == 0 || ret == -ERANGE, "%u MHz: set_freq %d", no_tx[k], ret);
		zassert_equal(esp_sdr_get_freq(), no_tx[k], "%u MHz not kept", no_tx[k]);
		zassert_equal(esp_sdr_tx_paced_start(RATE_HZ, 1, POWER, tone_input, &f), -ERANGE,
			      "%u MHz", no_tx[k]);
	}
	zassert_ok(esp_sdr_set_freq(TX_MHZ));
	zassert_equal(f.calls, 0U, "the input ran on a refused start");
}

ZTEST(esp_sdr_tx_paced_api, test_tone_and_stream_exclude)
{
	struct feed f = {.give = true};

	zassert_ok(esp_sdr_tx_tone_start(0, ESP_SDR_TX_TONE_ATT_MAX));
	zassert_equal(esp_sdr_tx_paced_start(RATE_HZ, INTERP, POWER, tone_input, &f), -EBUSY,
		      "stream beside the tone");
	esp_sdr_tx_tone_stop();
	zassert_ok(esp_sdr_tx_paced_start(RATE_HZ, INTERP, POWER, tone_input, &f));
	zassert_equal(esp_sdr_tx_tone_start(0, ESP_SDR_TX_TONE_ATT_MAX), -EBUSY,
		      "tone beside the stream");
	esp_sdr_tx_paced_stop();
	/* Stop twice is a no-op; the tone works again after it. */
	esp_sdr_tx_paced_stop();
	zassert_ok(esp_sdr_tx_tone_start(0, ESP_SDR_TX_TONE_ATT_MAX));
	esp_sdr_tx_tone_stop();
}

ZTEST(esp_sdr_tx_paced_api, test_one_second_locked)
{
	struct feed f = {.give = true};
	struct esp_sdr_paced_stats st;
	uint32_t before = sys_read32(TXDC_REG);

	zassert_ok(esp_sdr_tx_paced_start(RATE_HZ, INTERP, POWER, tone_input, &f));
	zassert_ok(run_locked(ESP_SDR_CPU_HZ));
	esp_sdr_paced_get_stats(&st);
	esp_sdr_tx_paced_stop();
	TC_PRINT("%u kS/s x%u: samples %llu late %u late_max %u cycles starved %u events %u\n",
		 RATE_HZ / 1000U, INTERP, (unsigned long long)st.samples, st.late, st.late_max,
		 st.starved, st.events);
	zassert_true(st.samples >= RATE_HZ - 1U && st.samples <= RATE_HZ + 1U,
		     "%llu samples in 1 s", (unsigned long long)st.samples);
	/* The block path takes the inputs blocks before they play; the stats count played. */
	zassert_true(f.calls >= (uint32_t)st.samples &&
		     f.calls - (uint32_t)st.samples <= LOOKAHEAD, "input calls %u, samples %llu",
		     f.calls, (unsigned long long)st.samples);
	zassert_equal(st.late, 0U, "%u late writes, worst %u cycles", st.late, st.late_max);
	zassert_equal(st.starved, 0U, "%u blocks not ready", st.starved);
	zassert_equal(st.events, 0U, "missing input");
	zassert_equal(sys_read32(TXDC_REG), before, "TX DC %08x, was %08x", sys_read32(TXDC_REG),
		      before);
}

/* Every rate the interpolator takes, briefly: each keeps its rate. */
ZTEST(esp_sdr_tx_paced_api, test_interp_rates)
{
	static const uint32_t rate[] = {250000, 250000, 250000, 125000, 125000, 125000};
	static const unsigned int interp[] = {1, 2, 4, 2, 4, 8};

	for (size_t k = 0; k < ARRAY_SIZE(rate); k++) {
		struct feed f = {.give = true};
		struct esp_sdr_paced_stats st;

		zassert_ok(esp_sdr_tx_paced_start(rate[k], interp[k], POWER, tone_input, &f));
		zassert_ok(run_locked(ESP_SDR_CPU_HZ / 10U));
		esp_sdr_paced_get_stats(&st);
		esp_sdr_tx_paced_stop();
		TC_PRINT("%u kS/s x%u: samples %llu late %u late_max %u cycles starved %u\n",
			 rate[k] / 1000U, interp[k], (unsigned long long)st.samples, st.late,
			 st.late_max, st.starved);
		zassert_true(st.samples + 1U >= rate[k] / 10U && st.samples <= rate[k] / 10U + 1U,
			     "%u x%u: %llu samples in 100 ms", rate[k], interp[k],
			     (unsigned long long)st.samples);
		zassert_equal(st.late, 0U, "%u x%u: %u late, worst %u cycles", rate[k], interp[k],
			      st.late, st.late_max);
		zassert_equal(st.starved, 0U, "%u x%u: %u starved", rate[k], interp[k], st.starved);
	}
}

/*
 * The rates the block path adds, writes of 2 MHz and a ladder above it: 160 CPU cycles a write,
 * one core stores, the other computes. Up to 2 MHz of writes every row is asserted; above it the
 * result is printed only (the per-word path needs about 220 cycles a write).
 */
ZTEST(esp_sdr_tx_paced_api, test_block_rates)
{
	static const struct {
		uint32_t rate;
		unsigned int interp;
		uint32_t ms;
		bool assert;
	} row[] = {
		{250000, 8, 1000, true},
		{500000, 4, 1000, true},
		{400000, 4, 500, true},
		{125000, 8, 500, true},
		{625000, 4, 200, false},
		{400000, 8, 200, false},
		{800000, 4, 200, false},
	};

#if !defined(CONFIG_ESP_SDR_PACED_TX_BLOCK)
	ztest_test_skip();
#endif
	for (size_t k = 0; k < ARRAY_SIZE(row); k++) {
		struct feed f = {.give = true};
		struct esp_sdr_paced_stats st;
		uint32_t rate = row[k].rate, step = ESP_SDR_CPU_HZ / (rate * row[k].interp);
		uint64_t want = (uint64_t)rate * row[k].ms / 1000U;

		zassert_ok(esp_sdr_tx_paced_start(rate, row[k].interp, POWER, tone_input, &f));
		zassert_ok(run_locked((uint32_t)((uint64_t)row[k].ms * (ESP_SDR_CPU_HZ / 1000U))));
		esp_sdr_paced_get_stats(&st);
		esp_sdr_tx_paced_stop();
		TC_PRINT("%u kS/s x%u (%u cycles a write): samples %llu late %u late_max %u cycles "
			 "starved %u events %u%s\n", rate / 1000U, row[k].interp, step,
			 (unsigned long long)st.samples, st.late, st.late_max, st.starved,
			 st.events, row[k].assert ? "" : " (not asserted)");
		if (!row[k].assert) {
			continue;
		}
		zassert_true(st.samples + 1U >= want && st.samples <= want + 1U,
			     "%u x%u: %llu samples of %llu", rate, row[k].interp,
			     (unsigned long long)st.samples, (unsigned long long)want);
		zassert_equal(st.late, 0U, "%u x%u: %u late, worst %u cycles", rate, row[k].interp,
			      st.late, st.late_max);
		zassert_true(st.late_max < step, "%u x%u: worst %u of %u cycles", rate,
			     row[k].interp, st.late_max, step);
		zassert_equal(st.starved, 0U, "%u x%u: %u starved", rate, row[k].interp, st.starved);
		zassert_equal(st.events, 0U, "missing input");
	}
}

/* Stop and start again, cleanly; a second start while one runs is refused and changes nothing. */
ZTEST(esp_sdr_tx_paced_api, test_block_restart_and_busy)
{
#if !defined(CONFIG_ESP_SDR_PACED_TX_BLOCK)
	ztest_test_skip();
#endif
	for (int round = 0; round < 3; round++) {
		struct feed f = {.give = true}, g = {.give = true};
		struct esp_sdr_paced_stats st;

		zassert_ok(esp_sdr_tx_paced_start(RATE_HZ, INTERP, POWER, tone_input, &f));
		zassert_equal(esp_sdr_tx_paced_start(RATE_HZ, INTERP, POWER, tone_input, &g),
			      -EBUSY, "second start");
		zassert_ok(run_locked(ESP_SDR_CPU_HZ / 20U));
		esp_sdr_paced_get_stats(&st);
		esp_sdr_tx_paced_stop();
		zassert_equal(g.calls, 0U, "the refused start's input ran");
		zassert_true(st.samples + 1U >= RATE_HZ / 20U, "round %d: %llu samples", round,
			     (unsigned long long)st.samples);
		zassert_equal(st.late, 0U, "round %d: %u late", round, st.late);
		zassert_equal(st.starved, 0U, "round %d: %u starved", round, st.starved);
	}
}

/* An input that has nothing plays zeros and counts each of them. */
ZTEST(esp_sdr_tx_paced_api, test_missing_input_counted)
{
	struct feed f = {.give = false};
	struct esp_sdr_paced_stats st;

	zassert_ok(esp_sdr_tx_paced_start(RATE_HZ, INTERP, POWER, tone_input, &f));
	zassert_ok(run_locked(ESP_SDR_CPU_HZ / 100U));
	esp_sdr_paced_get_stats(&st);
	esp_sdr_tx_paced_stop();
	zassert_true(st.samples > 0U);
	zassert_equal(st.events, (uint32_t)st.samples, "events %u of %llu", st.events,
		      (unsigned long long)st.samples);
}

/*
 * After a stream the radio receives as before, at a fixed gain: the floor's power within 3 dB
 * of before (over the air two sets before a stream differ by up to 1.4 dB; a receiver left
 * transmitting or at another gain is off by far more).
 */
ZTEST(esp_sdr_tx_paced_api, test_receive_after)
{
	struct feed f = {.give = true};
	uint32_t before, after_p;

	zassert_ok(esp_sdr_rx_set_gain(40));
	before = floor_power("before");
	zassert_ok(esp_sdr_tx_paced_start(RATE_HZ, INTERP, POWER, tone_input, &f));
	zassert_ok(run_locked(ESP_SDR_CPU_HZ / 10U));
	esp_sdr_tx_paced_stop();
	after_p = floor_power("after");
	zassert_ok(esp_sdr_rx_set_gain(ESP_SDR_RX_GAIN_AUTO));
	TC_PRINT("floor power %u before, %u after (1/256 unit^2)\n", before, after_p);
	zassert_true(before > 0U, "a dead receiver before");
	zassert_true(after_p <= 2U * before && 2U * after_p >= before, "power %u after, %u before",
		     after_p, before);
}

/* Polls for @p cycles with no sync, interrupts locked. */
ESP_SDR_CODE(PACED_CHUNK, poll_locked)
static __noinline void poll_locked(uint32_t cycles)
{
	unsigned int key = arch_irq_lock();
	uint32_t c0 = esp_sdr_paced_cycles();

	while (esp_sdr_paced_cycles() - c0 < cycles) {
		(void)esp_sdr_tx_paced_poll();
	}
	arch_irq_unlock(key);
}

/*
 * A callback that blocks 1.5 s on its first input: the sync gives up after 1 s (-ETIMEDOUT), the
 * poll then plays the idle word (nothing played, starved counted), a second sync succeeds once
 * the fill is through, and a new stream afterwards is clean.
 */
ZTEST(esp_sdr_tx_paced_api, test_block_sync_timeout)
{
	struct feed f = {.give = true, .slow_us = 1500000U}, g = {.give = true};
	struct esp_sdr_paced_stats st;

#if !defined(CONFIG_ESP_SDR_PACED_TX_BLOCK)
	ztest_test_skip();
#endif
	zassert_ok(esp_sdr_tx_paced_start(RATE_HZ, INTERP, POWER, tone_input, &f));
	zassert_equal(run_locked(ESP_SDR_CPU_HZ / 100U), -ETIMEDOUT, "sync with a blocked fill");
	poll_locked(ESP_SDR_CPU_HZ / 100U);
	esp_sdr_paced_get_stats(&st);
	zassert_equal(st.samples, 0U, "%llu samples played from nothing", (unsigned long long)st.samples);
	zassert_true(st.starved > 0U, "no replay of the idle word counted");
	zassert_ok(run_locked(ESP_SDR_CPU_HZ / 50U), "second sync");
	esp_sdr_paced_get_stats(&st);
	zassert_true(st.samples > 0U, "nothing played after the second sync");
	esp_sdr_tx_paced_stop();
	zassert_ok(esp_sdr_tx_paced_start(RATE_HZ, INTERP, POWER, tone_input, &g));
	zassert_ok(run_locked(ESP_SDR_CPU_HZ / 20U));
	esp_sdr_paced_get_stats(&st);
	esp_sdr_tx_paced_stop();
	zassert_equal(st.late, 0U, "%u late after the restart", st.late);
	zassert_equal(st.starved, 0U, "%u starved after the restart", st.starved);
	zassert_true(st.samples + 1U >= RATE_HZ / 20U, "%llu samples", (unsigned long long)st.samples);
}

#if defined(CONFIG_SMP)
struct cpu1_job {
	struct feed f;
	struct esp_sdr_paced_stats st;
	int start, sync;
};

static K_THREAD_STACK_DEFINE(cpu1_stack, 2048);
static struct k_thread cpu1_thread;

/* The whole stream from a thread on CPU 1: the fill thread goes to CPU 0. */
static void cpu1_main(void *a, void *b, void *c)
{
	struct cpu1_job *j = a;

	ARG_UNUSED(b);
	ARG_UNUSED(c);
	j->start = esp_sdr_tx_paced_start(RATE_HZ, INTERP, POWER, tone_input, &j->f);
	if (j->start == 0) {
		j->sync = run_locked(ESP_SDR_CPU_HZ / 5U);
		esp_sdr_paced_get_stats(&j->st);
		esp_sdr_tx_paced_stop();
	}
}
#endif

/* The caller on CPU 1, the fill (the block path) on CPU 0: the same stream, late 0. */
ZTEST(esp_sdr_tx_paced_api, test_caller_on_cpu1)
{
#if defined(CONFIG_SMP)
	static struct cpu1_job job;

	if (arch_num_cpus() < 2U) {
		ztest_test_skip();
	}
	memset(&job, 0, sizeof(job));
	job.f.give = true;
	k_thread_create(&cpu1_thread, cpu1_stack, K_THREAD_STACK_SIZEOF(cpu1_stack), cpu1_main,
			&job, NULL, NULL, K_PRIO_PREEMPT(5), 0, K_FOREVER);
	zassert_ok(k_thread_cpu_pin(&cpu1_thread, 1), "pin");
	k_thread_start(&cpu1_thread);
	zassert_ok(k_thread_join(&cpu1_thread, K_SECONDS(10)), "the stream thread did not end");
	TC_PRINT("caller on CPU 1: samples %llu late %u late_max %u cycles starved %u\n",
		 (unsigned long long)job.st.samples, job.st.late, job.st.late_max, job.st.starved);
	zassert_ok(job.start);
	zassert_ok(job.sync);
	zassert_true(job.st.samples + 1U >= RATE_HZ / 5U, "%llu samples",
		     (unsigned long long)job.st.samples);
	zassert_equal(job.st.late, 0U, "%u late, worst %u cycles", job.st.late, job.st.late_max);
	zassert_equal(job.st.starved, 0U, "%u starved", job.st.starved);
#else
	ztest_test_skip();
#endif
}
