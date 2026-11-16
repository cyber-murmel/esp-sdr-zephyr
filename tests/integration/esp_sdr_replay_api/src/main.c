/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Hardware integration test of the DAC-engine replay (esp_sdr_replay.h): the argument
 * contract, the engine's pass time at each rate against the CPU cycle counter, the repeat
 * count (passes back to back), restarts, and the receiver afterwards; on the S31 also that the
 * engine leaves its group as it was, that the CPU, timers and sleeps go on while it plays, and
 * that a capture or a carrier is refused meanwhile. The samples are all zero, so the transmit
 * chain puts out no more than its LO leakage; keep the board on its coax link all the same.
 */
#include <errno.h>
#include <string.h>

#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_replay.h>
#include <esp_sdr/esp_sdr_rx.h>
#include <esp_sdr/esp_sdr_tx.h>

/* A pass costs the samples plus a start of about 50 CPU cycles (doc/hardware-quirks.md). */
#define START_CYCLES_MAX 400U

/* CPU cycles a word: the samples' part of a pass. */
static uint32_t per_word(enum esp_sdr_rate rate)
{
	uint32_t hz = rate == ESP_SDR_RATE_80MSPS ? 80000000U :
		      (rate == ESP_SDR_RATE_40MSPS ? 40000000U : 20000000U);

	return ESP_SDR_CPU_HZ / hz;
}

static void *suite_setup(void)
{
	zassert_ok(esp_sdr_init(), "esp_sdr_init() failed");
	zassert_ok(esp_sdr_set_freq(2437), "set_freq(2437) failed");
	return NULL;
}

/* Zero samples before each test: a capture shares the bank and leaves receive words there. */
static void before(void *f)
{
	uint32_t *buf;
	size_t words;

	ARG_UNUSED(f);
	buf = esp_sdr_replay_buf(&words);
	zassert_equal(words, ESP_SDR_REPLAY_MAX);
	memset(buf, 0, words * sizeof(uint32_t));
}

static void after(void *f)
{
	ARG_UNUSED(f);
	esp_sdr_replay_stop();
}

ZTEST_SUITE(esp_sdr_replay_api, NULL, suite_setup, before, after, NULL);

ZTEST(esp_sdr_replay_api, test_rejects_bad_config)
{
	struct esp_sdr_replay_cfg cfg = {.rate = ESP_SDR_RATE_80MSPS, .count = 1000, .passes = 1};

	cfg.count = 0;
	zassert_equal(esp_sdr_replay_start(&cfg), -EINVAL, "count 0");
	cfg.count = ESP_SDR_REPLAY_MAX + 1U;
	zassert_equal(esp_sdr_replay_start(&cfg), -EINVAL, "count past the maximum");
	cfg.count = 1000;
	cfg.passes = 256;
	zassert_equal(esp_sdr_replay_start(&cfg), -EINVAL, "256 passes");
	cfg.passes = 1;
#if defined(CONFIG_SOC_SERIES_ESP32S31)
	cfg.rate = ESP_SDR_RATE_10MSPS;
	zassert_equal(esp_sdr_replay_start(&cfg), -EINVAL, "10 MS/s");
#else
	cfg.rate = ESP_SDR_RATE_20MSPS;
	zassert_equal(esp_sdr_replay_start(&cfg), -EINVAL, "20 MS/s");
#endif
	cfg.rate = ESP_SDR_RATE_80MSPS;
	cfg.power = 16;
	zassert_equal(esp_sdr_replay_start(&cfg), -EINVAL, "power step 16");
	zassert_false(esp_sdr_replay_active());
	zassert_equal(esp_sdr_replay_wait(1000, NULL), -EINVAL, "wait without a replay");
	zassert_equal(esp_sdr_replay_restart(), -EINVAL, "restart without a replay");
}

