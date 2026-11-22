/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The coex app: Wi-Fi (wifitest.c's "wifi" commands and the host's "net" shell) beside the SDR
 * under the radio arbiter, on one S3 radio (doc/radio/coexistence.md). Ported from apps/coexbench
 * (sdrwin.c, main.c), whose ring did not fit there:
 *
 *   coex sdr <seconds> [ring] [freq_mhz] [bg [delay_s]]  the SDR in the windows of Wi-Fi's sleep; bg returns
 *                                               at once, "coex sdr result" reports (the ring
 *                                               only: dump bank 2 holds the modules in this boot)
 *   coex arb [clear]                            the arbiter's counters
 *   coex ps none|min|max                        Wi-Fi's power save
 *   coex listen <n>                             listen interval in beacons (next association)
 *   coex chan                                   Wi-Fi's primary channel
 *
 * Join an access point with "wifi connect <ssid> <psk>" (typed at runtime, never stored).
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <esp_wifi.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_arb.h>
#include <esp_sdr/esp_sdr_place.h>
#include <esp_sdr/esp_sdr_ring.h>
#include <esp_sdr/esp_sdr_rx.h>

#define CAP_RATE    ESP_SDR_RATE_40MSPS
#define CAP_SAMPLES 4096U

/* For a debugger after a hang: 1 before a claim, 2 claimed, 3 in the ring, 4 ring returned, 5 released. */
volatile uint32_t coex_step;

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
/* The worker sleeps this long before its first claim ("coex sdr ... bg <delay>"). */
static uint32_t start_delay_s;
#if defined(CONFIG_ESP_SDR_RING)
/* The sink's own counters: it runs with interrupts masked. */
static uint64_t sink_samples, sink_next;
static uint32_t sink_gaps;
static bool sink_first;
#endif

/* Wi-Fi's primary channel (0 unknown). */
static uint8_t wifi_chan;

static void channel_refresh(void)
{
	uint8_t primary = 0;
	wifi_second_chan_t second;

	if (esp_wifi_get_channel(&primary, &second) == ESP_OK) {
		wifi_chan = primary;
		/* From this thread: the arbiter makes no driver call on the SDR's (esp_sdr_arb.h). */
		esp_sdr_arb_wifi_set_channel(primary, (int)second);
	}
}

static uint32_t chan_mhz(uint8_t chan)
{
	return chan == 14U ? 2484U : 2407U + 5U * chan;
}

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
		/* Banks 0 and 1 only: bank 2 is the module heap in this boot (BANK2). */
		.unit_pairs = 2048U,
		.spare_bank_app = true,
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

	if (start_delay_s != 0U) {
		k_sleep(K_SECONDS(start_delay_s));
	}
	while (!run_over) {
		int64_t deadline;
		int ret;

		coex_step = 1U;
		ret = esp_sdr_arb_claim(K_MSEC(MIN(2000U, run_ms)), &deadline);

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
		coex_step = 2U;
		if (use_ring) {
			coex_step = 3U;
			window_ring();
			coex_step = 4U;
		} else {
			window_burst();
		}
		esp_sdr_arb_release();
		coex_step = 5U;
	}
	k_sem_give(&done_sem);
}

/* The run that "coex sdr ... bg" started, until "coex sdr result" has reported it. */
static bool running;
static uint32_t run_mhz;
static int64_t run_t0;

static int sdr_report(const struct shell *sh, bool wait)
{
	struct esp_sdr_arb_stats arb;
	int64_t t0;

	if (!running) {
		shell_error(sh, "no run");
		return -EINVAL;
	}
	/* The worker ends after its last claim wait, at most 2 s past the time. */
	if (k_sem_take(&done_sem, wait ? K_MSEC(run_ms + 10000U) : K_NO_WAIT) != 0) {
		if (wait) {
			shell_error(sh, "SDR worker did not finish");
			return -ETIMEDOUT;
		}
		shell_print(sh, "running, %lld of %u ms", k_uptime_get() - run_t0, run_ms);
		return 0;
	}
	running = false;
	k_timer_stop(&end_timer);
	(void)k_thread_join(&sdr_thread, K_SECONDS(2));
	t0 = k_uptime_get() - run_t0;
	(void)esp_sdr_rx_set_passive(true);

	esp_sdr_arb_get_stats(&arb);
	shell_print(sh, "ring %u MHz, %lld ms", run_mhz, t0);
	shell_print(sh, "windows %u (claims failed: busy %u again %u other %u canceled %u)",
		    res.windows, res.claim_ebusy, res.claim_eagain, res.claim_other,
		    res.claim_canceled);
	shell_print(sh, "SDR time %llu us, duty %u.%02u %%, hold max %u us, per window %u us",
		    arb.sdr_us, (uint32_t)(arb.sdr_us * 100U / (uint64_t)(t0 * 1000)),
		    (uint32_t)(arb.sdr_us * 10000U / (uint64_t)(t0 * 1000)) % 100U,
		    arb.hold_max_us, arb.windows != 0U ? (uint32_t)(arb.sdr_us / arb.windows) : 0U);
	shell_print(sh, "samples %llu", res.samples);
	shell_print(sh, "ring runs %u: window ends %u, failures %u (last status %d), "
			"late max %u pairs, sink gaps %u",
		    res.ring_runs, res.ring_window_ends, res.ring_fail, res.ring_status_last,
		    res.late_max, res.gaps);
	shell_print(sh, "preemptions %u (timeouts %u, wait max %u us), deadline misses %u",
		    arb.preempts, arb.preempt_timeouts, arb.preempt_wait_max_us,
		    arb.deadline_misses);
	return 0;
}

