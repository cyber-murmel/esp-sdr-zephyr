/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The buttons (every 30 ms), the ADC and the charger (every 250 ms), the backlight, the SAO port's
 * pins: their registers, as the stock firmware's i2c_peripheral.c and lcd.c.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <hardware/adc.h>

#include "coproc.h"
#include "regmap.h"

LOG_MODULE_REGISTER(periph, LOG_LEVEL_INF);

#define USER_NODE DT_PATH(zephyr_user)
#define INPUT_MS  30
#define ADC_MS    250

#define SPEC_ELEM(node, prop, idx) GPIO_DT_SPEC_GET_BY_IDX(node, prop, idx),

static const struct gpio_dt_spec input1[] = {
	DT_FOREACH_PROP_ELEM(USER_NODE, input1_gpios, SPEC_ELEM)
};
static const struct gpio_dt_spec input2[] = {
	DT_FOREACH_PROP_ELEM(USER_NODE, input2_gpios, SPEC_ELEM)
};
static const struct gpio_dt_spec sao[] = {
	DT_FOREACH_PROP_ELEM(USER_NODE, sao_gpios, SPEC_ELEM)
};
static const struct gpio_dt_spec charging = GPIO_DT_SPEC_GET(USER_NODE, charging_gpios);
static const struct pwm_dt_spec backlight = PWM_DT_SPEC_GET(USER_NODE);
static const struct adc_dt_spec adc_ch[] = {
	ADC_DT_SPEC_GET_BY_IDX(USER_NODE, 0),
	ADC_DT_SPEC_GET_BY_IDX(USER_NODE, 1),
	ADC_DT_SPEC_GET_BY_IDX(USER_NODE, 2),
};
static const uint8_t adc_reg[] = {REG_ADC_VUSB, REG_ADC_VBAT, REG_TEMP};

/* Bit 5 of INPUT1 is CDONE, bit 7 SELECT; the pins above fill the others from bit 0. */
#define INPUT1_SELECT BIT(7)

static uint8_t read_pins(const struct gpio_dt_spec *pins, size_t n)
{
	uint8_t val = 0;

	for (size_t k = 0; k < n; k++) {
		if (gpio_pin_get_dt(&pins[k]) > 0) {
			val |= BIT(k);
		}
	}
	return val;
}

static void input_fn(struct k_work *work)
{
	struct k_work_delayable *dw = k_work_delayable_from_work(work);
	uint8_t v1 = read_pins(input1, ARRAY_SIZE(input1));
	uint8_t in = 0;

	if (select_pressed()) {
		v1 |= INPUT1_SELECT;
	}
	if (!fpga_cdone()) {
		v1 |= REG_INPUT1_NOT_CDONE;
	}
	/* CDONE raises no interrupt in the stock firmware either: it reads it apart. */
	regs_input(REG_INPUT1, v1, REG_INPUT1_NOT_CDONE);
	regs_input(REG_INPUT2, read_pins(input2, ARRAY_SIZE(input2)), 0);

	ARRAY_FOR_EACH(sao, k) {
		if (gpio_pin_get_dt(&sao[k]) > 0) {
			in |= BIT(k);
		}
	}
	regs_set(REG_GPIO_IN, in);
	(void)k_work_reschedule(dw, K_MSEC(INPUT_MS));
}
static K_WORK_DELAYABLE_DEFINE(input_work, input_fn);

static void adc_fn(struct k_work *work)
{
	static unsigned int next;
	struct k_work_delayable *dw = k_work_delayable_from_work(work);
	int16_t raw = 0;
	struct adc_sequence seq = {
		.buffer = &raw,
		.buffer_size = sizeof(raw),
	};

	regs_set(REG_CHARGING, gpio_pin_get_dt(&charging) > 0 ? 1U : 0U);
	if (adc_sequence_init_dt(&adc_ch[next], &seq) == 0 && adc_read_dt(&adc_ch[next], &seq) == 0) {
		regs_set(adc_reg[next], (uint8_t)raw);
		regs_set(adc_reg[next] + 1, (uint8_t)((uint16_t)raw >> 8));
	}
	next = (next + 1U) % ARRAY_SIZE(adc_ch);
	(void)k_work_reschedule(dw, K_MSEC(ADC_MS));
}
static K_WORK_DELAYABLE_DEFINE(adc_work, adc_fn);

void backlight_set(uint8_t value)
{
	(void)pwm_set_pulse_dt(&backlight, (uint32_t)((uint64_t)backlight.period * value / 255U));
}

void gpio_regs_apply(uint8_t dir, uint8_t out)
{
	ARRAY_FOR_EACH(sao, k) {
		gpio_flags_t flags = GPIO_INPUT;

		/* SAO IO0 is the PIO's while WS2812 is on. */
		if (k == 0U && regs_get(REG_WS2812_MODE) != 0U) {
			continue;
		}
		if ((dir & BIT(k)) != 0U) {
			flags = (out & BIT(k)) != 0U ? GPIO_OUTPUT_HIGH : GPIO_OUTPUT_LOW;
		}
		(void)gpio_pin_configure_dt(&sao[k], flags);
	}
}

int periph_init(void)
{
	int ret = 0;

	ARRAY_FOR_EACH(input1, k) {
		ret = ret != 0 ? ret : gpio_pin_configure_dt(&input1[k], GPIO_INPUT);
	}
	ARRAY_FOR_EACH(input2, k) {
		ret = ret != 0 ? ret : gpio_pin_configure_dt(&input2[k], GPIO_INPUT);
	}
	ret = ret != 0 ? ret : gpio_pin_configure_dt(&charging, GPIO_INPUT);
	if (ret != 0) {
		return ret;
	}
	gpio_regs_apply(0, 0);
	if (!pwm_is_ready_dt(&backlight)) {
		return -ENODEV;
	}
	backlight_set(255);
	ARRAY_FOR_EACH(adc_ch, k) {
		ret = adc_channel_setup_dt(&adc_ch[k]);
		if (ret != 0) {
			return ret;
		}
	}
	adc_set_temp_sensor_enabled(true);
	(void)k_work_schedule(&input_work, K_NO_WAIT);
	(void)k_work_schedule(&adc_work, K_NO_WAIT);
	return 0;
}
