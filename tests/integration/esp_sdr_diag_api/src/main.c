/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Hardware integration test of the diagnostic-bus ring (esp_sdr_diag.h): the argument
 * contract, the ring's byte rate and integrity over a long run, and the decimated stream's
 * continuity and cost. Runs on the real radio; needs no antenna, peer board or host tool and
 * never transmits. The ring drives 8 pads as outputs while it runs: nothing may be wired to
 * them.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_diag.h>

/* A bound on the CPU's share: box16 and the CIC cost about 4.4 cycles per byte at 240 MHz
 * (budget 6.0, doc/hardware-quirks.md). */
#define MAX_SINK_CYCLES_X100 600U

static void *suite_setup(void)
{
	zassert_ok(esp_sdr_init(), "esp_sdr_init() failed");
	zassert_ok(esp_sdr_set_freq(2437), "set_freq(2437) failed");
	/* A fixed gain: the AGC idles high on the C5 and its DC offset eats the nibble window. */
	zassert_ok(esp_sdr_rx_set_gain(40), "set_gain(40) failed");
	return NULL;
}

ZTEST_SUITE(esp_sdr_diag_api, NULL, suite_setup, NULL, NULL, NULL);

ZTEST(esp_sdr_diag_api, test_run_rejects_bad_config)
{
	static const int8_t good[ESP_SDR_DIAG_PINS] = {0, 1, 3, 4, 5, 7, 10, 23};
	int8_t bad[ESP_SDR_DIAG_PINS];
	struct esp_sdr_diag_cfg cfg = {.seconds = 1};

	zassert_equal(esp_sdr_diag_run(NULL, NULL), -EINVAL);

	cfg.clk_div = 1;
	zassert_equal(esp_sdr_diag_run(&cfg, NULL), -EINVAL, "divider 1");
	cfg.clk_div = 0;
	cfg.lsb = ESP_SDR_DIAG_LSB_MAX + 1;
	zassert_equal(esp_sdr_diag_run(&cfg, NULL), -EINVAL, "lsb past the maximum");
	cfg.lsb = 0;
	cfg.decim = ESP_SDR_DIAG_DECIM_MIN - 1;
	zassert_equal(esp_sdr_diag_run(&cfg, NULL), -EINVAL, "decimation below the minimum");
	cfg.decim = 0;
	memcpy(bad, good, sizeof(bad));
	bad[3] = -1;
	cfg.pins = bad;
	zassert_equal(esp_sdr_diag_run(&cfg, NULL), -EINVAL, "negative pad");
	bad[3] = 29;
	zassert_equal(esp_sdr_diag_run(&cfg, NULL), -EINVAL, "pad that does not exist");
	zassert_false(esp_sdr_diag_active());
}

ZTEST(esp_sdr_diag_api, test_level)
{
	uint8_t lsb = 0xff;

	zassert_ok(esp_sdr_diag_level(&lsb));
	zassert_true(lsb <= ESP_SDR_DIAG_LSB_MAX, "window %u", lsb);
}

static struct {
	uint32_t seen[256];
	uint64_t blocks, next;
	uint32_t jumps;
	bool first;
	uint32_t x;
} raw;

static void raw_sink(void *user, const uint8_t *blk, size_t n, uint64_t index)
{
	ARG_UNUSED(user);
	if (raw.blocks != 0U && index != raw.next) {
		raw.jumps++;
	}
	raw.next = index + n;
	raw.blocks++;
	/* Touch every byte, as a consumer would. */
	for (size_t k = 0; k < n; k += 4) {
		raw.x ^= *(const uint32_t *)&blk[k];
	}
	/* Count the values in 4 KiB of the first block: a per-byte count of more would itself
	 * take longer than the ring's 1.4 ms of slack and be reported as skipped blocks. */
	if (raw.blocks == 1U) {
		for (size_t k = 0; k < 4096U; k++) {
			raw.seen[blk[k]]++;
		}
	}
}

/*
 * The ring's contract over 10 s: every byte arrives (40 MB/s for the time the run took), none
 * skipped, no overrun, the byte count follows the system timer (a byte lost per ring wrap would
 * be -17 ppm), and the consumer keeps up.
 */