/* Plays count words passes times at rate and checks the time against the cycle counter. */
static void timed(enum esp_sdr_rate rate, size_t count, unsigned int passes)
{
	struct esp_sdr_replay_cfg cfg = {.rate = rate, .count = count, .passes = passes};
	uint32_t want = (uint32_t)count * passes * per_word(rate), got = 0;

	zassert_ok(esp_sdr_replay_start(&cfg));
	zassert_true(esp_sdr_replay_active());
	zassert_ok(esp_sdr_replay_wait(1000000, &got), "no done flag");
	zassert_true(got >= want && got <= want + START_CYCLES_MAX,
		     "%u words x %u passes: %u cycles, want %u", (unsigned int)count, passes, got,
		     want);
	esp_sdr_replay_stop();
	zassert_false(esp_sdr_replay_active());
}

ZTEST(esp_sdr_replay_api, test_pass_80msps)
{
	timed(ESP_SDR_RATE_80MSPS, 16000, 1);
	timed(ESP_SDR_RATE_80MSPS, ESP_SDR_REPLAY_MAX, 1);
}

ZTEST(esp_sdr_replay_api, test_pass_40msps)
{
	timed(ESP_SDR_RATE_40MSPS, 16000, 1);
}

#if defined(CONFIG_SOC_SERIES_ESP32S31)
ZTEST(esp_sdr_replay_api, test_pass_20msps)
{
	timed(ESP_SDR_RATE_20MSPS, 16000, 1);
	timed(ESP_SDR_RATE_20MSPS, ESP_SDR_REPLAY_MAX, 2);
}
#endif

/* Passes follow back to back: the whole run costs one start, not one per pass. */
ZTEST(esp_sdr_replay_api, test_repeat_count)
{
	timed(ESP_SDR_RATE_80MSPS, 1000, 16);
	timed(ESP_SDR_RATE_80MSPS, 16000, 255);
	timed(ESP_SDR_RATE_40MSPS, 16000, 4);
}

ZTEST(esp_sdr_replay_api, test_endless_and_busy)
{
	struct esp_sdr_replay_cfg cfg = {.rate = ESP_SDR_RATE_80MSPS, .count = 4000, .passes = 0};

	zassert_ok(esp_sdr_replay_start(&cfg));
	zassert_equal(esp_sdr_replay_start(&cfg), -EBUSY, "second start");
	zassert_equal(esp_sdr_replay_wait(1000, NULL), -EINVAL, "wait on an endless replay");
	k_msleep(10);
	zassert_true(esp_sdr_replay_active());
	esp_sdr_replay_stop();
	zassert_false(esp_sdr_replay_active());
	esp_sdr_replay_stop();
}

ZTEST(esp_sdr_replay_api, test_restart)
{
	struct esp_sdr_replay_cfg cfg = {.rate = ESP_SDR_RATE_80MSPS, .count = 800, .passes = 1};
	uint32_t want = 800U * per_word(ESP_SDR_RATE_80MSPS), got = 0;

	zassert_ok(esp_sdr_replay_start(&cfg));
	zassert_ok(esp_sdr_replay_wait(100000, NULL));
	for (int k = 0; k < 100; k++) {
		zassert_ok(esp_sdr_replay_restart());
		zassert_ok(esp_sdr_replay_wait(100000, &got), "restart %d", k);
		zassert_true(got >= want && got <= want + START_CYCLES_MAX, "%u cycles", got);
	}
}

/* After a replay the radio is back in receive: a capture works and sees no stuck words. */
ZTEST(esp_sdr_replay_api, test_receive_after)
{
	struct esp_sdr_replay_cfg cfg = {.rate = ESP_SDR_RATE_80MSPS, .count = 4000, .passes = 0};
	struct esp_sdr_rx_burst b = {0};
	uint32_t first;
	bool varies = false;

	zassert_ok(esp_sdr_replay_start(&cfg));
	k_msleep(5);
	esp_sdr_replay_stop();
	zassert_ok(esp_sdr_rx_capture(ESP_SDR_RATE_80MSPS, 4096, &b));
	zassert_equal(b.count, 4096U);
	first = b.words[0];
	for (size_t k = 1; k < b.count && !varies; k++) {
		varies = b.words[k] != first;
	}
	zassert_true(varies, "receive words stuck at %08x", first);
}

#if defined(CONFIG_SOC_SERIES_ESP32S31)
extern uint32_t _iram_text_start[], _iram_text_end[];

