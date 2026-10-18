/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Hardware integration test of the DAC-engine replay (esp_sdr_replay.h): the argument
 * contract, the engine's pass time at 80 and 40 MS/s against the CPU cycle counter, the
 * repeat count (passes back to back), restarts, and the receiver afterwards. The samples are
 * all zero, so the transmit chain puts out no more than its LO leakage; keep the board on
 * its coax link all the same.
 */
#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_paced.h>
#include <esp_sdr/esp_sdr_replay.h>
#include <esp_sdr/esp_sdr_rx.h>

/* A pass costs the samples plus a start of about 50 CPU cycles (doc/hardware-quirks.md). */
#define START_CYCLES_MAX 400U

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
	cfg.rate = ESP_SDR_RATE_20MSPS;
	zassert_equal(esp_sdr_replay_start(&cfg), -EINVAL, "20 MS/s");
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
	uint32_t per_word = ESP_SDR_CPU_HZ / (rate == ESP_SDR_RATE_80MSPS ? 80000000U : 40000000U);
	uint32_t want = (uint32_t)count * passes * per_word, got = 0;

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
	uint32_t got = 0;

	zassert_ok(esp_sdr_replay_start(&cfg));
	zassert_ok(esp_sdr_replay_wait(100000, NULL));
	for (int k = 0; k < 100; k++) {
		zassert_ok(esp_sdr_replay_restart());
		zassert_ok(esp_sdr_replay_wait(100000, &got), "restart %d", k);
		zassert_true(got >= 800U * 3U && got <= 800U * 3U + START_CYCLES_MAX, "%u cycles", got);
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
