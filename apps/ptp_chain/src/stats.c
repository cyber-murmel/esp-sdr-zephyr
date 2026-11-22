/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The stats thread (CPU 0, lowest priority): drains the exchange ring, keeps the log of the last
 * CONFIG_APP_CHAIN_LOG exchanges, fits a line to the offsets of each 1 s window and, on the uke
 * with servo on, runs the PI loop (chain_servo in chain_math.c) every period: the rate of the PTP
 * clock follows the seme, steps only for the initial lock and a large offset.
 */
#include <math.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/ptp_clock.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/net/ptp_time.h>
#include <zephyr/sys/atomic.h>

#include "chain.h"

#define LOG_SLOTS    CONFIG_APP_CHAIN_LOG
#define WINDOW_MS    1000
#define DRAIN_MS     5
/* A step this large or larger sets the clock; a smaller one adjusts it */
#define STEP_SET_NS  1000000LL
/* An exchange with a mean one-way delay outside 0 to 100 us is a mix of stamps of two clock eras */
#define DELAY_MAX_PS 100000000LL
/*
 * Periods in a row that each stepped the clock: a step leaves the rate alone, so a loop that
 * steps every period has lost the rate (seen once: rate stuck at +19 ppm, a step every 50 ms).
 * Then the servo starts over from the nominal rate.
 */
#define SERVO_STEP_RUN_MAX 5

BUILD_ASSERT((LOG_SLOTS & (LOG_SLOTS - 1)) == 0, "CONFIG_APP_CHAIN_LOG: a power of two");

atomic_t chain_stat_on;
atomic_t chain_servo_on;

static struct chain_xchg xlog[LOG_SLOTS];
static atomic_t log_n;
static atomic_t servo_steps;

/* Servo settings (the shell writes, the thread reads when servo_reconf is set) */
static atomic_t servo_period_ms = 50;
static atomic_t servo_bw_mhz = 2000;
static atomic_t servo_clamp_ns = 500;
static atomic_t servo_reconf;

static K_THREAD_STACK_DEFINE(stack, CONFIG_APP_CHAIN_THREAD_STACK);
static struct k_thread thread;

uint32_t chain_log_count(void)
{
	return (uint32_t)atomic_get(&log_n);
}

/* Entry idx (counted from boot) if it is still in the log. */
bool chain_log_get(uint32_t idx, struct chain_xchg *x)
{
	uint32_t n = chain_log_count();

	if (n - idx > LOG_SLOTS) {
		return false;
	}
	*x = xlog[idx & (LOG_SLOTS - 1U)];
	/* the thread may have lapped us while copying */
	return chain_log_count() - idx <= LOG_SLOTS;
}

uint32_t chain_servo_steps(void)
{
	return (uint32_t)atomic_get(&servo_steps);
}

static void log_add(const struct chain_xchg *x)
{
	uint32_t n = chain_log_count();

	xlog[n & (LOG_SLOTS - 1U)] = *x;
	atomic_set(&log_n, (atomic_val_t)(n + 1U));
}

struct window {
	struct chain_fit fit;
	uint32_t base_sec;
	int64_t delay_sum_ps;
};

static void window_reset(struct window *w)
{
	chain_fit_reset(&w->fit);
	w->delay_sum_ps = 0;
	w->base_sec = 0;
}

static void window_add(struct window *w, const struct chain_xchg *x)
{
	int64_t t;

	if (w->fit.n == 0) {
		w->base_sec = x->base_sec;
	}
	t = (int64_t)(int32_t)(x->base_sec - w->base_sec) * CHAIN_PS_PER_SEC + x->t_ps[0];
	chain_fit_add(&w->fit, t, chain_xchg_offset_ps(x));
	w->delay_sum_ps += chain_xchg_delay_ps(x);
}

/*
 * Exchanges whose t1 (local tx stamp) is from before a servo step are in the old time base: the
 * late-completing pair of the last burst. After a forward step they are told apart by t1 < the
 * clock after the step; after a backward step old and new stamps overlap, so every exchange of
 * the next two burst intervals is dropped.
 */
static struct {
	bool on;
	bool backward;
	uint32_t sec;
	int64_t ps;
	int64_t until_ms;
} gate;

static bool gate_blocks(const struct chain_xchg *x)
{
	if (!gate.on) {
		return false;
	}
	if (gate.backward) {
		if (k_uptime_get() < gate.until_ms) {
			return true;
		}
	} else if (x->base_sec < gate.sec || (x->base_sec == gate.sec && x->t_ps[0] < gate.ps)) {
		return true;
	}
	gate.on = false;
	return false;
}

