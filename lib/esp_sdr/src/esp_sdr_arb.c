/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Radio arbiter: the Wi-Fi blob and the SDR time-share one PHY (include/esp_sdr/esp_sdr_arb.h).
 *
 * Two clients, WIFI and SDR, own the real esp_phy_enable()/esp_phy_disable(): the PHY is powered
 * while either is set. Wi-Fi's calls arrive through the linker wraps below; the SDR's through
 * esp_sdr_arb_claim() and esp_sdr_arb_release().
 *
 *   Wi-Fi awake, SDR idle   ->  claim waits (a window needs Wi-Fi's disable)
 *   Wi-Fi asleep (disable)  ->  claim powers the PHY up, SDR runs until the deadline
 *   Wi-Fi enable, SDR holds ->  stop hook, wait for the release (bounded), Wi-Fi goes on
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#include <esp_phy_init.h>
#include <esp_timer.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_arb.h>
#include <esp_sdr/esp_sdr_place.h>

#include "esp_sdr_arb_pred.h"

/* Wi-Fi's wifi_second_chan_t, which the phy module cannot include (the driver is another module). */
#define SECOND_NONE  0
#define SECOND_ABOVE 1
#define SECOND_BELOW 2

#define CLIENT_WIFI BIT(0)
#define CLIENT_SDR  BIT(1)

/* What esp_sdr_arb_window_over() and esp_sdr_arb_gate() read, with interrupts masked too. */
struct arb_win {
	volatile bool claimed;
	volatile bool preempt;
	volatile bool no_deadline;
	volatile bool wifi_on;
	int64_t deadline_us;
};

/* In the high region, which boot does not zero: arb_init() does. */
static struct arb_win win ESP_SDR_DATA_F2(STATE, win, NOINIT, ATOMIC);

/*
 * Where the last claim or release got to, for a debugger after a hang: 1 claim entered, 2 PHY
 * powered, 3 before the SDR's prepare, 4 after it, 5 before the restore, 6 after it, 7 released.
 */
volatile uint32_t esp_sdr_arb_step;

static K_MUTEX_DEFINE(arb_lock);
static K_CONDVAR_DEFINE(arb_cv);
static K_SEM_DEFINE(released, 0, 1);

/* Guarded by arb_lock. */
static uint32_t clients;
static bool real_on;
static struct arb_pred pred;
static struct esp_sdr_arb_stats stats;
static int64_t claim_t_us, claim_deadline_us;
/* Wi-Fi's latest enable, not yet taught to the predictor: only a wake that lasts is real. */
static int64_t pending_t_us;
static bool pending;
static void (*stop_hook)(void *user);
static void *stop_user;
/* Wi-Fi's channel as the last claim saw it (MHz of its centre, 0 unknown). */
static uint32_t wifi_mhz, wifi_cbw;
/* The SDR's side of a claim and a release (esp_sdr_arb_set_ops()). */
static struct esp_sdr_arb_ops sdr_ops;
static volatile bool wifi_attached;
/* The claim esp_sdr_init() keeps when no Wi-Fi is attached. */
static bool standing;


__weak void esp_sdr_arb_trace(enum esp_sdr_arb_event ev, int64_t t_us, uint32_t arg)
{
	ARG_UNUSED(ev);
	ARG_UNUSED(t_us);
	ARG_UNUSED(arg);
}

/* The systimer's microseconds, the same on both CPUs (the CPUs' CCOUNT registers are not shared). */
int64_t esp_sdr_arb_now_us(void)
{
	return esp_timer_get_time();
}

void __real_esp_phy_enable(esp_phy_modem_t modem);
void __real_esp_phy_disable(esp_phy_modem_t modem);

/* The real power calls, arb_lock held. flag: also the PHY's Wi-Fi flag, which the adapter sets itself. */
static void real_up(bool flag)
{
	__real_esp_phy_enable(PHY_MODEM_WIFI);
	if (flag && IS_ENABLED(CONFIG_ESP_SDR_ARB_WIFI_FLAG)) {
		phy_wifi_enable_set(1);
	}
	real_on = true;
}

static void real_down(bool flag)
{
	if (flag && IS_ENABLED(CONFIG_ESP_SDR_ARB_WIFI_FLAG)) {
		phy_wifi_enable_set(0);
	}
	__real_esp_phy_disable(PHY_MODEM_WIFI);
	real_on = false;
}

ESP_SDR_CODE(PACED_CHUNK, esp_sdr_arb_window_over)
bool esp_sdr_arb_window_over(void)
{
	if (!win.claimed || win.preempt) {
		return true;
	}
	return !win.no_deadline && esp_timer_get_time() >= win.deadline_us;
}

bool esp_sdr_arb_holds(void)
{
	return win.claimed && !win.preempt;
}

bool esp_sdr_arb_wifi_awake(void)
{
	return win.wifi_on;
}

int esp_sdr_arb_gate(bool passive)
{
	if (win.claimed && !win.preempt) {
		return 0;
	}
	if (passive) {
		return win.wifi_on ? 0 : -EAGAIN;
	}
	return -EBUSY;
}

void esp_sdr_arb_set_stop_hook(void (*fn)(void *user), void *user)
{
	k_mutex_lock(&arb_lock, K_FOREVER);
	stop_hook = fn;
	stop_user = user;
	k_mutex_unlock(&arb_lock);
}

/* Teach the predictor a real wake (arb_lock held): jitter against the median, then the time. */
static void learn(int64_t t)
{
	int64_t period = arb_pred_period(&pred);

	if (period != 0 && pred.seen > 0U) {
		/* How far this wake came from where the median put it; skipped wakes are no jitter. */
		int64_t err = t - arb_pred_newest(&pred, 0U) - period;

		if (err > -period / 2 && err < period / 2) {
			stats.jitter_min_us = MIN(stats.jitter_min_us, (int32_t)err);
			stats.jitter_max_us = MAX(stats.jitter_max_us, (int32_t)err);
		}
	}
	arb_pred_add(&pred, t);
	stats.period_us = (uint32_t)arb_pred_period(&pred);
}

/* Wi-Fi's wake (esp_phy_enable). */
static void wifi_enable(esp_phy_modem_t modem)
{
	int64_t t = esp_sdr_arb_now_us();
	void (*hook)(void *user) = NULL;
	void *user = NULL;
	bool preempt = false;

	k_mutex_lock(&arb_lock, K_FOREVER);
	pending_t_us = t;
	pending = true;
	stats.wifi_on++;
	win.wifi_on = true;
	if ((clients & CLIENT_SDR) != 0U) {
		/* The SDR holds the powered PHY: nothing to power, it must let go. */
		win.preempt = true;
		hook = stop_hook;
		user = stop_user;
		preempt = true;
		k_sem_reset(&released);
	} else if (!real_on) {
		real_up(false);
	}
	clients |= CLIENT_WIFI;
	k_condvar_broadcast(&arb_cv);
	k_mutex_unlock(&arb_lock);
	esp_sdr_arb_trace(ESP_SDR_ARB_WIFI_ENABLE, t, (uint32_t)modem);

	if (preempt) {
		int64_t t0 = esp_sdr_arb_now_us(), waited;
		int ret;

		if (hook != NULL) {
			hook(user);
		}
		ret = k_sem_take(&released, K_USEC(CONFIG_ESP_SDR_ARB_PREEMPT_WAIT_US));
		waited = esp_sdr_arb_now_us() - t0;
		k_mutex_lock(&arb_lock, K_FOREVER);
		stats.preempts++;
		stats.preempt_wait_max_us = MAX(stats.preempt_wait_max_us, (uint32_t)waited);
		if (ret != 0) {
			stats.preempt_timeouts++;
		}
		k_mutex_unlock(&arb_lock);
		esp_sdr_arb_trace(ret != 0 ? ESP_SDR_ARB_PREEMPT_TIMEOUT : ESP_SDR_ARB_PREEMPT, t0,
				  (uint32_t)waited);
	}
}

/* Wi-Fi's sleep (esp_phy_disable); the adapter cleared the PHY's Wi-Fi flag already. */
static void wifi_disable(esp_phy_modem_t modem)
{
	int64_t t = esp_sdr_arb_now_us();

	k_mutex_lock(&arb_lock, K_FOREVER);
	stats.wifi_off++;
	if (pending) {
		pending = false;
		if (t - pending_t_us >= CONFIG_ESP_SDR_ARB_MIN_WAKE_US) {
			learn(pending_t_us);
		} else {
			stats.short_wakes++;
		}
	}
	win.wifi_on = false;
	clients &= ~CLIENT_WIFI;
	if (clients == 0U && real_on) {
		real_down(false);
	}
	k_condvar_broadcast(&arb_cv);
	k_mutex_unlock(&arb_lock);
	esp_sdr_arb_trace(ESP_SDR_ARB_WIFI_DISABLE, t, (uint32_t)modem);
}

/*
 * The blob's calls. Other modems (BT, 802.15.4) go straight on; a call from an interrupt cannot
 * take the lock and does too (the blob calls from its task only).
 */
void __wrap_esp_phy_enable(esp_phy_modem_t modem)
{
	esp_phy_modem_t other = modem & ~PHY_MODEM_WIFI;

	if (k_is_in_isr() || (modem & PHY_MODEM_WIFI) == 0) {
		__real_esp_phy_enable(modem);
		return;
	}
	if (other != 0) {
		__real_esp_phy_enable(other);
	}
	wifi_enable(modem);
}

void __wrap_esp_phy_disable(esp_phy_modem_t modem)
{
	esp_phy_modem_t other = modem & ~PHY_MODEM_WIFI;

	if (k_is_in_isr() || (modem & PHY_MODEM_WIFI) == 0) {
		__real_esp_phy_disable(modem);
		return;
	}
	if (other != 0) {
		__real_esp_phy_disable(other);
	}
	wifi_disable(modem);
}

/* Wi-Fi's channel as the application read it from the driver (esp_sdr_arb.h): no blob call here. */
void esp_sdr_arb_wifi_set_channel(uint8_t primary, int second)
{
	uint32_t mhz, cbw = 0U;

	if (primary == 0U || primary > 14U) {
		return;
	}
	mhz = primary == 14U ? 2484U : 2407U + 5U * primary;
	if (second == SECOND_ABOVE) {
		mhz += 10U;
		cbw = 1U;
	} else if (second == SECOND_BELOW) {
		mhz -= 10U;
		cbw = 1U;
	}
	k_mutex_lock(&arb_lock, K_FOREVER);
	wifi_mhz = mhz;
	wifi_cbw = cbw;
	k_mutex_unlock(&arb_lock);
}

/* Whether a window can start now, arb_lock held; *deadline gets its end. */
static int window_check(int64_t now, int64_t *deadline)
{
	if (win.wifi_on) {
		return -EBUSY;
	}
	switch (arb_pred_state_at(&pred, now)) {
	case ARB_PRED_LEARNING:
		return -EAGAIN;
	case ARB_PRED_NONE:
		*deadline = ESP_SDR_ARB_NO_DEADLINE;
		return 0;
	default:
		break;
	}
	*deadline = arb_pred_next(&pred, now) - CONFIG_ESP_SDR_ARB_MARGIN_US;
	return *deadline - now >= CONFIG_ESP_SDR_ARB_MIN_WINDOW_US ? 0 : -EAGAIN;
}

int esp_sdr_arb_claim(k_timeout_t wait, int64_t *deadline_us)
{
	k_timepoint_t end = sys_timepoint_calc(wait);
	int64_t t_call = esp_sdr_arb_now_us(), now, deadline = 0, t_up;
	int ret;

	if (win.claimed) {
		return -EALREADY;
	}
	esp_sdr_arb_step = 1U;
	k_mutex_lock(&arb_lock, K_FOREVER);
	for (;;) {
		now = esp_sdr_arb_now_us();
		ret = window_check(now, &deadline);
		if (ret == 0) {
			break;
		}
		k_timeout_t left = sys_timepoint_timeout(end);

		if (K_TIMEOUT_EQ(left, K_NO_WAIT) ||
		    k_condvar_wait(&arb_cv, &arb_lock, left) != 0) {
			k_mutex_unlock(&arb_lock);
			return ret;
		}
	}
	if (win.claimed) {
		k_mutex_unlock(&arb_lock);
		return -EALREADY;
	}
	if (!real_on) {
		real_up(true);
		stats.powerup_us += (uint64_t)(esp_sdr_arb_now_us() - now);
	}
	esp_sdr_arb_step = 2U;
	t_up = esp_sdr_arb_now_us();
	clients |= CLIENT_SDR;
	win.preempt = false;
	win.no_deadline = deadline == ESP_SDR_ARB_NO_DEADLINE;
	win.deadline_us = deadline;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	win.claimed = true;
	standing = deadline == ESP_SDR_ARB_NO_DEADLINE && !wifi_attached;
	claim_t_us = t_up;
	claim_deadline_us = deadline;
	k_sem_reset(&released);
	k_mutex_unlock(&arb_lock);

	/* The PHY may have been down: the SDR's frequency, front end, gain and filter again. */
	esp_sdr_arb_step = 3U;
	if (sdr_ops.prepare != NULL && !win.preempt) {
		sdr_ops.prepare();
	}
	esp_sdr_arb_step = 4U;

	k_mutex_lock(&arb_lock, K_FOREVER);
	stats.windows++;
	stats.claim_wait_max_us = MAX(stats.claim_wait_max_us, (uint32_t)(t_up - t_call));
	k_mutex_unlock(&arb_lock);
	if (deadline_us != NULL) {
		*deadline_us = deadline;
	}
	esp_sdr_arb_trace(ESP_SDR_ARB_SDR_CLAIM, t_up,
			  deadline == ESP_SDR_ARB_NO_DEADLINE ? 0U : (uint32_t)(deadline - t_up));
	if (win.preempt) {
		/* Wi-Fi woke during the setup. */
		esp_sdr_arb_release();
		return -ECANCELED;
	}
	return 0;
}

void esp_sdr_arb_release(void)
{
	int64_t t, t_rel = esp_sdr_arb_now_us();
	bool wifi, preempted = win.preempt;

	if (!win.claimed) {
		return;
	}
	/* Wi-Fi's state back, while the PHY is powered. */
	esp_sdr_arb_step = 5U;
	if (sdr_ops.restore != NULL) {
		sdr_ops.restore(wifi_mhz, wifi_cbw);
	}
	esp_sdr_arb_step = 6U;

	k_mutex_lock(&arb_lock, K_FOREVER);
	t = esp_sdr_arb_now_us();
	wifi = (clients & CLIENT_WIFI) != 0U;
#if defined(CONFIG_ESP_SDR_ARB_PHY_CYCLE)
	if (wifi) {
		/* Wi-Fi is waiting for this: a power cycle resets what the restore may have missed. */
		real_down(true);
		real_up(true);
	}
#endif
	if (t - claim_t_us > (int64_t)stats.hold_max_us) {
		stats.hold_max_us = (uint32_t)(t - claim_t_us);
	}
	stats.sdr_us += (uint64_t)(t - claim_t_us);
	/*
	 * Late = released past the predicted wake (deadline plus margin), timed at the call; a run
	 * overshoots the deadline by up to one burst or unit by design. A preemption is not a miss.
	 */
	if (!preempted && claim_deadline_us != ESP_SDR_ARB_NO_DEADLINE &&
	    t_rel > claim_deadline_us + CONFIG_ESP_SDR_ARB_MARGIN_US) {
		stats.deadline_misses++;
	}
	win.claimed = false;
	win.preempt = false;
	standing = false;
	clients &= ~CLIENT_SDR;
	stop_hook = NULL;
	if (!wifi && real_on) {
		real_down(true);
	}
	k_sem_give(&released);
	k_condvar_broadcast(&arb_cv);
	k_mutex_unlock(&arb_lock);
	esp_sdr_arb_trace(ESP_SDR_ARB_SDR_RELEASE, t, (uint32_t)(t - claim_t_us));
	esp_sdr_arb_step = 7U;
}

void esp_sdr_arb_set_ops(const struct esp_sdr_arb_ops *ops)
{
	k_mutex_lock(&arb_lock, K_FOREVER);
	if (ops != NULL) {
		sdr_ops = *ops;
	} else {
		memset(&sdr_ops, 0, sizeof(sdr_ops));
	}
	k_mutex_unlock(&arb_lock);
}

void esp_sdr_arb_wifi_attach(void)
{
	bool drop;

	k_mutex_lock(&arb_lock, K_FOREVER);
	wifi_attached = true;
	drop = standing;
	k_mutex_unlock(&arb_lock);
	if (drop) {
		/* The SDR loaded first and took the radio for good: from now on it takes windows. */
		esp_sdr_arb_release();
	}
}

bool esp_sdr_arb_wifi_attached(void)
{
	return wifi_attached;
}

void esp_sdr_arb_wifi_detach(void)
{
	k_mutex_lock(&arb_lock, K_FOREVER);
	wifi_attached = false;
	/* The driver's deinit gave its holds back; a hold that is left would keep the PHY up. */
	win.wifi_on = false;
	clients &= ~CLIENT_WIFI;
	pending = false;
	arb_pred_reset(&pred);
	if (clients == 0U && real_on) {
		real_down(true);
	}
	k_condvar_broadcast(&arb_cv);
	k_mutex_unlock(&arb_lock);
}

int esp_sdr_arb_shutdown(void)
{
	int ret = 0;

	k_mutex_lock(&arb_lock, K_FOREVER);
	if (clients != 0U || win.claimed) {
		/* A client is left: the PHY stays up and the module with it. */
		ret = -EBUSY;
	} else if (real_on) {
		real_down(true);
	}
	if (ret == 0) {
		wifi_attached = false;
		stop_hook = NULL;
		memset(&sdr_ops, 0, sizeof(sdr_ops));
	}
	k_mutex_unlock(&arb_lock);
	return ret;
}

#if defined(CONFIG_ESP_SDR_AS_EXTENSION)
/*
 * The phy module's tracking timer (phy_common.c, built here with CONFIG_ESP_WIFI_ENABLED) asks to
 * track the PLL whenever the PHY's Wi-Fi flag is set, which the SDR's windows and its standing
 * claim set too. Only Wi-Fi's own hold gets it: the PHY is not touched behind a ring.
 */
void __real_phy_param_track_tot(bool en_wifi, bool en_ble_154);

void __wrap_phy_param_track_tot(bool en_wifi, bool en_ble_154)
{
	if (en_wifi && !(win.wifi_on && !win.claimed)) {
		en_wifi = false;
	}
	if (en_wifi || en_ble_154) {
		__real_phy_param_track_tot(en_wifi, en_ble_154);
	}
}
#endif

void esp_sdr_arb_get_stats(struct esp_sdr_arb_stats *out)
{
	k_mutex_lock(&arb_lock, K_FOREVER);
	*out = stats;
	k_mutex_unlock(&arb_lock);
}

void esp_sdr_arb_reset_stats(void)
{
	k_mutex_lock(&arb_lock, K_FOREVER);
	memset(&stats, 0, sizeof(stats));
	stats.period_us = (uint32_t)arb_pred_period(&pred);
	stats.jitter_min_us = INT32_MAX;
	stats.jitter_max_us = INT32_MIN;
	k_mutex_unlock(&arb_lock);
}

static int arb_init(void)
{
	memset(&win, 0, sizeof(win));
	arb_pred_reset(&pred);
	stats.jitter_min_us = INT32_MAX;
	stats.jitter_max_us = INT32_MIN;
	return 0;
}
SYS_INIT(arb_init, POST_KERNEL, 0);
