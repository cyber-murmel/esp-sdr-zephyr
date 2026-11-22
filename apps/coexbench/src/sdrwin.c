/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * "coex sdr": the SDR in the radio arbiter's windows (claim, run to the deadline, release) while
 * Wi-Fi sleeps, and "coex arb": the arbiter's counters.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_arb.h>
#include <esp_sdr/esp_sdr_place.h>
#include <esp_sdr/esp_sdr_ring.h>
#include <esp_sdr/esp_sdr_rx.h>

#include "coex.h"

#define CAP_RATE    ESP_SDR_RATE_40MSPS
#define CAP_SAMPLES 4096U

/* What a run counted (written by the worker thread, read after it signals done). */
struct run_result {
	uint32_t claim_ebusy, claim_eagain, claim_other, claim_canceled;
	uint32_t windows;
	/* Ring runs: ended by the window, by a failure; index gaps seen by the sink. */
	uint32_t ring_runs, ring_window_ends, ring_fail, gaps;
	uint64_t samples;
	/* Bursts taken. */
	uint32_t bursts, burst_err;
	uint32_t late_max;
	int ring_status_last;
};

static struct run_result res;
static struct k_sem done_sem;
static struct k_timer end_timer;
static volatile bool run_over;
static bool use_ring;
static uint32_t run_ms;
#if defined(CONFIG_ESP_SDR_RING)
/* The sink's own counters: it runs with interrupts masked. */
static uint64_t sink_samples, sink_next;
static uint32_t sink_gaps;
static bool sink_first;
#endif

static struct k_thread sdr_thread;
ESP_SDR_STACK_DEFINE(sdr_stack, 3072);

#if defined(CONFIG_ESP_SDR_RING)
ESP_SDR_CODE(PACED_SAMPLE, ring_sink)
static void ring_sink(void *user, const int16_t *i, const int16_t *q, size_t n, uint64_t index)
{
	ARG_UNUSED(user);
	ARG_UNUSED(i);
	ARG_UNUSED(q);
	if (!sink_first && index != sink_next) {
		sink_gaps++;
	}
	sink_first = false;
	sink_next = index + n;
	sink_samples += n;
}
#endif

static void end_expired(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	run_over = true;
#if defined(CONFIG_ESP_SDR_RING)
	esp_sdr_ring_stop();
#endif
}

static void window_ring(void)
{
#if defined(CONFIG_ESP_SDR_RING)
	struct esp_sdr_ring_cfg cfg = {
		.decim = ESP_SDR_RING_DECIM_MIN * 2U,
		.sink = ring_sink,
	};
	struct esp_sdr_ring_stats st;
	int ret;

	sink_first = true;
	sink_samples = 0U;
	sink_gaps = 0U;
	ret = esp_sdr_ring_run(&cfg, &st);
	res.ring_runs++;
	res.samples += sink_samples;
	res.gaps += sink_gaps;
	res.late_max = MAX(res.late_max, st.late_max);
	res.ring_status_last = (int)st.status;
	if (st.status == ESP_SDR_RING_WINDOW) {
		res.ring_window_ends++;
	} else if (ret != 0 || st.status != ESP_SDR_RING_OK) {
		res.ring_fail++;
	}
#endif
}

static void window_burst(void)
{
	while (!esp_sdr_arb_window_over() && !run_over) {
		struct esp_sdr_rx_burst b;

		if (esp_sdr_rx_capture(CAP_RATE, CAP_SAMPLES, &b) == 0) {
			res.bursts++;
			res.samples += b.count;
		} else {
			res.burst_err++;
		}
	}
}

static void sdr_worker(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (!run_over) {
		int64_t deadline;
		int ret = esp_sdr_arb_claim(K_MSEC(MIN(2000U, run_ms)), &deadline);

		if (ret != 0) {
			switch (ret) {
			case -EBUSY:
				res.claim_ebusy++;
				break;
			case -EAGAIN:
				res.claim_eagain++;
				break;
			case -ECANCELED:
				res.claim_canceled++;
				break;
			default:
				res.claim_other++;
				break;
			}
			if (ret != -EBUSY && ret != -EAGAIN) {
				k_msleep(1);
			}
			continue;
		}
		res.windows++;
		if (use_ring) {
			window_ring();
		} else {
			window_burst();
		}
		esp_sdr_arb_release();
	}
	k_sem_give(&done_sem);
}

