/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The S31's gapless ring on a real board: the configuration's checks, a run's byte count against
 * the clock, no gaps, the hole left alone, and the sampling phase: at this gain, on a quiet band, a
 * large sample cannot be real, and noise crosses zero all the time, which is what shows a bad phase:
 * a sign taken from the next sample makes a crossing a large sample, up to thousands in a node.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_diag16.h>
#include <esp_sdr/esp_sdr_rx.h>

#define HOLE_FILL 0xa5U
/* The noise here is some 1.3 steps rms. */
#define QUIET_GAIN 54
#define LARGE     32
/* Look at one node in this many: enough to catch a phase, at any rate. */
#define SCAN_MASK 15U
/* A real spike on a quiet band; a bad phase makes tens of thousands in a run. */
#define LARGE_MAX 400U

struct check {
	uint64_t expect;
	uint32_t gaps;
	uint32_t nodes;
	uint32_t scanned;
	uint32_t large;
	uint32_t hole_bad;
	/* A node a sink gets is finished and still whole, by the API that tells. */
	uint32_t node_bad;
	/* The hole is the sink's: filled here, it must read the same a lap later. */
	uint8_t *marked;
	uint64_t marked_at;
};

static struct check c;

static void sink(void *user, uint8_t *node, uint64_t index)
{
	struct check *ck = user;
	uint64_t nr = index / ESP_SDR_DIAG16_PAYLOAD;

	if (esp_sdr_diag16_produced() < index + ESP_SDR_DIAG16_PAYLOAD ||
	    !esp_sdr_diag16_node_valid(index)) {
		ck->node_bad++;
	}
	if (index != ck->expect) {
		ck->gaps++;
	}
	ck->expect = index + ESP_SDR_DIAG16_PAYLOAD;
	ck->nodes++;
	if (ck->marked == node) {
		/* The DMA has been through this node again since. */
		for (unsigned int j = 0; j < ESP_SDR_DIAG16_HOLE; j++) {
			if (node[j] != HOLE_FILL) {
				ck->hole_bad++;
				break;
			}
		}
		ck->marked = NULL;
	}
	if (ck->marked == NULL) {
		memset(node, HOLE_FILL, ESP_SDR_DIAG16_HOLE);
		ck->marked = node;
		ck->marked_at = nr;
	}
	if ((nr & SCAN_MASK) == 0U) {
		const int8_t *s = (const int8_t *)(node + ESP_SDR_DIAG16_HOLE);

		for (size_t j = 0; j < ESP_SDR_DIAG16_PAYLOAD; j++) {
			if (abs(s[j]) >= LARGE) {
				ck->large++;
			}
		}
		ck->scanned++;
	}
}

static int run(uint32_t rate, uint32_t seconds, struct esp_sdr_diag16_stats *s)
{
	struct esp_sdr_diag16_cfg cfg = {
		.rate_hz = rate,
		.seconds = seconds,
		.sink = sink,
		.user = &c,
	};

	memset(&c, 0, sizeof(c));
	return esp_sdr_diag16_run(&cfg, s);
}

static void *suite_setup(void)
{
	zassert_ok(esp_sdr_init(), "esp_sdr_init() failed");
	/* Away from Wi-Fi: real signals must not look like a bad phase. */
	zassert_ok(esp_sdr_set_freq(2350), "set_freq(2350) failed");
	return NULL;
}

ZTEST_SUITE(esp_sdr_diag16_api, NULL, suite_setup, NULL, NULL, NULL);

