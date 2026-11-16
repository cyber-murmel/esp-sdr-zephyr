/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Hardware integration test of the S31's dump ring (esp_sdr_dumpring.h): the argument contract,
 * a second's words against its time (a word lost or doubled anywhere ends the run with -EIO, a
 * slow sink with -ENOBUFS), what the turns cost, a capture refused from inside a run, a second run
 * and a capture after, and with two cores what a busy thread on the other one keeps. Receive only.
 */
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_dumpring.h>
#include <esp_sdr/esp_sdr_rx.h>

#define RUN_MS    1000
/* A unit and a turn's words, and the settling words, at most: what a run need not give. */
#define SLACK     4096U

static struct {
	uint64_t words;
	uint32_t calls, marks;
	int capture_ret;
	bool try_capture;
} sk;

static void sink(void *user, const uint32_t *words, size_t n)
{
	ARG_UNUSED(user);
	/*
	 * The run's sentinel (esp_sdr_dumpring.c) is no sample: one here is a word never
	 * written. Every 8th word, 8 loads at a time: a TCM load stalls its use some 5 cycles,
	 * and a sink must keep up with 20 MS/s.
	 */
	for (size_t k = 0; k + 64U <= n; k += 64U) {
		uint32_t w[8];

		for (unsigned int j = 0; j < 8U; j++) {
			w[j] = words[k + 8U * j];
		}
		for (unsigned int j = 0; j < 8U; j++) {
			sk.marks += w[j] == 0xa5c33c5aU;
		}
	}
	sk.words += n;
	sk.calls++;
	if (sk.try_capture) {
		struct esp_sdr_rx_burst b = {0};

		sk.try_capture = false;
		sk.capture_ret = esp_sdr_rx_capture(ESP_SDR_RATE_80MSPS, 1024, &b);
	}
}

static void stop_later(struct k_timer *t)
{
	ARG_UNUSED(t);
	esp_sdr_dumpring_stop();
}

K_TIMER_DEFINE(stopper, stop_later, NULL);

static int run_for(int ms, struct esp_sdr_dumpring_stats *st)
{
	const struct esp_sdr_dumpring_cfg cfg = {.rate = ESP_SDR_RATE_20MSPS, .sink = sink};

	sk = (typeof(sk)){0};
	k_timer_start(&stopper, K_MSEC(ms), K_NO_WAIT);
	return esp_sdr_dumpring_run(&cfg, st);
}

static void *suite_setup(void)
{
	zassert_ok(esp_sdr_init(), "esp_sdr_init() failed");
	zassert_ok(esp_sdr_set_freq(2470), "set_freq(2470) failed");
	return NULL;
}

ZTEST_SUITE(esp_sdr_dumpring_api, NULL, suite_setup, NULL, NULL, NULL);

ZTEST(esp_sdr_dumpring_api, test_bad_config)
{
	struct esp_sdr_dumpring_cfg cfg = {.rate = ESP_SDR_RATE_80MSPS, .sink = sink};

	zassert_equal(esp_sdr_dumpring_run(&cfg, NULL), -EINVAL, "80 MS/s");
	cfg.rate = ESP_SDR_RATE_20MSPS;
	cfg.sink = NULL;
	zassert_equal(esp_sdr_dumpring_run(&cfg, NULL), -EINVAL, "no sink");
}

/* Every word of a second: the count follows the run's own clock. */
ZTEST(esp_sdr_dumpring_api, test_one_second_gapless)
{
	struct esp_sdr_dumpring_stats st;
	int ret = run_for(RUN_MS, &st);
	uint64_t want = st.cycles * 20000000ULL / ESP_SDR_CPU_HZ;

	TC_PRINT("words %llu of %llu, %u turns, late max %u, turn max %u cycles, turns %u.%u %%\n",
		 st.words, want, st.turns, st.late_max, st.turn_cycles_max,
		 (unsigned int)(st.turn_cycles * 100U / MAX(st.cycles, 1U)),
		 (unsigned int)(st.turn_cycles * 1000U / MAX(st.cycles, 1U) % 10U));
	zassert_ok(ret, "run ended with %d after %llu words", ret, st.words);
	zassert_equal(sk.words, st.words, "the sink saw %llu", sk.words);
	zassert_true(st.words <= want && st.words + SLACK >= want, "%llu words for %llu",
		     st.words, want);
	zassert_equal(sk.marks, 0U, "%u sentinels among the words", sk.marks);
	zassert_true(st.turn_cycles_max < 40U * (ESP_SDR_CPU_HZ / 1000000U),
		     "a turn took %u cycles", st.turn_cycles_max);
	zassert_true(st.turn_cycles * 4U < st.cycles, "turns took %llu of %llu cycles",
		     st.turn_cycles, st.cycles);
}

/* The engine is the run's: a capture from its own thread is refused, then works after. */
ZTEST(esp_sdr_dumpring_api, test_capture_refused_then_works)
{
	struct esp_sdr_rx_burst b = {0};
	struct esp_sdr_dumpring_stats st;

	sk.try_capture = true;
	k_timer_start(&stopper, K_MSEC(100), K_NO_WAIT);
	{
		const struct esp_sdr_dumpring_cfg cfg = {.rate = ESP_SDR_RATE_20MSPS, .sink = sink};

		zassert_ok(esp_sdr_dumpring_run(&cfg, &st));
	}
	zassert_equal(sk.capture_ret, -EBUSY, "capture in the run gave %d", sk.capture_ret);
	zassert_ok(esp_sdr_rx_capture(ESP_SDR_RATE_80MSPS, 4096, &b), "capture after");
	zassert_ok(run_for(100, &st), "a second run");
	zassert_ok(esp_sdr_rx_capture(ESP_SDR_RATE_20MSPS, 4096, &b), "capture after that");
}

#if defined(CONFIG_SMP)
static volatile bool spin_on;
static volatile uint64_t spun;

static void spinner(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	while (spin_on) {
		spun++;
	}
}

K_THREAD_STACK_DEFINE(spin_stack, 1024);
static struct k_thread spin_thread;

static uint64_t spin_for(int ms, bool ring)
{
	struct esp_sdr_dumpring_stats st;

	spun = 0U;
	spin_on = true;
	k_thread_create(&spin_thread, spin_stack, K_THREAD_STACK_SIZEOF(spin_stack), spinner, NULL,
			NULL, NULL, K_PRIO_PREEMPT(10), 0, K_NO_WAIT);
	if (ring) {
		zassert_ok(run_for(ms, &st));
	} else {
		k_busy_wait(ms * 1000U);
	}
	spin_on = false;
	k_thread_join(&spin_thread, K_FOREVER);
	return spun;
}

/* The other core waits only while a turn lasts: a fifth of the time at 20 MS/s, some 27 % of its
 * work with the stall interrupt's way in and out. */
ZTEST(esp_sdr_dumpring_api, test_other_core_keeps_most)
{
	uint64_t idle = spin_for(500, false), ringing = spin_for(500, true);

	TC_PRINT("other core: %llu loops idle, %llu during a run (%u %%)\n", idle, ringing,
		 (unsigned int)(ringing * 100U / MAX(idle, 1U)));
	zassert_true(ringing * 10U > idle * 7U, "%llu of %llu", ringing, idle);
}
#endif

