/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The ESP32's EN and IO0 and the FPGA's CRESET and CDONE. Resets of the ESP32 as the stock
 * firmware's uart_task.c: each one holds the FPGA in reset too (the ESP32 loads it again, register
 * 5 keeps its value), EN low for a millisecond, IO0 low with it for download mode. Unlike there,
 * IO0 is let go again: it is also the codec's clock, and stayed low until the next normal reset.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "coproc.h"
#include "regmap.h"
#include "reset_detect.h"

LOG_MODULE_REGISTER(esp32_ctl, LOG_LEVEL_INF);

#define USER_NODE DT_PATH(zephyr_user)

/* After EN rises the ESP32 has read its strapping pins long ago. */
#define IO0_HOLD_MS 50
#define EN_LOW_MS   1
/* The circuit's capacitor on EN: IO0 goes low before EN rises, whichever line moves first. */
#define CIRCUIT_EN_DELAY_MS 10

static const struct gpio_dt_spec en = GPIO_DT_SPEC_GET(USER_NODE, esp32_en_gpios);
static const struct gpio_dt_spec io0 = GPIO_DT_SPEC_GET(USER_NODE, esp32_io0_gpios);
static const struct gpio_dt_spec creset = GPIO_DT_SPEC_GET(USER_NODE, fpga_creset_gpios);
static const struct gpio_dt_spec cdone = GPIO_DT_SPEC_GET(USER_NODE, fpga_cdone_gpios);

static struct reset_detect detect;
static bool reset_active;
static bool circuit_en_low;

static void io0_release_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)gpio_pin_set_dt(&io0, 0);
}
static K_WORK_DELAYABLE_DEFINE(io0_release_work, io0_release_fn);

static void en_release_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)gpio_pin_set_dt(&en, 1);
	reset_active = false;
	(void)k_work_reschedule(&io0_release_work, K_MSEC(IO0_HOLD_MS));
}
static K_WORK_DELAYABLE_DEFINE(en_release_work, en_release_fn);

void esp32_reset(bool download)
{
	if (reset_active) {
		return;
	}
	reset_active = true;
	coproc_stats.esp32_resets++;
	(void)k_work_cancel_delayable(&io0_release_work);
	(void)gpio_pin_set_dt(&creset, 1);
	(void)gpio_pin_set_dt(&en, 0);
	(void)gpio_pin_set_dt(&io0, download ? 1 : 0);
	reset_detect_clear(&detect);
	(void)k_work_reschedule(&en_release_work, K_MSEC(EN_LOW_MS));
}

/* What a download reset becomes with the lock set: the launcher's flash protection. */
static bool download_allowed(void)
{
	if (regs_get(REG_RESET_LOCK) == 0U) {
		return true;
	}
	regs_set(REG_RESET_ATTEMPTED, 1);
	coproc_stats.resets_locked++;
	return false;
}

static void circuit_lines(bool dtr, bool rts)
{
	struct reset_lines l = reset_detect_circuit(dtr, rts);

	if (l.io0_low && download_allowed()) {
		(void)k_work_cancel_delayable(&io0_release_work);
		(void)gpio_pin_set_dt(&io0, 1);
	} else if (!l.io0_low) {
		/* Later: EN may be about to rise. */
		(void)k_work_reschedule(&io0_release_work, K_MSEC(IO0_HOLD_MS));
	}
	if (l.en_low) {
		if (!circuit_en_low) {
			coproc_stats.esp32_resets++;
		}
		circuit_en_low = true;
		(void)k_work_cancel_delayable(&en_release_work);
		(void)gpio_pin_set_dt(&creset, 1);
		(void)gpio_pin_set_dt(&en, 0);
	} else if (circuit_en_low) {
		circuit_en_low = false;
		reset_active = true;
		(void)k_work_reschedule(&en_release_work, K_MSEC(CIRCUIT_EN_DELAY_MS));
	}
}

void esp32_line_state(bool dtr, bool rts)
{
	if (!bridge_cdc_owns_esp32()) {
		return;
	}
	if (detect.mode == COPROC_RESET_CIRCUIT) {
		circuit_lines(dtr, rts);
		return;
	}
	switch (reset_detect_stock(&detect, dtr, rts, reset_active)) {
	case RESET_ACTION_DOWNLOAD:
		esp32_reset(download_allowed());
		break;
	case RESET_ACTION_APP:
		esp32_reset(false);
		break;
	default:
		break;
	}
}

void esp32_set_reset_mode(uint8_t mode)
{
	reset_detect_init(&detect, mode);
	regs_set(COPROC_REG_RESET_MODE, mode);
}

uint8_t esp32_get_reset_mode(void)
{
	return detect.mode;
}

void fpga_run(bool run)
{
	(void)gpio_pin_set_dt(&creset, run ? 0 : 1);
}

bool fpga_cdone(void)
{
	return gpio_pin_get_dt(&cdone) > 0;
}

int esp32_ctl_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&en)) {
		return -ENODEV;
	}
	/* EN and CRESET were driven low at PRE_KERNEL_2 (sysctl.c). */
	ret = gpio_pin_configure_dt(&en, GPIO_OUTPUT_INACTIVE);
	ret = ret != 0 ? ret : gpio_pin_configure_dt(&creset, GPIO_OUTPUT_ACTIVE);
	ret = ret != 0 ? ret : gpio_pin_configure_dt(&io0, GPIO_OUTPUT_INACTIVE);
	ret = ret != 0 ? ret : gpio_pin_configure_dt(&cdone, GPIO_INPUT);
	if (ret == 0) {
		esp32_set_reset_mode(CONFIG_APP_COPROC_RESET_MODE);
	}
	return ret;
}
