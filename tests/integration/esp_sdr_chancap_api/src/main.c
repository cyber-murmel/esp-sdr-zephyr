/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Hardware integration test of esp_sdr_chancap on the XIAO ESP32S3: the start contract and
 * the LO plan, the channelizer's timing on synthetic data for both engines, and a short live
 * run on whatever is on the air (needs no peer; never transmits).
 */
#include <errno.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_chancap.h>

#include "app_cpu.h"
#include "app_crash.h"
#include "app_usb.h"

/* The USB shell and DFU start before ztest's main(). */
static int usb_bringup(void)
{
	app_cpu_pin_system_threads(0);
	(void)app_usb_init();
	return 0;
}

/* What ended the previous boot, if it crashed: printed once the console is open. */
static void print_last_crash(void)
{
	struct app_crash c;

	if (app_crash_last(&c)) {
		printk("previous boot crashed: reason %u, thread %s, cpu %u, pc 0x%08x, a0 0x%08x, "
		       "exccause %u, excvaddr 0x%08x, at %u ms\n",
		       c.reason, c.thread, c.cpu, c.pc, c.a0, c.exccause, c.excvaddr, c.uptime_ms);
	}
}
SYS_INIT(usb_bringup, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* Before a DFU download: a running ring would keep its CPU from taking the flash stall. */
void app_usb_dfu_prepare(void)
{
	esp_sdr_chancap_stop();
}

static atomic_t captures, samples, chunks;

static void count_cb(void *user, const struct esp_sdr_chancap *cap)
{
	ARG_UNUSED(user);
	atomic_inc(&captures);
	atomic_add(&samples, (atomic_val_t)cap->n);
	if (cap->chunk) {
		atomic_inc(&chunks);
	}
	esp_sdr_chancap_release(cap);
}

static const struct esp_sdr_chancap_cfg ble = {
	.first_freq_hz = 2402000000U,
	.spacing_hz = 2000000U,
	.n_channels = 8,
	.channel_rate_hz = 2000000U,
	.wb_gate = true,
	.ch_gate = true,
	.wb_threshold_db = 8,
	.ch_threshold_db = 10,
	.lookback_us = 64,
	.hang_us = 250,
	.max_capture_us = 2400,
	.cb = count_cb,
};

static const struct esp_sdr_chancap_cfg wpan = {
	.first_freq_hz = 2405000000U,
	.spacing_hz = 5000000U,
	.n_channels = 3,
	.channel_rate_hz = 4000000U,
	.wb_gate = true,
	.ch_gate = true,
	.wb_threshold_db = 8,
	.ch_threshold_db = 10,
	.lookback_us = 64,
	.hang_us = 250,
	.max_capture_us = 4500,
	.cb = count_cb,
};

static void *suite_setup(void)
{
	/* The console is USB CDC-ACM: give the host time to open it. */
	k_sleep(K_SECONDS(10));
	print_last_crash();
	zassert_ok(esp_sdr_init(), "esp_sdr_init() failed");
	return NULL;
}

ZTEST_SUITE(esp_sdr_chancap_api, NULL, suite_setup, NULL, NULL, NULL);

ZTEST(esp_sdr_chancap_api, test_start_rejects)
{
	struct esp_sdr_chancap_cfg c = ble;

	c.cb = NULL;
	zassert_equal(esp_sdr_chancap_start(&c), -EINVAL, "no callback");
	c = ble;
	c.n_channels = 0;
	zassert_equal(esp_sdr_chancap_start(&c), -EINVAL, "no channels");
	c = ble;
	c.n_channels = 9;
	zassert_equal(esp_sdr_chancap_start(&c), -EINVAL, "9 x 2 MHz do not fit 16 MHz");
	c = ble;
	c.channel_rate_hz = 3000000U;
	zassert_equal(esp_sdr_chancap_start(&c), -EINVAL, "rate");
	c = wpan;
	c.channel_rate_hz = 3000000U;
	zassert_equal(esp_sdr_chancap_start(&c), -EINVAL, "ddc rate");
	zassert_false(esp_sdr_chancap_active(), "active after a refused start");
}

ZTEST(esp_sdr_chancap_api, test_plan)
{
	struct esp_sdr_chancap_plan p;
	struct esp_sdr_chancap_bench_result r;

	/* The bench plans without tuning: BLE 2402 to 2416 MHz, LO on the edge between 2408 and 2410. */
	zassert_ok(esp_sdr_chancap_bench(&ble, 512, &r));
	esp_sdr_chancap_get_plan(&p);
	zassert_true(p.pfb, "pfb");
	zassert_equal(p.m, 8, "m %u", p.m);
	zassert_equal(p.d, 8, "d %u", p.d);
	zassert_equal(p.lo_hz, 2409000000ULL, "LO %llu", (unsigned long long)p.lo_hz);
	/* 802.15.4 channels 11 to 13: span centre 2410 MHz, LO 1.5 MHz below it. */
	zassert_ok(esp_sdr_chancap_bench(&wpan, 512, &r));
	esp_sdr_chancap_get_plan(&p);
	zassert_false(p.pfb, "ddc");
	zassert_equal(p.m, 3, "m %u", p.m);
	zassert_equal(p.d, 4, "d %u", p.d);
	zassert_equal(p.lo_hz, 2408500000ULL, "LO %llu", (unsigned long long)p.lo_hz);
}

ZTEST(esp_sdr_chancap_api, test_bench)
{
	struct esp_sdr_chancap_bench_result r;

	zassert_ok(esp_sdr_chancap_bench(&ble, 8192, &r));
	printk("bench pfb 8 x 2 MHz at 2 MS/s%s: ring CPU %u.%02u, channelizer CPU %u.%02u cycles "
	       "per pair (15 available each); per call: sums %u (a frame), post %u (8 frames), "
	       "power %u (32 frames); kernels %s\n",
	       r.fast ? " (both CPUs)" : "", r.ring_x100 / 100U, r.ring_x100 % 100U,
	       r.block_x100 / 100U, r.block_x100 % 100U, r.sums_cyc, r.post_cyc, r.power_cyc,
	       r.kernels_match ? "match" : "MISMATCH");
	printk("bench ring CPU: unpack %u.%02u, sink %u.%02u (alone %u.%02u), history %u.%02u; "
	       "channelizer CPU: kernels %u.%02u, channels %u.%02u cycles per pair\n",
	       r.ring_unpack_x100 / 100U, r.ring_unpack_x100 % 100U, r.ring_sink_x100 / 100U,
	       r.ring_sink_x100 % 100U, r.sink_x100 / 100U, r.sink_x100 % 100U,
	       r.ring_hist_x100 / 100U, r.ring_hist_x100 % 100U,
	       r.kern_x100 / 100U, r.kern_x100 % 100U, r.chan_x100 / 100U, r.chan_x100 % 100U);
	zassert_true(r.fast, "the BLE grid runs the fast path");
	zassert_true(r.kernels_match, "pfb kernels: %u mismatches", r.mismatches);
	zassert_ok(esp_sdr_chancap_bench(&wpan, 8192, &r));
	printk("bench ddc 3 x 5 MHz at 4 MS/s: ring CPU %u.%02u, a channel %u.%02u, channelizer CPU "
	       "%u.%02u cycles per pair; kernels %s\n",
	       r.ring_x100 / 100U, r.ring_x100 % 100U, r.ddc_x100 / 100U, r.ddc_x100 % 100U,
	       r.block_x100 / 100U, r.block_x100 % 100U, r.kernels_match ? "match" : "MISMATCH");
	zassert_true(r.kernels_match, "ddc kernels: %u mismatches", r.mismatches);
}

#if defined(CONFIG_THREAD_RUNTIME_STATS)
/*
 * Per-thread CPU time over a live run: who else takes the channelizer's CPU. Off by default:
 * the option's kernel code does not fit below bank 0 in this image.
 */
#define TU_MAX 32
static struct {
	const struct k_thread *t;
	uint64_t c;
} tu[TU_MAX];
static unsigned int tu_n;

static void tu_snap(const struct k_thread *t, void *user)
{
	k_thread_runtime_stats_t s;

	ARG_UNUSED(user);
	if (tu_n < TU_MAX && k_thread_runtime_stats_get((k_tid_t)t, &s) == 0) {
		tu[tu_n].t = t;
		tu[tu_n].c = s.execution_cycles;
		tu_n++;
	}
}

static void tu_print(const struct k_thread *t, void *user)
{
	uint64_t elapsed = *(const uint64_t *)user;
	k_thread_runtime_stats_t s;

	for (unsigned int i = 0; i < tu_n; i++) {
		if (tu[i].t == t && k_thread_runtime_stats_get((k_tid_t)t, &s) == 0) {
			uint32_t pm = (uint32_t)((s.execution_cycles - tu[i].c) * 1000U / elapsed);
			const char *n = k_thread_name_get((k_tid_t)t);

			if (pm >= 5U) {
				printk("  thread %s: %u.%u %% of a CPU\n", n != NULL ? n : "?",
				       pm / 10U, pm % 10U);
			}
		}
	}
}
#endif

static void live(const struct esp_sdr_chancap_cfg *cfg, const char *name, unsigned int secs)
{
	struct esp_sdr_chancap_stats st;
	uint64_t t0, elapsed;

	atomic_set(&captures, 0);
	atomic_set(&samples, 0);
	atomic_set(&chunks, 0);
#if defined(CONFIG_THREAD_RUNTIME_STATS)
	tu_n = 0;
	k_thread_foreach(tu_snap, NULL);
#endif
	t0 = k_cycle_get_64();
	zassert_ok(esp_sdr_chancap_start(cfg), "start %s", name);
	zassert_true(esp_sdr_chancap_active(), "active");
	zassert_equal(esp_sdr_chancap_start(cfg), -EBUSY, "second start");
	k_sleep(K_SECONDS(secs));
	esp_sdr_chancap_get_stats(&st);
	elapsed = k_cycle_get_64() - t0;
	esp_sdr_chancap_stop();

	zassert_false(esp_sdr_chancap_active(), "active after stop");
	/* Lines under the log buffer's 256 bytes, or they are dropped. */
	printk("%s %u s: %llu pairs in, %llu channelized (%u %%), %ld captures (%ld chunks, "
	       "%ld samples), %u dropped\n",
	       name, secs, (unsigned long long)st.pairs, (unsigned long long)st.pairs_run,
	       st.pairs != 0U ? (unsigned int)(st.pairs_run * 100U / st.pairs) : 0U,
	       atomic_get(&captures), atomic_get(&chunks), atomic_get(&samples), st.dropped);
	printk("  %u overruns, %u gaps, %u.%02u cycles per pair, ring status %d, %u abandoned "
	       "units, %u buffers at most, longest block %u cycles, lag %u pairs at most\n",
	       st.overruns, st.gaps, st.cycles_x100 / 100U, st.cycles_x100 % 100U,
	       st.ring.status, st.ring.abandoned, st.bufs_peak, st.block_max, st.lag_max);
	printk("  longest: sub-block run %u, capture open %u, capture finish %u cycles\n",
	       st.run_max, st.open_max, st.finish_max);

#if defined(CONFIG_THREAD_RUNTIME_STATS)
	k_thread_foreach(tu_print, &elapsed);
#else
	ARG_UNUSED(elapsed);
#endif
	zassert_true(st.pairs > (uint64_t)secs * 15000000U, "ring ran: %llu pairs",
		     (unsigned long long)st.pairs);
	zassert_equal(st.ring.status, ESP_SDR_RING_OK, "ring status %d detail 0x%08x",
		      st.ring.status, st.ring.detail);
}

ZTEST(esp_sdr_chancap_api, test_live_gated)
{
	live(&ble, "ble gated", 3);
	live(&wpan, "wpan gated", 3);
}

ZTEST(esp_sdr_chancap_api, test_live_continuous)
{
	struct esp_sdr_chancap_cfg c = ble;

	/* Both gates off: every channel streams in chunks, whether or not the CPU keeps up. */
	c.wb_gate = false;
	c.ch_gate = false;
	c.max_capture_us = 500;
	live(&c, "ble continuous", 2);
	zassert_true(atomic_get(&chunks) > 0, "no chunks");
}

ZTEST(esp_sdr_chancap_api, test_gate_switches)
{
	zassert_equal(esp_sdr_chancap_set_gates(true, true), -EINVAL, "not running");
	zassert_ok(esp_sdr_chancap_start(&ble));
	zassert_ok(esp_sdr_chancap_set_gates(false, true));
	k_sleep(K_MSEC(300));
	zassert_ok(esp_sdr_chancap_set_thresholds(12, 14));
	k_sleep(K_MSEC(300));
	zassert_ok(esp_sdr_chancap_set_gates(true, false));
	k_sleep(K_MSEC(300));
	esp_sdr_chancap_stop();
}