int coex_cmd_sdr(const struct shell *sh, size_t argc, char **argv)
{
	int secs = atoi(argv[1]);
	uint32_t mhz = 0U;
	struct esp_sdr_arb_stats arb;
	int64_t t0;
	int ret;

	use_ring = IS_ENABLED(CONFIG_ESP_SDR_RING);
	if (argc > 2) {
		if (strcmp(argv[2], "burst") == 0) {
			use_ring = false;
		} else if (strcmp(argv[2], "ring") != 0) {
			return -EINVAL;
		} else if (!IS_ENABLED(CONFIG_ESP_SDR_RING)) {
			shell_error(sh, "built without the ring: burst only");
			return -ENOTSUP;
		}
	}
	if (secs < 1) {
		return -EINVAL;
	}
	if (argc > 3) {
		mhz = (uint32_t)atoi(argv[3]);
	} else {
		coex_channel_refresh();
		mhz = coex_chan_mhz(phylog_channel() != 0U ? phylog_channel() : 6U);
	}
	/* The SDR owns the front end in its windows: forced, at the frequency asked. */
	(void)esp_sdr_rx_set_passive(false);
	ret = esp_sdr_set_freq(mhz);
	if (ret != 0) {
		shell_error(sh, "set_freq %u: %d", mhz, ret);
		(void)esp_sdr_rx_set_passive(true);
		return ret;
	}
	memset(&res, 0, sizeof(res));
	esp_sdr_arb_reset_stats();
	run_over = false;
	run_ms = (uint32_t)secs * 1000U;
	k_sem_init(&done_sem, 0, 1);
	k_timer_init(&end_timer, end_expired, NULL);

	k_thread_create(&sdr_thread, sdr_stack, K_KERNEL_STACK_SIZEOF(sdr_stack), sdr_worker, NULL,
			NULL, NULL, K_PRIO_COOP(2), K_FP_REGS, K_FOREVER);
	k_thread_name_set(&sdr_thread, "coex_sdr");
#if defined(CONFIG_SCHED_CPU_MASK)
	if (arch_num_cpus() > 1) {
		/* The ring masks its CPU: the other one keeps Wi-Fi's thread and interrupt. */
		k_thread_cpu_pin(&sdr_thread, 1);
	}
#endif
	t0 = k_uptime_get();
	k_timer_start(&end_timer, K_SECONDS(secs), K_NO_WAIT);
	k_thread_start(&sdr_thread);
	/* The worker ends after its last claim wait, at most 2 s past the time. */
	ret = k_sem_take(&done_sem, K_SECONDS(secs + 10));
	k_timer_stop(&end_timer);
	if (ret != 0) {
		shell_error(sh, "SDR worker did not finish");
		return -ETIMEDOUT;
	}
	(void)k_thread_join(&sdr_thread, K_SECONDS(2));
	t0 = k_uptime_get() - t0;
	(void)esp_sdr_rx_set_passive(true);

	esp_sdr_arb_get_stats(&arb);
	shell_print(sh, "%s %u MHz, %lld ms", use_ring ? "ring" : "burst", mhz, t0);
	shell_print(sh, "windows %u (claims failed: busy %u again %u other %u canceled %u)",
		    res.windows, res.claim_ebusy, res.claim_eagain, res.claim_other,
		    res.claim_canceled);
	shell_print(sh, "SDR time %llu us, duty %u.%02u %%, hold max %u us, per window %u us",
		    arb.sdr_us, (uint32_t)(arb.sdr_us * 100U / (uint64_t)(t0 * 1000)),
		    (uint32_t)(arb.sdr_us * 10000U / (uint64_t)(t0 * 1000)) % 100U,
		    arb.hold_max_us, arb.windows != 0U ? (uint32_t)(arb.sdr_us / arb.windows) : 0U);
	shell_print(sh, "samples %llu (%u bursts, %u burst errors)", res.samples, res.bursts,
		    res.burst_err);
	if (use_ring) {
		shell_print(sh, "ring runs %u: window ends %u, failures %u (last status %d), "
				"late max %u pairs, sink gaps %u",
			    res.ring_runs, res.ring_window_ends, res.ring_fail,
			    res.ring_status_last, res.late_max, res.gaps);
	}
	shell_print(sh, "preemptions %u (timeouts %u, wait max %u us), deadline misses %u",
		    arb.preempts, arb.preempt_timeouts, arb.preempt_wait_max_us,
		    arb.deadline_misses);
	return 0;
}

int coex_cmd_arb(const struct shell *sh, size_t argc, char **argv)
{
	struct esp_sdr_arb_stats s;

	esp_sdr_arb_get_stats(&s);
	shell_print(sh, "Wi-Fi: awake %d, enables %u, disables %u, learned period %u us, "
			"interval error %d..%d us",
		    esp_sdr_arb_wifi_awake(), s.wifi_on, s.wifi_off, s.period_us,
		    s.jitter_min_us == INT32_MAX ? 0 : s.jitter_min_us,
		    s.jitter_max_us == INT32_MIN ? 0 : s.jitter_max_us);
	shell_print(sh, "SDR: holds %d, windows %u, total %llu us, longest %u us, claim wait max %u us, "
			"own PHY power-up %llu us",
		    esp_sdr_arb_holds(), s.windows, s.sdr_us, s.hold_max_us, s.claim_wait_max_us,
		    s.powerup_us);
	shell_print(sh, "preemptions %u, timeouts %u, wait max %u us, deadline misses %u", s.preempts,
		    s.preempt_timeouts, s.preempt_wait_max_us, s.deadline_misses);
	if (argc > 1 && strcmp(argv[1], "clear") == 0) {
		esp_sdr_arb_reset_stats();
		shell_print(sh, "counters cleared");
	}
	return 0;
}