ZTEST(esp_sdr_diag16_api, test_bad_configs)
{
	static const int8_t reserved[ESP_SDR_DIAG16_PINS] = {
		29, 21, 22, 23, 24, 25, 47, 48, 36, 37, 38, 39, 40, 42, 43, 44,
	};
	struct esp_sdr_diag16_cfg cfg = {.rate_hz = 20000000U, .seconds = 1U, .sink = sink};

	zassert_equal(esp_sdr_diag16_run(NULL, NULL), -EINVAL);
	cfg.sink = NULL;
	zassert_equal(esp_sdr_diag16_run(&cfg, NULL), -EINVAL, "a sink is required");
	cfg.sink = sink;
	cfg.rate_hz = 80000000U;
	zassert_equal(esp_sdr_diag16_run(&cfg, NULL), -EINVAL, "above the ring's top rate");
	cfg.rate_hz = 3000000U;
	zassert_equal(esp_sdr_diag16_run(&cfg, NULL), -EINVAL, "no divider of 160 MHz");
	cfg.rate_hz = 32000000U;
	zassert_equal(esp_sdr_diag16_run(&cfg, NULL), -EINVAL, "an odd divider of 160 MHz");
	cfg.rate_hz = 500000U;
	zassert_equal(esp_sdr_diag16_run(&cfg, NULL), -EINVAL, "too large a divider");
	cfg.rate_hz = 20000000U;
	cfg.wake_nodes = CONFIG_ESP_SDR_DIAG16_RING_KB / 4U / 2U + 1U;
	zassert_equal(esp_sdr_diag16_run(&cfg, NULL), -EINVAL, "waking too rarely to keep up");
	cfg.wake_nodes = 0U;
	cfg.pins = reserved;
	zassert_equal(esp_sdr_diag16_run(&cfg, NULL), -EINVAL, "GPIO 29 is not bonded");
}

static void check_rate(uint32_t rate)
{
	struct esp_sdr_diag16_stats s = {0};
	uint64_t want = (uint64_t)rate * 2U * 2U;
	int ret;

	zassert_ok(esp_sdr_rx_set_gain(QUIET_GAIN));
	ret = run(rate, 2U, &s);
	zassert_ok(ret, "%u S/s: run failed (%d)", rate, ret);
	zassert_false(s.phase_bad, "%u S/s: no clean sampling phase", rate);
	zassert_false(s.overrun, "%u S/s: the interrupt fell behind", rate);
	zassert_equal(s.skips, 0, "%u S/s: the consumer fell behind %u times", rate, s.skips);
	zassert_equal(c.gaps, 0, "%u S/s: the sink saw %u gaps", rate, c.gaps);
	zassert_equal(s.byte_rate, rate * 2U);
	/* A second's worth off at most: the phase check and the stop come on top of the two. */
	zassert_true(s.bytes + rate * 2U >= want, "%u S/s: only %llu bytes", rate, s.bytes);
	zassert_true(abs(s.ppm_x100) < 10000, "%u S/s: %d ppm/100 against the timer", rate,
		     s.ppm_x100);
	zassert_equal(c.hole_bad, 0, "%u S/s: the DMA wrote into the hole", rate);
	zassert_equal(c.node_bad, 0, "%u S/s: %u nodes unfinished or stale by the API", rate,
		      c.node_bad);
	zassert_true(c.scanned > 0U, "%u S/s: nothing to look at", rate);
	zassert_true(c.large <= LARGE_MAX, "%u S/s: %u large samples of %u nodes, %u slips", rate,
		     c.large, c.scanned, s.phase_tries);
}

ZTEST(esp_sdr_diag16_api, test_40_msps)
{
	check_rate(40000000U);
}

ZTEST(esp_sdr_diag16_api, test_20_msps)
{
	check_rate(20000000U);
}

ZTEST(esp_sdr_diag16_api, test_10_msps)
{
	check_rate(10000000U);
}

ZTEST(esp_sdr_diag16_api, test_5_msps)
{
	check_rate(5000000U);
}

/* Starts land on either sampling phase: every one of them must end up on the good one. */
ZTEST(esp_sdr_diag16_api, test_every_start_finds_the_phase)
{
	zassert_ok(esp_sdr_rx_set_gain(QUIET_GAIN));
	for (unsigned int k = 0; k < 12U; k++) {
		struct esp_sdr_diag16_stats s = {0};

		zassert_ok(run(40000000U, 1U, &s));
		zassert_false(s.phase_bad, "start %u: no clean sampling phase", k);
		zassert_true(c.large <= LARGE_MAX, "start %u: %u large samples, %u slips", k, c.large,
			     s.phase_tries);
	}
}

