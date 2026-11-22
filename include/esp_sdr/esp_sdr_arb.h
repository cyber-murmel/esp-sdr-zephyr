/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file
 * @brief Radio arbiter: the SDR and the Wi-Fi blob time-share one PHY (CONFIG_ESP_SDR_ARB).
 */

#ifndef ESP_SDR_ESP_SDR_ARB_H_
#define ESP_SDR_ESP_SDR_ARB_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup esp_sdr_arb
 * @{
 *
 * The Wi-Fi blob powers the PHY down in its sleep windows (station modem sleep, ESP-NOW wake
 * windows) and up again before the next beacon, through esp_phy_enable() and esp_phy_disable()
 * of the OS adapter. The arbiter wraps those two (linker `--wrap`, set up by the library) and owns
 * the real calls, with two clients: Wi-Fi and the SDR. The PHY is powered when either needs it.
 *
 * The SDR takes a window with esp_sdr_arb_claim() while Wi-Fi sleeps, runs until the window's
 * deadline (esp_sdr_arb_window_over()) and gives it back with esp_sdr_arb_release(), which
 * restores what Wi-Fi needs (AGC gain, filter codes, no TX forcing, Wi-Fi's channel). The
 * deadline is the next Wi-Fi enable predicted from the enable times seen so far (the median
 * enable-to-enable interval and the last enable) less a margin. If Wi-Fi wakes early anyway, its
 * enable preempts the SDR: it calls the stop hook, waits a bounded time for the release and goes
 * on (never longer than CONFIG_ESP_SDR_ARB_PREEMPT_WAIT_US).
 *
 * The Wi-Fi wake comes from the `wifi` thread on CPU 0 in task context; a ring that masks that
 * CPU cannot be preempted, so it must end by the deadline, or run on the other CPU.
 *
 * Capture and ring calls of the library return -EBUSY without a claim (-EAGAIN for passive
 * captures while Wi-Fi sleeps).
 *
 * As modules (apps/multiapp, CONFIG_ESP_SDR_AS_EXTENSION) the arbiter is in the `phy` module,
 * which the `wifi` and `sdr` modules share. A boot without the Wi-Fi driver (no
 * esp_sdr_arb_wifi_attach()) gets a standing claim from esp_sdr_init(), with no deadline, so its
 * apps run the SDR without claims as they did before the arbiter.
 */

/** What esp_sdr_arb_trace() reports. */
enum esp_sdr_arb_event {
	/** Wi-Fi asked for the PHY (arg: modem bits); t is when. */
	ESP_SDR_ARB_WIFI_ENABLE,
	/** Wi-Fi gave the PHY up (arg: modem bits). */
	ESP_SDR_ARB_WIFI_DISABLE,
	/** A claim was granted (arg: the window's deadline less now, us; 0 without one). */
	ESP_SDR_ARB_SDR_CLAIM,
	/** The SDR released (arg: how long it held, us). */
	ESP_SDR_ARB_SDR_RELEASE,
	/** Wi-Fi found the SDR holding the radio (arg: us the wait for the release took). */
	ESP_SDR_ARB_PREEMPT,
	/** The SDR did not release in time (arg: us waited). */
	ESP_SDR_ARB_PREEMPT_TIMEOUT,
};

/**
 * @brief Event hook (weak, empty by default): an application defines it to log them.
 *
 * Called in the context of the event (the `wifi` thread for Wi-Fi's, the SDR's thread for the
 * others), not in interrupts, with no arbiter lock held. @p t_us is on esp_sdr_arb_now_us().
 */
void esp_sdr_arb_trace(enum esp_sdr_arb_event ev, int64_t t_us, uint32_t arg);

/** @return The arbiter's clock, microseconds (esp_timer_get_time()), the base of every deadline. */
int64_t esp_sdr_arb_now_us(void);

/** Deadline of a window without one: Wi-Fi absent or never sleeping periodically. */
#define ESP_SDR_ARB_NO_DEADLINE INT64_MAX

/**
 * @brief Take the radio while Wi-Fi sleeps.
 *
 * Waits (for @p wait) until Wi-Fi has powered the PHY down with a window long enough
 * (CONFIG_ESP_SDR_ARB_MIN_WINDOW_US) or is absent, powers it up for the SDR, and re-applies the
 * SDR's own state (esp_sdr_rx_prepare(): frequency, front end, gain, filter).
 *
 * @param wait How long to wait for a window.
 * @param deadline_us Gets the window's end on esp_sdr_arb_now_us(): the predicted Wi-Fi wake less
 *                    CONFIG_ESP_SDR_ARB_MARGIN_US, or ESP_SDR_ARB_NO_DEADLINE. May be NULL.
 * @retval 0 The SDR holds the radio until esp_sdr_arb_release().
 * @retval -EBUSY Timed out with Wi-Fi awake (power save off, or never asleep that long).
 * @retval -EAGAIN Timed out with Wi-Fi asleep but without a long enough window yet.
 * @retval -EALREADY The SDR holds it already.
 * @retval -ECANCELED Wi-Fi woke during the SDR's setup: nothing is held, try again.
 */
int esp_sdr_arb_claim(k_timeout_t wait, int64_t *deadline_us);

/**
 * @brief Give the radio back: restore Wi-Fi's state, then drop the SDR (the PHY goes down if Wi-Fi
 * is asleep too). Safe without a claim.
 */
void esp_sdr_arb_release(void);

/**
 * @brief The function Wi-Fi's wake calls to ask the SDR to give the radio up, for the claim now
 * held (cleared by esp_sdr_arb_release()).
 *
 * Runs in the `wifi` thread: set a flag or give a semaphore and return; the SDR then calls
 * esp_sdr_arb_release(). A ring needs none: it polls esp_sdr_arb_window_over().
 */
void esp_sdr_arb_set_stop_hook(void (*fn)(void *user), void *user);

/**
 * @brief Whether the window has ended: the deadline passed or Wi-Fi asked for the radio.
 *
 * A compare of the systimer's microseconds, usable with interrupts masked and from IRAM code.
 * True without a claim.
 */
bool esp_sdr_arb_window_over(void);

/**
 * @brief What the SDR does at a claim and a release. The library registers it in esp_sdr_init(),
 * so the arbiter itself (the phy module of apps/multiapp) does not call the library's code.
 */
struct esp_sdr_arb_ops {
	/** The PHY has just powered up for a claim: the SDR's frequency, front end, gain, filter. */
	void (*prepare)(void);
	/** Before the release: Wi-Fi's gain, filter codes and channel (MHz of its centre, 0
	 *  unknown; @p cbw nonzero for HT40) back. */
	void (*restore)(uint32_t wifi_mhz, unsigned int wifi_cbw);
};

/** @brief Register the SDR's side (esp_sdr_init() does); NULL takes it away (the SDR unloads). */
void esp_sdr_arb_set_ops(const struct esp_sdr_arb_ops *ops);

/**
 * @brief The Wi-Fi driver is in this boot (apps/multiapp: the wifi module's multiapp_module_up()).
 *
 * Without it esp_sdr_init() takes a standing claim with no deadline, so the SDR's callers need no
 * claim of their own, as without the arbiter. With it they claim per window. A standing claim
 * already held is released.
 */
void esp_sdr_arb_wifi_attach(void);

/**
 * @brief Tell the arbiter Wi-Fi's channel, for the release to retune to.
 *
 * The arbiter makes no driver call of its own: a claim runs on the SDR's thread, which may hold
 * a CPU the Wi-Fi task needs inside the blob's locks. The application reads the channel
 * (esp_wifi_get_channel()) from its own thread before it starts the SDR and passes it here.
 *
 * @param primary Primary channel 1 to 14 (else ignored).
 * @param second Second channel: 0 none, 1 above, 2 below (wifi_second_chan_t).
 */
void esp_sdr_arb_wifi_set_channel(uint8_t primary, int second);

/**
 * @brief The Wi-Fi driver is gone from this boot (its module unloaded, after its deinit).
 *
 * Clears the attach and any hold Wi-Fi left, and powers the PHY down if nothing else holds it. An
 * SDR that initialized while Wi-Fi was attached claims per window; it must be reloaded to take the
 * standing claim again (apps/multiapp unloads it with Wi-Fi).
 */
void esp_sdr_arb_wifi_detach(void);

/**
 * @brief The arbiter's module unloads: nothing may hold the PHY any more.
 *
 * @retval 0 The PHY is down, the hooks are cleared.
 * @retval -EBUSY A client still holds the PHY; nothing changed.
 */
int esp_sdr_arb_shutdown(void);

/** @return Whether esp_sdr_arb_wifi_attach() was called. */
bool esp_sdr_arb_wifi_attached(void);

/** @return Whether the SDR holds the radio and Wi-Fi has not asked for it back. */
bool esp_sdr_arb_holds(void);

/** @return Whether Wi-Fi has the PHY powered (a passive capture sees live RF). */
bool esp_sdr_arb_wifi_awake(void);

struct esp_sdr_arb_stats {
	/** Claims granted, total time held (us), the longest hold. */
	uint32_t windows;
	uint64_t sdr_us;
	uint32_t hold_max_us;
	/** Claim time from the call to the grant, longest (us). */
	uint32_t claim_wait_max_us;
	/** Times Wi-Fi's enable found the SDR holding, how often the release did not come in time,
	 *  the longest wait for it (us). */
	uint32_t preempts, preempt_timeouts, preempt_wait_max_us;
	/** Releases later than the predicted wake (deadline plus CONFIG_ESP_SDR_ARB_MARGIN_US). */
	uint32_t deadline_misses;
	/** Wi-Fi's enables and disables seen, and wakes shorter than CONFIG_ESP_SDR_ARB_MIN_WAKE_US
	 *  that the predictor ignored. */
	uint32_t wifi_on, wifi_off, short_wakes;
	/** The learned wake period (us, 0 unknown) and how far an enable came off it (min, max). */
	uint32_t period_us;
	int32_t jitter_min_us, jitter_max_us;
	/** Time the SDR held the PHY powered by itself (a window's setup costs), total (us). */
	uint64_t powerup_us;
};

/** @brief Copy the counters. */
void esp_sdr_arb_get_stats(struct esp_sdr_arb_stats *stats);

/** @brief Zero the counters (not the learned period). */
void esp_sdr_arb_reset_stats(void);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ESP_SDR_ESP_SDR_ARB_H_ */
