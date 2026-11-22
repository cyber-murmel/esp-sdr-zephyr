/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The MAC's PPS0 output on a GPIO: the pad is made an output by the GPIO driver, then the GPIO
 * matrix connects the EMAC PPS signal to it. The pulse shape is unverified (README).
 */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/ptp_clock.h>
#include <zephyr/net/ptp_time.h>
#include <zephyr/sys/util.h>

#include <esp_rom_gpio.h>
#include <soc/emac_ptp_struct.h>
#include <soc/gpio_sig_map.h>

#include "chain.h"

int chain_pps_init(void)
{
	const struct device *gpio = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	int ret;

	if (!device_is_ready(gpio)) {
		return -ENODEV;
	}
	ret = gpio_pin_configure(gpio, CONFIG_APP_CHAIN_PPS_PIN, GPIO_OUTPUT_INACTIVE);
	if (ret != 0) {
		return ret;
	}
	esp_rom_gpio_connect_out_signal(CONFIG_APP_CHAIN_PPS_PIN, EMAC_PTP_PPS_PAD_OUT_IDX, false, false);
	return 0;
}

/* PPS control register (EMAC_PTP + 0x2c, the DWMAC 3.7 PPSCTRL at 0x72c) */
#define PPS_CTRL_PPSEN0        BIT(4)      /* flexible mode: PPSCMD0 is a command */
#define PPS_CTRL_TRGT_SHIFT    5           /* TRGTMODSEL0, 2 bits */
#define PPS_TRGT_PPS_ONLY      3U          /* target time starts or stops the output, no interrupt */
#define PPS_CMD_START_TRAIN    2U
#define PPS_CMD_STOP_NOW       5U
/* The registers are 31 bits of nanoseconds and a busy flag */
#define PPS_WAIT_LOOPS         100000

/** Fixed mode: PPS0 control field (4 bits), PPSEN0 off. */
int chain_pps_set(uint32_t n)
{
	if (n > 15U) {
		return -EINVAL;
	}
	EMAC_PTP.pps_ctrl.val = n;
	return 0;
}

static int target_wait(volatile uint32_t *nsec_reg)
{
	for (int i = 0; i < PPS_WAIT_LOOPS; i++) {
		if ((*nsec_reg & BIT(31)) == 0U) {
			return 0;
		}
	}
	return -ETIMEDOUT;
}

/*
 * Is flexible mode there: select it (PPSEN0, stop command) and write a pattern to the PPS0
 * interval register. The S31 keeps only the fixed-mode field (pps_ctrl reads back 0x5) and the
 * interval reads 0: then the old control value is written back, so a fixed-mode output runs on.
 */
static int flex_probe(void)
{
	uint32_t ctrl_old = EMAC_PTP.pps_ctrl.val;

	EMAC_PTP.pps_ctrl.val = PPS_CTRL_PPSEN0 | (PPS_TRGT_PPS_ONLY << PPS_CTRL_TRGT_SHIFT) |
				PPS_CMD_STOP_NOW;
	EMAC_PTP.pps0_interval.val = 0x5a5aa5a5U;
	if ((EMAC_PTP.pps_ctrl.val & PPS_CTRL_PPSEN0) == 0U ||
	    EMAC_PTP.pps0_interval.val != 0x5a5aa5a5U) {
		EMAC_PTP.pps_ctrl.val = ctrl_old;
		return -ENODEV;
	}
	return 0;
}

/*
 * Flexible mode: a pulse train, interval 1 s, width width_us, first rising edge at the PTP second
 * two seconds on (the next full second plus one, so the registers are in before it). Interval and
 * width are programmed as ticks - 1. A tick is the sub-second increment of the PTP clock
 * (SSIR: 13 ns with FINE_SSINC at 80 MHz), or with ref_clk one cycle of the PTP reference clock
 * (12.5 ns): which unit the MAC counts in is unverified, read it off the scope.
 */
int chain_pps_flex(uint32_t width_us, bool ref_clk, struct chain_pps_info *info)
{
	const struct device *clk = DEVICE_DT_GET(DT_NODELABEL(ptp_clock));
	struct net_ptp_time t;
	uint64_t tick_ps;
	int ret;

	if (width_us == 0U) {
		width_us = 1000U;
	}
	if (ref_clk) {
		tick_ps = (uint64_t)CHAIN_PS_PER_SEC / CONFIG_APP_CHAIN_PTP_REF_HZ;
	} else {
		tick_ps = (uint64_t)chain_subsec_to_ps(EMAC_PTP.sub_sec_incre.sub_second_incre_value,
						      CHAIN_DIGITAL);
	}
	if (tick_ps == 0U) {
		return -EINVAL;
	}
	info->tick_ps = (uint32_t)tick_ps;
	info->interval_ticks = (uint32_t)(((uint64_t)CHAIN_PS_PER_SEC + tick_ps / 2U) / tick_ps);
	info->width_ticks = (uint32_t)(((uint64_t)width_us * 1000000U + tick_ps / 2U) / tick_ps);
	if (info->width_ticks == 0U || info->width_ticks >= info->interval_ticks) {
		return -EINVAL;
	}
	ret = flex_probe();
	if (ret != 0) {
		return ret;
	}
	ret = ptp_clock_get(clk, &t);
	if (ret != 0) {
		return ret;
	}
	info->start_sec = t.second + 2U;

	/* stop what runs, select flexible mode, program, then the start command */
	EMAC_PTP.pps_ctrl.val = PPS_CTRL_PPSEN0 | (PPS_TRGT_PPS_ONLY << PPS_CTRL_TRGT_SHIFT) |
				PPS_CMD_STOP_NOW;
	EMAC_PTP.pps_ctrl.val = PPS_CTRL_PPSEN0 | (PPS_TRGT_PPS_ONLY << PPS_CTRL_TRGT_SHIFT);
	EMAC_PTP.pps0_interval.val = info->interval_ticks - 1U;
	EMAC_PTP.pps0_width.val = info->width_ticks - 1U;
	EMAC_PTP.tgt_seconds.val = (uint32_t)info->start_sec;
	ret = target_wait(&EMAC_PTP.tgt_nanosec.val);
	if (ret != 0) {
		return ret;
	}
	EMAC_PTP.tgt_nanosec.val = 0U;
	ret = target_wait(&EMAC_PTP.tgt_nanosec.val);
	if (ret != 0) {
		return ret;
	}
	EMAC_PTP.pps_ctrl.val = PPS_CTRL_PPSEN0 | (PPS_TRGT_PPS_ONLY << PPS_CTRL_TRGT_SHIFT) |
				PPS_CMD_START_TRAIN;
	return 0;
}

/* Stops a flexible train; -ENODEV and nothing changed without flexible mode (the S31) */
int chain_pps_flex_stop(void)
{
	uint32_t ctrl_old = EMAC_PTP.pps_ctrl.val;

	EMAC_PTP.pps_ctrl.val = PPS_CTRL_PPSEN0 | (PPS_TRGT_PPS_ONLY << PPS_CTRL_TRGT_SHIFT) |
				PPS_CMD_STOP_NOW;
	if ((EMAC_PTP.pps_ctrl.val & PPS_CTRL_PPSEN0) == 0U) {
		EMAC_PTP.pps_ctrl.val = ctrl_old;
		return -ENODEV;
	}
	return 0;
}