static void stopper_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	esp_sdr_diag16_stop();
}
static K_TIMER_DEFINE(stopper, stopper_fn, NULL);

ZTEST(esp_sdr_diag16_api, test_stop_ends_the_run)
{
	struct esp_sdr_diag16_stats s = {0};
	int64_t t0 = k_uptime_get();

	k_timer_start(&stopper, K_MSEC(300), K_NO_WAIT);
	zassert_ok(run(20000000U, 0U, &s));
	zassert_true(k_uptime_get() - t0 < 2000, "the run outlived its stop");
	zassert_false(esp_sdr_diag16_active());
}

/* With no run in progress a stop does nothing: the next run is not cut short by it. */
ZTEST(esp_sdr_diag16_api, test_stop_with_no_run)
{
	struct esp_sdr_diag16_stats s = {0};

	zassert_false(esp_sdr_diag16_active());
	esp_sdr_diag16_stop();
	zassert_ok(run(20000000U, 1U, &s));
	zassert_true(s.consumed > 20000000U, "the run stopped early: %llu bytes", s.consumed);
}

/* A node lasts 51 us at 40 MS/s. */
static void slow_sink(void *user, uint8_t *node, uint64_t index)
{
	ARG_UNUSED(user);
	ARG_UNUSED(node);
	ARG_UNUSED(index);
	k_busy_wait(120);
}

ZTEST(esp_sdr_diag16_api, test_slow_sink_still_ends)
{
	struct esp_sdr_diag16_cfg cfg = {.rate_hz = 40000000U, .seconds = 1U, .sink = slow_sink};
	struct esp_sdr_diag16_stats s = {0};
	int64_t t0 = k_uptime_get();

	zassert_ok(esp_sdr_diag16_run(&cfg, &s));
	zassert_true(k_uptime_get() - t0 < 5000, "the run outlived its time");
	zassert_true(s.skips > 0U, "a sink this slow must fall behind");
	zassert_true(s.skipped_nodes > 0U);

	t0 = k_uptime_get();
	cfg.seconds = 0U;
	k_timer_start(&stopper, K_MSEC(300), K_NO_WAIT);
	zassert_ok(esp_sdr_diag16_run(&cfg, &s));
	zassert_true(k_uptime_get() - t0 < 5000, "the run outlived its stop");
}

/* The burst path shares the engine with the ring. */
ZTEST(esp_sdr_diag16_api, test_burst_after_a_run)
{
	struct esp_sdr_diag16_stats s = {0};
	struct esp_sdr_rx_burst b = {0};

	zassert_ok(run(20000000U, 1U, &s));
	zassert_ok(esp_sdr_rx_capture(ESP_SDR_RATE_80MSPS, 1024, &b));
	zassert_equal(b.count, 1024);
}

/* A fixed gain only: the servo on the ring leaves the DC under a step. */
ZTEST(esp_sdr_diag16_api, test_dc_correct)
{
	struct esp_sdr_diag16_cfg cfg = {
		.rate_hz = 20000000U,
		.seconds = 1U,
		.dc_correct = true,
		.sink = sink,
		.user = &c,
	};
	struct esp_sdr_diag16_stats s = {0};

	zassert_ok(esp_sdr_rx_set_gain(30));
	memset(&c, 0, sizeof(c));
	zassert_ok(esp_sdr_diag16_run(&cfg, &s));
	zassert_ok(s.dc_ret, "servo: %d", s.dc_ret);
	zassert_true(abs(s.dc_after[0]) < 256 && abs(s.dc_after[1]) < 256,
		     "DC still %d %d (1/256 step)", s.dc_after[0], s.dc_after[1]);
	zassert_ok(esp_sdr_rx_set_gain(ESP_SDR_RX_GAIN_AUTO));
}
