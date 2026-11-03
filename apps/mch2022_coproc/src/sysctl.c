/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * How a run begins and ends. The badge's bootloader is what starts this image, and looks at the
 * watchdog's scratch registers for its magic (it does not clear it), so they are read and cleared
 * here before anything else can reboot; a crash leaves a magic of its own there, which register
 * 18 reports, as in the stock firmware.
 */

#include <zephyr/fatal.h>
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/reboot.h>

#include <hardware/gpio.h>
#include <hardware/structs/psm.h>
#include <hardware/structs/rosc.h>
#include <hardware/structs/watchdog.h>
#include <pico/bootrom.h>

#include "coproc.h"

LOG_MODULE_REGISTER(sysctl, LOG_LEVEL_INF);

/* The stock firmware's. */
#define BOOTLOADER_MAGIC 0xB105F00DU
#define CRASH_MAGIC      0xFA174200U

#define PIN_ESP32_EN   13U
#define PIN_FPGA_RESET 21U

static bool crashed;
static uint32_t reset_reason, rosc_status;

/*
 * The clock's start reset the pins: the ESP32 is on its pull-up, about to be held while the I2C
 * target comes up. The FPGA stays in reset, on its pull-down, until the ESP32 says.
 */
static int early_init(void)
{
	gpio_init(PIN_ESP32_EN);
	gpio_put(PIN_ESP32_EN, false);
	gpio_set_dir(PIN_ESP32_EN, true);
	gpio_init(PIN_FPGA_RESET);
	gpio_put(PIN_FPGA_RESET, false);
	gpio_set_dir(PIN_FPGA_RESET, true);

	crashed = watchdog_hw->scratch[5] == CRASH_MAGIC && watchdog_hw->scratch[6] == ~CRASH_MAGIC;
	reset_reason = watchdog_hw->reason;
	rosc_status = rosc_hw->status;
	watchdog_hw->scratch[5] = 0;
	watchdog_hw->scratch[6] = 0;
	return 0;
}
SYS_INIT(early_init, PRE_KERNEL_2, 0);

bool sysctl_crashed(void)
{
	return crashed;
}

uint32_t sysctl_reset_reason(void)
{
	return reset_reason;
}

uint32_t sysctl_rosc_status(void)
{
	return rosc_status;
}

/* A watchdog reset of all but the oscillators; the scratch registers survive it. */
static FUNC_NORETURN void reboot_with(uint32_t magic)
{
	(void)irq_lock();
	watchdog_hw->ctrl &= ~WATCHDOG_CTRL_ENABLE_BITS;
	watchdog_hw->scratch[4] = 0;
	watchdog_hw->scratch[5] = magic;
	watchdog_hw->scratch[6] = ~magic;
	psm_hw->wdsel = PSM_WDSEL_BITS & ~(PSM_WDSEL_ROSC_BITS | PSM_WDSEL_XOSC_BITS);
	watchdog_hw->ctrl |= WATCHDOG_CTRL_TRIGGER_BITS;
	for (;;) {
	}
}

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(esf);
	LOG_PANIC();
	LOG_ERR("fatal error %u", reason);
	reboot_with(CRASH_MAGIC);
}

static void bootloader_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	reboot_with(BOOTLOADER_MAGIC);
}
static K_WORK_DELAYABLE_DEFINE(bootloader_work, bootloader_fn);

static void bootsel_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)irq_lock();
	reset_usb_boot(0, 0);
}
static K_WORK_DELAYABLE_DEFINE(bootsel_work, bootsel_fn);

/* A moment later: a USB request's status stage still goes out. */
void sysctl_reboot_bootloader(void)
{
	(void)k_work_schedule(&bootloader_work, K_MSEC(20));
}

void sysctl_reboot_bootsel(void)
{
	(void)k_work_schedule(&bootsel_work, K_MSEC(20));
}