/* Step the local clock by step_ps (peer minus local): set above 1 ms, else adjust. */
static void step_clock(int64_t step_ps)
{
	const struct device *clk = DEVICE_DT_GET(DT_NODELABEL(ptp_clock));
	struct chain_xchg mark = {.seq = CHAIN_LOG_STEP};
	int64_t step_ns = step_ps / 1000;
	struct net_ptp_time t;
	int ret = 0;

	if (llabs(step_ns) >= STEP_SET_NS) {
		/* the boot offset is seconds: adjust takes an int and the MAC less than a second */
		int64_t total_ns;

		ret = ptp_clock_get(clk, &t);
		if (ret == 0) {
			total_ns = (int64_t)t.second * NSEC_PER_SEC + t.nanosecond + step_ns;
			if (total_ns < 0) {
				total_ns = 0;
			}
			t.second = (uint64_t)(total_ns / NSEC_PER_SEC);
			t.nanosecond = (uint32_t)(total_ns % NSEC_PER_SEC);
			ret = ptp_clock_set(clk, &t);
		}
	} else if (step_ns != 0) {
		ret = ptp_clock_adjust(clk, (int)step_ns);
	}
	if (ptp_clock_get(clk, &t) == 0) {
		gate.sec = (uint32_t)t.second;
		gate.ps = (int64_t)t.nanosecond * 1000;
		gate.backward = step_ps < 0;
		gate.until_ms = k_uptime_get() + 2 * (int64_t)chain_interval_us() / 1000 + 20;
		gate.on = true;
	}
	mark.t_ps[0] = step_ps;
	log_add(&mark);
	atomic_inc(&servo_steps);
	printk("servo: STEP %lld ns (%s %d)\n", (long long)step_ns, ret == 0 ? "ok" : "error", ret);
}

/* The servo of the uke and its statistics */
struct servo_run {
	bool on;
	struct chain_servo pi;
	int64_t next_ms;
	int64_t period_ms;
	/* the offsets of the period (for the loop) and of the 1 s window (for the log) */
	uint32_t p_n;
	int64_t p_sum_ps;
	uint32_t step_run;
	uint32_t w_n;
	double w_ref_ps, w_sum_ps, w_sumsq_ps2;
};

static void servo_start(struct servo_run *v)
{
	const struct device *clk = DEVICE_DT_GET(DT_NODELABEL(ptp_clock));
	uint32_t period_ms = (uint32_t)atomic_get(&servo_period_ms);

	chain_servo_init(&v->pi, (double)period_ms / 1e3, (double)atomic_get(&servo_bw_mhz) / 1e3,
			 (double)atomic_get(&servo_clamp_ns) * 1e3);
	v->period_ms = period_ms;
	v->next_ms = k_uptime_get() + period_ms;
	v->p_n = 0;
	v->p_sum_ps = 0;
	v->step_run = 0;
	v->w_n = 0;
	v->on = true;
	/* the nominal rate, then the loop learns the rest */
	(void)ptp_clock_rate_adjust(clk, 1.0);
	printk("servo: on, period %u ms, bandwidth %.3f Hz, clamp %lld ns\n", period_ms,
	       (double)atomic_get(&servo_bw_mhz) / 1e3, (long long)atomic_get(&servo_clamp_ns));
}

static void servo_sample(struct servo_run *v, int64_t offset_ps)
{
	double d_ps = (double)offset_ps;

	v->p_n++;
	v->p_sum_ps += offset_ps;
	if (v->w_n == 0) {
		v->w_ref_ps = d_ps;
	}
	v->w_n++;
	v->w_sum_ps += d_ps - v->w_ref_ps;
	v->w_sumsq_ps2 += (d_ps - v->w_ref_ps) * (d_ps - v->w_ref_ps);
}

/* End of a period: the mean offset of the period drives the loop; absolute rate out. */
static void servo_period(struct servo_run *v)
{
	const struct device *clk = DEVICE_DT_GET(DT_NODELABEL(ptp_clock));
	int64_t step_ps;
	double rate_ppb;

	if (v->p_n == 0) {
		return;
	}
	rate_ppb = chain_servo_update(&v->pi, (double)v->p_sum_ps / (double)v->p_n, &step_ps);
	v->p_n = 0;
	v->p_sum_ps = 0;
	if (step_ps != 0) {
		/* the first lock steps twice (seconds, then microseconds) */
		if (++v->step_run > SERVO_STEP_RUN_MAX) {
			printk("servo: %u steps in a row at rate %.3f ppb, restart\n", v->step_run,
			       v->pi.rate_ppb);
			servo_start(v);
			return;
		}
		step_clock(step_ps);
		/* what is queued predates the step */
		v->w_n = 0;
		v->w_sum_ps = 0;
		v->w_sumsq_ps2 = 0;
	} else {
		v->step_run = 0;
		/* absolute: addend = default addend * ratio */
		(void)ptp_clock_rate_adjust(clk, 1.0 + rate_ppb * 1e-9);
	}
}

/*
 * Die temperature in degrees Celsius for the window lines, NAN without a sensor or on an error.
 * The ESP HAL reads it under an interrupt lock on this CPU: a register read, except for the first
 * read and a range change, which wait 300 us (thread_fn reads once before the chain can run).
 */
static double die_temp_c(void)
{
#if DT_HAS_ALIAS(die_temp0) && DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(die_temp0))
	const struct device *dev = DEVICE_DT_GET(DT_ALIAS(die_temp0));
	struct sensor_value v;

	if (!device_is_ready(dev) || sensor_sample_fetch(dev) != 0 ||
	    sensor_channel_get(dev, SENSOR_CHAN_DIE_TEMP, &v) != 0) {
		return NAN;
	}
	return sensor_value_to_double(&v);
#else
	return NAN;
#endif
}

