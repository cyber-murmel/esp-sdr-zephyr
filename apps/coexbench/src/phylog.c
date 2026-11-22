/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A log of the Wi-Fi blob's PHY power and channel calls. The power calls come from the radio
 * arbiter's trace hook (lib/esp_sdr/src/esp_sdr_arb.c, which wraps them), the channel calls
 * through linker wraps (CMakeLists.txt): each is recorded in a ring and goes on to the real
 * function.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>

#include <esp_phy_init.h>
#include <esp_timer.h>

#include <esp_sdr/esp_sdr_arb.h>
#include <esp_sdr/esp_sdr_place.h>

#include "coex.h"

enum ev_kind {
	EV_PHY_ENABLE,
	EV_PHY_DISABLE,
	EV_CHANGE_CHANNEL,
	EV_SET_CHAN,
	EV_SET_CHAN_MISC,
	EV_SET_CHANFREQ,
	EV_SDR_CLAIM,
	EV_SDR_RELEASE,
	EV_PREEMPT,
	EV_PREEMPT_TIMEOUT,
	EV_COUNT,
};

static const char *const ev_name[EV_COUNT] = {
	"phy_enable", "phy_disable", "phy_change_channel", "chip_v7_set_chan",
	"chip_v7_set_chan_misc", "set_chanfreq", "sdr_claim", "sdr_release", "preempt",
	"preempt_timeout",
};

struct ev {
	uint64_t t_us;
	uint32_t a0;
	uint32_t a1;
	uint8_t kind;
	uint8_t isr;
	uint8_t cpu;
	uint8_t chan;
	char thread[12];
};

#define N CONFIG_COEX_LOG_ENTRIES

/* In PSRAM: the DRAM image must end below the ring's banks (socs/esp32s3_procpu.conf). */
static struct ev ring[N] ESP_SDR_DATA(BULK, ring);
static uint32_t head; /* Entries ever written. */
static uint32_t counts[EV_COUNT];
static struct k_spinlock lock;
static volatile uint8_t wifi_chan;

void phylog_set_channel(uint8_t chan)
{
	wifi_chan = chan;
}

uint8_t phylog_channel(void)
{
	return wifi_chan;
}

static void record_at(enum ev_kind kind, int64_t t_us, uint32_t a0, uint32_t a1)
{
	struct ev e = {
		.t_us = (uint64_t)t_us,
		.a0 = a0,
		.a1 = a1,
		.kind = kind,
		.isr = k_is_in_isr(),
		.cpu = arch_curr_cpu()->id,
		.chan = wifi_chan,
	};
	k_spinlock_key_t key;

	if (!e.isr) {
		const char *name = k_thread_name_get(k_current_get());

		if (name != NULL) {
			strncpy(e.thread, name, sizeof(e.thread) - 1);
		}
	}
	key = k_spin_lock(&lock);
	ring[head % N] = e;
	head++;
	counts[kind]++;
	k_spin_unlock(&lock, key);
}

static void record(enum ev_kind kind, uint32_t a0, uint32_t a1)
{
	record_at(kind, esp_timer_get_time(), a0, a1);
}

/* The arbiter's events: Wi-Fi's PHY enables and disables (a0: modem bits), the SDR's windows. */
void esp_sdr_arb_trace(enum esp_sdr_arb_event ev, int64_t t_us, uint32_t arg)
{
	static const enum ev_kind kind[] = {
		[ESP_SDR_ARB_WIFI_ENABLE] = EV_PHY_ENABLE,
		[ESP_SDR_ARB_WIFI_DISABLE] = EV_PHY_DISABLE,
		[ESP_SDR_ARB_SDR_CLAIM] = EV_SDR_CLAIM,
		[ESP_SDR_ARB_SDR_RELEASE] = EV_SDR_RELEASE,
		[ESP_SDR_ARB_PREEMPT] = EV_PREEMPT,
		[ESP_SDR_ARB_PREEMPT_TIMEOUT] = EV_PREEMPT_TIMEOUT,
	};

	record_at(kind[ev], t_us, arg, 0);
}

/*
 * The blob's channel entry points: signatures unknown, so up to six word arguments are passed
 * through (Xtensa windowed ABI: a2 to a7) and a word returned.
 */
#define WRAP_CHAN(name, kind)                                                                      \
	uint32_t __real_##name(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);        \
	uint32_t __wrap_##name(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4,    \
			       uint32_t a5)                                                        \
	{                                                                                          \
		record(kind, a0, a1);                                                              \
		return __real_##name(a0, a1, a2, a3, a4, a5);                                      \
	}

WRAP_CHAN(phy_change_channel, EV_CHANGE_CHANNEL)
WRAP_CHAN(chip_v7_set_chan, EV_SET_CHAN)
WRAP_CHAN(chip_v7_set_chan_misc, EV_SET_CHAN_MISC)
WRAP_CHAN(set_chanfreq, EV_SET_CHANFREQ)

struct span {
	uint32_t n;
	uint64_t min, max, sum;
};

static void span_add(struct span *s, uint64_t v)
{
	s->min = s->n == 0U ? v : MIN(s->min, v);
	s->max = MAX(s->max, v);
	s->sum += v;
	s->n++;
}

static void span_print(const struct shell *sh, const char *what, const struct span *s)
{
	if (s->n == 0U) {
		shell_print(sh, "%s: none", what);
		return;
	}
	shell_print(sh, "%s: n %u min %llu mean %llu max %llu us", what, s->n, s->min,
		    s->sum / s->n, s->max);
}

int phylog_shell(const struct shell *sh, bool clear)
{
	struct span off = {0}, on = {0};
	uint32_t total, first, c[EV_COUNT];
	uint64_t t_dis = 0, t_en = 0;
	bool have_dis = false, have_en = false;
	k_spinlock_key_t key = k_spin_lock(&lock);

	total = head;
	memcpy(c, counts, sizeof(c));
	k_spin_unlock(&lock, key);
	first = total > N ? total - N : 0U;

	for (uint32_t i = first; i < total; i++) {
		struct ev e = ring[i % N];

		shell_print(sh, "%llu %-22s a0 %u a1 %u isr %u cpu %u thr %s ch %u", e.t_us,
			    ev_name[e.kind], e.a0, e.a1, e.isr, e.cpu, e.thread, e.chan);
		/* Wi-Fi's own bit only: the others have their own timelines. */
		if (e.kind == EV_PHY_ENABLE && (e.a0 & PHY_MODEM_WIFI)) {
			if (have_dis) {
				span_add(&off, e.t_us - t_dis);
			}
			t_en = e.t_us;
			have_en = true;
			have_dis = false;
		} else if (e.kind == EV_PHY_DISABLE && (e.a0 & PHY_MODEM_WIFI)) {
			if (have_en) {
				span_add(&on, e.t_us - t_en);
			}
			t_dis = e.t_us;
			have_dis = true;
			have_en = false;
		}
	}
	shell_print(sh, "entries %u shown %u (ring %d)", total, total - first, N);
	for (int k = 0; k < EV_COUNT; k++) {
		shell_print(sh, "count %-22s %u", ev_name[k], c[k]);
	}
	span_print(sh, "off window (enable - previous disable)", &off);
	span_print(sh, "on time (disable - enable)", &on);
	if (clear) {
		key = k_spin_lock(&lock);
		head = 0U;
		memset(counts, 0, sizeof(counts));
		k_spin_unlock(&lock, key);
		shell_print(sh, "log cleared");
	}
	return 0;
}