/* coex sdr <seconds> [ring] [freq_mhz] [bg [delay_s]]; coex sdr result */
static int cmd_sdr(const struct shell *sh, size_t argc, char **argv)
{
	bool bg = false;
	uint32_t mhz = 0U;
	int secs, ret;

	start_delay_s = 0U;
	for (size_t i = 2; i < argc; i++) {
		if (strcmp(argv[i], "bg") == 0) {
			bg = true;
			if (i + 1 < argc) {
				start_delay_s = (uint32_t)atoi(argv[i + 1]);
			}
			argc = i;
			break;
		}
	}

	if (strcmp(argv[1], "result") == 0) {
		return sdr_report(sh, false);
	}
	if (running) {
		shell_error(sh, "a run is going: \"coex sdr result\"");
		return -EBUSY;
	}
	secs = atoi(argv[1]);
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
	if (!use_ring) {
		shell_error(sh, "bursts need dump bank 2, which holds the modules here: ring only");
		return -ENOTSUP;
	}
	if (secs < 1) {
		return -EINVAL;
	}
	/* The arbiter's channel, from this thread (before the worker exists). */
	channel_refresh();
	if (argc > 3) {
		mhz = (uint32_t)atoi(argv[3]);
	} else {
		mhz = chan_mhz(wifi_chan != 0U ? wifi_chan : 6U);
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
	run_mhz = mhz;
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
	running = true;
	run_t0 = k_uptime_get();
	k_timer_start(&end_timer, K_SECONDS(secs + start_delay_s), K_NO_WAIT);
	k_thread_start(&sdr_thread);
	if (bg) {
		shell_print(sh, "started: %d s at %u MHz, \"coex sdr result\" or \"coex arb\" later", secs,
			    mhz);
		return 0;
	}
	return sdr_report(sh, true);
}

static int cmd_arb(const struct shell *sh, size_t argc, char **argv)
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

static int cmd_ps(const struct shell *sh, size_t argc, char **argv)
{
	wifi_ps_type_t type;
	esp_err_t err;

	if (strcmp(argv[1], "none") == 0) {
		type = WIFI_PS_NONE;
	} else if (strcmp(argv[1], "min") == 0) {
		type = WIFI_PS_MIN_MODEM;
	} else if (strcmp(argv[1], "max") == 0) {
		type = WIFI_PS_MAX_MODEM;
	} else {
		return -EINVAL;
	}
	err = esp_wifi_set_ps(type);
	if (err == ESP_OK) {
		err = esp_wifi_get_ps(&type);
	}
	shell_print(sh, "ps now %d (0 none, 1 min, 2 max): %d", (int)type, err);
	return err == ESP_OK ? 0 : -EIO;
}

/* Applies at the next association, and only with MAX modem sleep. */
static int cmd_listen(const struct shell *sh, size_t argc, char **argv)
{
	wifi_config_t cfg;
	esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &cfg);

	if (err == ESP_OK) {
		cfg.sta.listen_interval = (uint16_t)atoi(argv[1]);
		err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
	}
	shell_print(sh, "listen interval %s: %d", argv[1], err);
	return err == ESP_OK ? 0 : -EIO;
}

static int cmd_chan(const struct shell *sh, size_t argc, char **argv)
{
	channel_refresh();
	shell_print(sh, "channel %u (%u MHz)", wifi_chan, chan_mhz(wifi_chan));
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	coex_cmds,
	SHELL_CMD_ARG(ps, NULL, "none|min|max: esp_wifi_set_ps()", cmd_ps, 2, 0),
	SHELL_CMD_ARG(listen, NULL, "<n> listen interval in beacons (next association)", cmd_listen,
		      2, 0),
	SHELL_CMD_ARG(chan, NULL, "Wi-Fi primary channel", cmd_chan, 1, 0),
	SHELL_CMD_ARG(sdr, NULL,
		      "<seconds> [ring] [freq_mhz] [bg [delay_s]] | result: SDR windows in Wi-Fi's sleep (arbiter)",
		      cmd_sdr, 2, 5),
	SHELL_CMD_ARG(arb, NULL, "[clear] radio arbiter counters", cmd_arb, 1, 1),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(coex, &coex_cmds, "Wi-Fi and the SDR on one radio", NULL);

/* The AGC and the PHY's filter codes are Wi-Fi's until a window asks (esp_sdr_rx_set_passive()). */
extern void (*wifitest_stop_hook)(void);
void coex_stop(void);

static int coex_init(void)
{
	wifitest_stop_hook = coex_stop;
	(void)esp_sdr_rx_set_passive(true);
	return 0;
}
SYS_INIT(coex_init, APPLICATION, 0);

/* A run in the background ends before the app unloads (multiapp_app_stop() in wifitest.c). */
void coex_stop(void)
{
	if (running) {
		end_expired(NULL);
		k_timer_stop(&end_timer);
		(void)k_thread_join(&sdr_thread, K_SECONDS(5));
		running = false;
	}
	(void)esp_sdr_rx_set_passive(true);
}
