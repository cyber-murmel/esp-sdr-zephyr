/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See ubt_mode.h.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <esp_sdr/esp_sdr.h>

#include "app_cpu.h"
#include "ubt_mode.h"

LOG_MODULE_REGISTER(ubt_mode, LOG_LEVEL_INF);

#define THREAD_STACK 1536
#define THREAD_PRIO  14

/* The reply of RESET and FLASH needs time to reach the host before the chip goes. */
#define RESET_DELAY_MS 1000

/* Rebase the clock at least this often so the cycle scaling cannot overflow. */
#define REBASE_MS 10000

static struct ubt_dev *dev;
static K_SEM_DEFINE(kick_sem, 0, 1);
/* Internal RAM below the ring banks is full: the stack lives in the high region. */
static struct {
	K_KERNEL_STACK_MEMBER(stack, THREAD_STACK);
} mode_high ESP_SDR_HIGH_RAM;
static struct k_thread thread;

/* XMAS: the one LED blinks (upstream runs a light show on the three of an Ubertooth One). */
static void xmas_run(struct ubt_dev *d)
{
	bool on = false;

	while (d->requested_mode == MODE_XMAS) {
		on = !on;
		if (d->plat->led != NULL) {
			d->plat->led(0, on);
		}
		k_sleep(K_MSEC(100));
	}
	if (d->plat->led != NULL) {
		d->plat->led(0, false);
	}
}

static const struct ubt_mode_ops mode_xmas = {.mode = MODE_XMAS, .run = xmas_run};

static const struct ubt_mode_ops mode_specan = {.mode = MODE_SPECAN, .run = ubt_specan_run};

static const struct ubt_mode_ops mode_btle = {.mode = MODE_BT_FOLLOW_LE, .run = ubt_btle_run};
static const struct ubt_mode_ops mode_slave = {.mode = MODE_BT_SLAVE_LE, .run = ubt_btle_slave_run};
static const struct ubt_mode_ops mode_txtest = {.mode = MODE_TX_TEST, .run = ubt_tx_test_run};
static const struct ubt_mode_ops mode_txgen = {.mode = MODE_TX_GENERIC, .run = ubt_tx_generic_run};

static const struct ubt_mode_ops *const modes[] = {
	&mode_xmas,
	&mode_specan,
	&mode_btle,
	&mode_slave,
	&mode_txtest,
	&mode_txgen,
};

static const struct ubt_mode_ops *find_mode(uint8_t mode)
{
	for (size_t i = 0; i < ARRAY_SIZE(modes); i++) {
		if (modes[i]->mode == mode) {
			return modes[i];
		}
	}
	return NULL;
}

void ubt_mode_kick(void)
{
	k_sem_give(&kick_sem);
}

static void mode_thread(void *p1, void *p2, void *p3)
{
	int64_t last_rebase = k_uptime_get();

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		uint8_t req;
		const struct ubt_mode_ops *ops;

		(void)k_sem_take(&kick_sem, K_MSEC(REBASE_MS / 4));
		if (k_uptime_get() - last_rebase >= REBASE_MS) {
			ubt_clock_rebase(&dev->clock, dev->plat->cycles());
			last_rebase = k_uptime_get();
		}

		req = dev->requested_mode;
		if (req == dev->mode) {
			continue;
		}
		if (req == MODE_RESET) {
			k_sleep(K_MSEC(RESET_DELAY_MS));
			if (dev->plat->reset == NULL || !dev->plat->reset(dev->reset_dfu)) {
				LOG_WRN("reset failed");
				dev->requested_mode = MODE_IDLE;
			}
			continue;
		}
		if (req == MODE_IDLE) {
			ubt_dev_reset(dev);
			continue;
		}
		ops = find_mode(req);
		if (ops == NULL) {
			LOG_WRN("mode %u not supported", req);
			dev->requested_mode = MODE_IDLE;
			continue;
		}
		ubt_queue_reset(&dev->queue);
		dev->mode = req;
		ops->run(dev);
		dev->mode = MODE_IDLE;
		if (dev->requested_mode == MODE_IDLE) {
			ubt_dev_reset(dev);
		}
	}
}

void ubt_mode_init(struct ubt_dev *d)
{
	k_tid_t tid;

	dev = d;
	d->supported_modes = 0;
	for (size_t i = 0; i < ARRAY_SIZE(modes); i++) {
		d->supported_modes |= UBT_MODE_BIT(modes[i]->mode);
	}
	tid = k_thread_create(&thread, mode_high.stack, K_KERNEL_STACK_SIZEOF(mode_high.stack), mode_thread, NULL, NULL,
			      NULL, THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(tid, "ubt_mode");
	k_thread_cpu_pin(tid, 0);
}