ZTEST(esp_sdr_diag_api, test_ring_integrity)
{
	struct esp_sdr_diag_stats s;
	struct esp_sdr_diag_cfg cfg = {.seconds = 10, .lsb = 0, .sink = raw_sink};
	int64_t t0;
	unsigned int distinct = 0;
	uint64_t want;

	memset(&raw, 0, sizeof(raw));
	t0 = k_uptime_get();
	zassert_ok(esp_sdr_diag_run(&cfg, &s));
	t0 = k_uptime_get() - t0;

	want = (uint64_t)s.byte_rate * (uint64_t)t0 / MSEC_PER_SEC;
	zassert_equal(s.byte_rate, 40000000U);
	zassert_within(s.bytes, want, want / 50U, "%llu bytes in %lld ms", s.bytes, t0);
	zassert_equal(s.skips, 0U, "consumer fell behind");
	zassert_false(s.overrun);
	zassert_equal(raw.jumps, 0U, "index jumps");
	zassert_true(s.isrs > 0U);
	zassert_true(abs(s.ppm_x100) < 500, "ring against the timer: %d.%02d ppm", s.ppm_x100 / 100,
		     abs(s.ppm_x100) % 100);
	zassert_true(s.sink_cycles_x100 < MAX_SINK_CYCLES_X100, "consumer %u cycles/100 per byte",
		     s.sink_cycles_x100);
	zassert_false(esp_sdr_diag_active());

	/* The receiver's noise reaches the pads: the ring is not a constant. */
	for (unsigned int k = 0; k < ARRAY_SIZE(raw.seen); k++) {
		distinct += raw.seen[k] != 0U;
	}
	zassert_true(distinct > 8U, "only %u distinct byte values in 4 KiB of the ring", distinct);
}

static struct {
	uint64_t next, samples;
	uint32_t calls, jumps;
	int32_t peak;
} dec;

static void dec_sink(void *user, const int32_t *iq, size_t n, uint64_t index)
{
	ARG_UNUSED(user);
	if (dec.calls != 0U && index != dec.next) {
		dec.jumps++;
	}
	dec.next = index + n;
	dec.samples += n;
	dec.calls++;
	for (size_t k = 0; k < 2U * n; k++) {
		dec.peak = MAX(dec.peak, iq[k] < 0 ? -iq[k] : iq[k]);
	}
}

/* Every step of the sample-rate ladder: the output rate, a gapless index, the count. */
ZTEST(esp_sdr_diag_api, test_decimated_ladder)
{
	static const uint8_t ladder[] = {10, 20, 40, 80, 160};

	ARRAY_FOR_EACH(ladder, k) {
		struct esp_sdr_diag_stats s;
		struct esp_sdr_diag_cfg cfg = {.seconds = 2, .decim = ladder[k], .iq_sink = dec_sink};
		uint64_t want;

		memset(&dec, 0, sizeof(dec));
		zassert_ok(esp_sdr_diag_run(&cfg, &s), "R %u", ladder[k]);
		zassert_equal(s.iq_rate, 2500000U / ladder[k], "R %u: rate %u", ladder[k], s.iq_rate);
		zassert_equal(dec.jumps, 0U, "R %u: index jumps", ladder[k]);
		zassert_equal(s.skips, 0U, "R %u: skipped blocks", ladder[k]);
		zassert_false(s.overrun);
		/* The ring's bytes through box16 and the CIC, less the three samples it drops. */
		want = s.consumed / 16U / ladder[k];
		zassert_within(dec.samples, want, 8U, "R %u: %llu samples, want %llu", ladder[k],
			       dec.samples, want);
		zassert_true(dec.calls > 0U);
		zassert_true(s.sink_cycles_x100 < MAX_SINK_CYCLES_X100,
			     "R %u: consumer %u cycles/100 per byte", ladder[k], s.sink_cycles_x100);
	}
}

static void stop_sink(void *user, const uint8_t *blk, size_t n, uint64_t index)
{
	ARG_UNUSED(blk);
	ARG_UNUSED(n);
	if (index >= 10U * ESP_SDR_DIAG_BLOCK) {
		esp_sdr_diag_stop();
	}
}

ZTEST(esp_sdr_diag_api, test_stop)
{
	struct esp_sdr_diag_stats s;
	struct esp_sdr_diag_cfg cfg = {.seconds = 0, .sink = stop_sink};
	int64_t t0 = k_uptime_get();

	zassert_ok(esp_sdr_diag_run(&cfg, &s));
	/* 11 blocks are 2.3 ms of ring; setup and the level snapshot take a few ms more. */
	zassert_true(k_uptime_get() - t0 < 500, "took %lld ms", k_uptime_get() - t0);
	zassert_true(s.consumed >= 11U * ESP_SDR_DIAG_BLOCK);
	zassert_false(esp_sdr_diag_active());
	/* And it can run again. */
	cfg.seconds = 1;
	cfg.sink = NULL;
	zassert_ok(esp_sdr_diag_run(&cfg, &s));
}