static uint32_t pattern(size_t k)
{
	return (uint32_t)k * 2654435761U;
}

/* The engine reads its group and writes nothing: every word, played or not, stays. */
ZTEST(esp_sdr_replay_api, test_group_unchanged)
{
	struct esp_sdr_replay_cfg cfg = {.rate = ESP_SDR_RATE_80MSPS, .count = 4000, .passes = 0};
	size_t words, changed = 0;
	uint32_t *buf = esp_sdr_replay_buf(&words);

	/* The samples are zero (no RF beyond the leakage); the rest of the group a pattern. */
	for (size_t k = cfg.count; k < words; k++) {
		buf[k] = pattern(k);
	}
	zassert_ok(esp_sdr_replay_start(&cfg));
	k_msleep(20);
	esp_sdr_replay_stop();
	for (size_t k = 0; k < words; k++) {
		changed += buf[k] != (k < cfg.count ? 0U : pattern(k));
	}
	zassert_equal(changed, 0U, "%u words of the group changed", (unsigned int)changed);
}

static void tick(struct k_timer *t)
{
	ARG_UNUSED(t);
}

/*
 * The CPU keeps the rest of the TCM while the engine plays group 2: sleeps, a timer, a busy wait
 * and a CRC of the image's IRAM code all go on as without it, and the code is the same after.
 */
ZTEST(esp_sdr_replay_api, test_cpu_runs_while_playing)
{
	struct esp_sdr_replay_cfg cfg = {.rate = ESP_SDR_RATE_40MSPS, .count = 16000, .passes = 0};
	size_t len = (uintptr_t)_iram_text_end - (uintptr_t)_iram_text_start;
	uint32_t crc0 = crc32_ieee((const uint8_t *)_iram_text_start, len), crc;
	struct k_timer timer;
	int64_t t0, slept;
	uint32_t c0, spun, fired;

	k_timer_init(&timer, tick, NULL);
	zassert_ok(esp_sdr_replay_start(&cfg));
	t0 = k_uptime_get();
	k_msleep(100);
	slept = k_uptime_get() - t0;
	k_timer_start(&timer, K_MSEC(1), K_MSEC(1));
	k_msleep(50);
	k_timer_stop(&timer);
	fired = k_timer_status_get(&timer);
	c0 = k_cycle_get_32();
	k_busy_wait(1000);
	spun = k_cycle_get_32() - c0;
	crc = crc32_ieee((const uint8_t *)_iram_text_start, len);
	zassert_true(esp_sdr_replay_active());
	esp_sdr_replay_stop();
	zassert_true(slept >= 100 && slept <= 110, "k_msleep(100) took %lld ms", slept);
	zassert_true(fired >= 45U, "timer fired %u times in 50 ms", fired);
	zassert_true(spun >= sys_clock_hw_cycles_per_sec() / 1000U, "busy wait %u cycles", spun);
	zassert_equal(crc, crc0, "IRAM code changed while playing");
	zassert_equal(crc32_ieee((const uint8_t *)_iram_text_start, len), crc0,
		      "IRAM code changed after the replay");
}

/* A capture and the carrier would take the engine or the chain: both refused while it plays. */
ZTEST(esp_sdr_replay_api, test_refused_while_playing)
{
	struct esp_sdr_replay_cfg cfg = {.rate = ESP_SDR_RATE_80MSPS, .count = 4000, .passes = 0};
	struct esp_sdr_rx_burst b = {0};

	zassert_ok(esp_sdr_replay_start(&cfg));
	zassert_equal(esp_sdr_rx_capture(ESP_SDR_RATE_80MSPS, 4096, &b), -EBUSY, "capture");
	zassert_equal(esp_sdr_tx_tone_start(0, ESP_SDR_TX_TONE_ATT_MAX), -EBUSY, "carrier");
	zassert_true(esp_sdr_replay_active());
	esp_sdr_replay_stop();
	zassert_ok(esp_sdr_rx_capture(ESP_SDR_RATE_80MSPS, 4096, &b), "capture after");
}
#endif
