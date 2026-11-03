/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The I2C target at 0x17: the register map on the bus, and what writes do (the stock firmware's
 * i2c_peripheral.c). Bytes come and go in the driver's interrupt; the effects of writes and the
 * interrupt line to the ESP32 wait in a work item until the bus is free, as there.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "coproc.h"
#include "regmap.h"

LOG_MODULE_REGISTER(i2c_regs, LOG_LEVEL_INF);

#define USER_NODE DT_PATH(zephyr_user)
#define I2C_ADDR  0x17

static const struct device *const bus = DEVICE_DT_GET(DT_NODELABEL(i2c1));
static const struct gpio_dt_spec irq_line = GPIO_DT_SPEC_GET(USER_NODE, esp32_int_gpios);

static struct regmap map;
static atomic_t bus_active;
/* The line should be low; a read of register 9 asked it released. */
static atomic_t irq_want, irq_clear;
static bool irq_is_low;
static uint8_t webusb_mode;
static uint8_t usb_state;

uint8_t regs_get(uint8_t reg)
{
	return map.regs[reg];
}

void regs_set(uint8_t reg, uint8_t val)
{
	map.regs[reg] = val;
}

uint8_t regs_webusb_mode_get(void)
{
	return webusb_mode;
}

static void apply_register(uint8_t reg, uint8_t value)
{
	switch (reg) {
	case REG_GPIO_DIR:
	case REG_GPIO_OUT:
		gpio_regs_apply(map.regs[REG_GPIO_DIR], map.regs[REG_GPIO_OUT]);
		break;
	case REG_BACKLIGHT:
		backlight_set(value);
		break;
	case REG_FPGA:
		fpga_run((value & 0x01U) != 0U);
		bridge_loopback((value & 0x02U) != 0U);
		break;
	case REG_BL_TRIGGER:
		if (value == 0xBEU) {
			sysctl_reboot_bootloader();
		}
		break;
	case REG_IR_TRIGGER:
		if (value == 0x01U) {
			uint8_t cmd = map.regs[REG_IR_COMMAND];

			ir_send(map.regs[REG_IR_ADDRESS_LO] |
					((uint16_t)map.regs[REG_IR_ADDRESS_HI] << 8),
				cmd | ((uint16_t)(uint8_t)~cmd << 8));
		}
		break;
	case REG_WS2812_MODE:
		ws2812_mode(value);
		gpio_regs_apply(map.regs[REG_GPIO_DIR], map.regs[REG_GPIO_OUT]);
		break;
	case REG_WS2812_TRIGGER: {
		uint8_t n = MIN(map.regs[REG_WS2812_LENGTH], 10U);

		for (uint8_t k = 0; k < n; k++) {
			ws2812_put(sys_get_le32(&map.regs[REG_WS2812_LED0 + 4U * k]));
		}
		break;
	}
	case COPROC_REG_RESET_MODE:
		if (value <= COPROC_RESET_CIRCUIT) {
			esp32_set_reset_mode(value);
		}
		break;
	case COPROC_REG_BOOTSEL:
		if (value == COPROC_BOOTSEL_MAGIC) {
			sysctl_reboot_bootsel();
		}
		break;
	default:
		break;
	}
}

static void apply_fn(struct k_work *work)
{
	int reg = -1;

	ARG_UNUSED(work);
	if (atomic_get(&bus_active) != 0) {
		/* The transfer's stop comes back here. */
		return;
	}
	if (atomic_clear(&irq_clear) != 0) {
		atomic_set(&irq_want, 0);
	}
	if ((atomic_get(&irq_want) != 0) != irq_is_low) {
		irq_is_low = !irq_is_low;
		(void)gpio_pin_set_dt(&irq_line, irq_is_low ? 1 : 0);
	}
	while ((reg = regmap_next_modified(&map, reg + 1)) >= 0) {
		apply_register((uint8_t)reg, map.regs[reg]);
	}
	map.regs[REG_USB] = usb_state;
	map.regs[REG_WEBUSB_MODE] = webusb_mode;
}
static K_WORK_DEFINE(apply_work, apply_fn);

void regs_input(uint8_t reg, uint8_t value, uint8_t ignore)
{
	if (regmap_update_input(&map, reg, value, ignore)) {
		atomic_set(&irq_want, 1);
		(void)k_work_submit(&apply_work);
	}
}

void regs_webusb_mode(uint8_t mode)
{
	webusb_mode = mode;
	(void)k_work_submit(&apply_work);
}

void regs_usb_state(bool mounted, bool suspended, bool remote_wakeup)
{
	usb_state = (mounted ? BIT(0) : 0U) | (suspended ? BIT(1) : 0U) |
		    (remote_wakeup ? BIT(2) : 0U);
	(void)k_work_submit(&apply_work);
}

/* ---- the bus, in the driver's interrupt ---- */

static uint8_t read_next(void)
{
	bool cleared;
	uint8_t val;

	/* CDONE is the one the ESP32 waits on: read when asked. */
	if (map.ptr == REG_INPUT1) {
		if (fpga_cdone()) {
			map.regs[REG_INPUT1] &= ~REG_INPUT1_NOT_CDONE;
		} else {
			map.regs[REG_INPUT1] |= REG_INPUT1_NOT_CDONE;
		}
	}
	val = regmap_read_byte(&map, &cleared);
	if (cleared) {
		atomic_set(&irq_clear, 1);
	}
	return val;
}

static int on_write_requested(struct i2c_target_config *cfg)
{
	ARG_UNUSED(cfg);
	atomic_set(&bus_active, 1);
	regmap_write_start(&map);
	return 0;
}

static int on_write_received(struct i2c_target_config *cfg, uint8_t val)
{
	ARG_UNUSED(cfg);
	regmap_write_byte(&map, val);
	return 0;
}

static int on_read_requested(struct i2c_target_config *cfg, uint8_t *val)
{
	ARG_UNUSED(cfg);
	atomic_set(&bus_active, 1);
	*val = read_next();
	return 0;
}

static int on_read_processed(struct i2c_target_config *cfg, uint8_t *val)
{
	ARG_UNUSED(cfg);
	*val = read_next();
	return 0;
}

static int on_stop(struct i2c_target_config *cfg)
{
	ARG_UNUSED(cfg);
	atomic_set(&bus_active, 0);
	(void)k_work_submit(&apply_work);
	return 0;
}

static const struct i2c_target_callbacks callbacks = {
	.write_requested = on_write_requested,
	.write_received = on_write_received,
	.read_requested = on_read_requested,
	.read_processed = on_read_processed,
	.stop = on_stop,
};

static struct i2c_target_config target = {
	.address = I2C_ADDR,
	.callbacks = &callbacks,
};

int regs_init(void)
{
	uint8_t id[8] = {0};

	regmap_init(&map);
	if (hwinfo_get_device_id(id, sizeof(id)) > 0) {
		memcpy(&map.regs[REG_UID], id, sizeof(id));
	}
	map.regs[COPROC_REG_FEATURES] = (uint8_t)COPROC_F_LINK;
	map.regs[REG_CRASH_DEBUG] = sysctl_crashed() ? BIT(0) : 0U;
	/* What the PWM starts at (the stock firmware's register said 0). */
	map.regs[REG_BACKLIGHT] = 255;
	return gpio_pin_configure_dt(&irq_line, GPIO_OUTPUT_INACTIVE);
}

int regs_start(void)
{
	if (!device_is_ready(bus)) {
		return -ENODEV;
	}
	return i2c_target_register(bus, &target);
}
