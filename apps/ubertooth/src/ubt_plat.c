/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See ubt_plat.h.
 */

#include <zephyr/app_version.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/version.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include <esp_attr.h>

#include "app_usb.h"
#include "ubt_mode.h"
#include "ubt_plat.h"
#include "ubt_usb.h"

LOG_MODULE_REGISTER(ubt_plat, LOG_LEVEL_INF);

/* Cycles per 100 ns: the CPU counter runs at the core clock. */
#define CYCLES_PER_100NS ((uint32_t)(CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / 10000000U))

/* GET_PARTNUM has no LPC1758 to report: "ES33". */
#define PART_NUM 0x45533333U

struct ubt_dev ubt;
/* The packet slots are only touched under the queue indices' ordering: PSRAM is fine. */
static EXT_RAM_BSS_ATTR usb_pkt_rx queue_slots[UBT_QUEUE_SLOTS];

#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#endif

static void plat_led(unsigned int which, bool on)
{
	/* The board has one LED: USR drives it, RX and TX are only remembered. */
#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)
	if (which == 0U && gpio_is_ready_dt(&led)) {
		(void)gpio_pin_set_dt(&led, on ? 1 : 0);
	}
#else
	ARG_UNUSED(which);
	ARG_UNUSED(on);
#endif
}

static void plat_kick(void)
{
	ubt_mode_kick();
}

static bool plat_reset(bool dfu)
{
	if (dfu) {
		return app_usb_dfu_enter();
	}
	sys_reboot(SYS_REBOOT_COLD);
	return true;
}

/* 10 zero bytes, then the chip's 6 byte id: the host prints the 16 bytes as 32 hex digits. */
static void plat_serial(uint8_t serial[16])
{
	uint8_t id[8] = {0};
	ssize_t n = hwinfo_get_device_id(id, sizeof(id));

	memset(serial, 0, 16);
	if (n > 6) {
		n = 6;
	}
	if (n > 0) {
		memcpy(&serial[16 - n], id, (size_t)n);
	}
}

static uint64_t plat_cycles(void)
{
	return k_cycle_get_64();
}

static const struct ubt_platform plat = {
	.led = plat_led,
	.kick = plat_kick,
	.reset = plat_reset,
	.serial = plat_serial,
	.cycles = plat_cycles,
	.rev = "esp-sdr " APP_VERSION_STRING,
	.compile_info = "ubertooth esp-sdr " APP_VERSION_STRING " (zephyr " KERNEL_VERSION_STRING ")",
	.part_num = PART_NUM,
};

int ubt_init(void)
{
#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)
	if (gpio_is_ready_dt(&led)) {
		(void)gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	}
#endif
	ubt.plat = &plat;
	ubt_clock_init(&ubt.clock, CYCLES_PER_100NS, k_cycle_get_64());
	ubt_queue_init(&ubt.queue, queue_slots);
	ubt_regs_init(&ubt.regs);
	ubt.le.access_address = 0x8e89bed6U;
	/* Upstream's defaults: the advertising data {flags} and PA level 3 (FREND 0b1011). */
	ubt.adv_data[0] = 0x02;
	ubt.adv_data[1] = 0x01;
	ubt.adv_data[2] = 0x05;
	ubt.adv_len = 3;
	ubt.pa_level = 3;
	ubt_dev_reset(&ubt);
	ubt_usb_init(&ubt);
	ubt_mode_init(&ubt);
	return 0;
}