static void servo_window(struct servo_run *v)
{
	if (v->w_n != 0) {
		double n = (double)v->w_n;
		double m_ps = v->w_sum_ps / n;
		double var_ps2 = v->w_sumsq_ps2 / n - m_ps * m_ps;

		printk("servo n=%u mean=%.1f ps rms=%.1f ps rate=%.3f ppb integ=%.3f ppb steps=%u "
		       "temp=%.2f C\n",
		       v->w_n, v->w_ref_ps + m_ps, sqrt(var_ps2 > 0.0 ? var_ps2 : 0.0),
		       v->pi.rate_ppb, v->pi.integ_ppb, chain_servo_steps(), die_temp_c());
	} else {
		printk("servo n=0 rate=%.3f ppb integ=%.3f ppb steps=%u temp=%.2f C\n",
		       v->pi.rate_ppb, v->pi.integ_ppb, chain_servo_steps(), die_temp_c());
	}
	v->w_n = 0;
	v->w_sum_ps = 0;
	v->w_sumsq_ps2 = 0;
}

void chain_servo_config(uint32_t period_ms, uint32_t bw_mhz, uint32_t clamp_ns)
{
	atomic_set(&servo_period_ms, (atomic_val_t)period_ms);
	atomic_set(&servo_bw_mhz, (atomic_val_t)bw_mhz);
	atomic_set(&servo_clamp_ns, (atomic_val_t)clamp_ns);
	atomic_set(&servo_reconf, 1);
}

static void window_end(struct window *w)
{
	struct chain_fit_res r;
	int ok = chain_fit_end(&w->fit, &r);

	if (atomic_get(&chain_stat_on) != 0) {
		if (w->fit.n == 0) {
			printk("win n=0 tx=%u rx=%u lost=%u stamp_miss=%u ring_ovf=%u busy=%u "
			       "temp=%.2f C\n",
			       chain_cnt.tx_sent, chain_cnt.rx_chain, chain_cnt.rx_lost,
			       chain_cnt.tx_stamp_missing, chain_cnt.ring_overflow,
			       chain_cnt.tx_busy, die_temp_c());
		} else {
			printk("win n=%u off=%.1f ps rate=%.3f ppb rms=%.1f ps delay=%lld ps "
			       "tx=%u rx=%u lost=%u stamp_miss=%u ring_ovf=%u busy=%u temp=%.2f C%s\n",
			       r.n, r.y_mean_ps, r.rate_ppb, r.rms_ps,
			       (long long)(w->delay_sum_ps / (int64_t)w->fit.n), chain_cnt.tx_sent,
			       chain_cnt.rx_chain, chain_cnt.rx_lost, chain_cnt.tx_stamp_missing,
			       chain_cnt.ring_overflow, chain_cnt.tx_busy, die_temp_c(),
			       ok != 0 ? " (no fit)" : "");
		}
	}
	window_reset(w);
}

static void thread_fn(void *a, void *b, void *c)
{
	struct window w;
	struct servo_run v = {.on = false};
	int64_t next = k_uptime_get() + WINDOW_MS;
	struct chain_xchg x;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	window_reset(&w);
	/* the sensor's first read selects its range and waits 300 us with interrupts locked */
	(void)die_temp_c();
	for (;;) {
		bool run;

		k_msleep(DRAIN_MS);
		run = atomic_get(&chain_servo_on) != 0 && chain_role_get() != CHAIN_SEME;
		if (!run) {
			v.on = false;
		} else if (!v.on || atomic_cas(&servo_reconf, 1, 0)) {
			servo_start(&v);
		}
		while (chain_ring_pop(&x)) {
			int64_t delay_ps = chain_xchg_delay_ps(&x);

			if (delay_ps < 0 || delay_ps > DELAY_MAX_PS) {
				chain_cnt.rejected++;
				continue;
			}
			if (gate_blocks(&x)) {
				continue;
			}
			log_add(&x);
			window_add(&w, &x);
			if (v.on) {
				servo_sample(&v, chain_xchg_offset_ps(&x));
			}
		}
		if (v.on && k_uptime_get() >= v.next_ms) {
			v.next_ms += v.period_ms;
			servo_period(&v);
		}
		if (k_uptime_get() >= next) {
			next += WINDOW_MS;
			window_end(&w);
			if (v.on) {
				servo_window(&v);
			}
		}
	}
}

void chain_stats_init(void)
{
	k_thread_create(&thread, stack, K_THREAD_STACK_SIZEOF(stack), thread_fn, NULL, NULL, NULL,
			K_LOWEST_APPLICATION_THREAD_PRIO, 0, K_FOREVER);
	k_thread_name_set(&thread, "chain_stats");
#ifdef CONFIG_SMP
	k_thread_cpu_pin(&thread, 0);
#endif
	k_thread_start(&thread);
}
