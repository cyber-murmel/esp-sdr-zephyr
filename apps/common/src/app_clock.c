/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The ESP32-P4's CPU runs from the CPLL: 90/180/360 MHz before silicon v3.0, 100/200/400 MHz
 * from v3.0. The families are separate builds, not a choice at run time: the HAL compiles one
 * clock table and one register layout (CONFIG_ESP32P4_SELECTS_REV_LESS_V3, set when
 * SOC_ESP32P4_REV_MIN_FULL < 300), soc.c asserts the devicetree frequency is in the family's
 * set, and an image for one family does not run on the other. A pre-v3 image cannot reach
 * 400 MHz (rtc_clk_cpu_freq_mhz_to_config() refuses it); a v3 image gets its devicetree
 * frequency from the clock driver in PRE_KERNEL_1, 400 MHz unless the board chose less. This
 * only lifts the latter.
 *
 * Switching at run time is safe on one CPU: the kernel clock (systimer: ticks, k_cycle_get,
 * k_busy_wait, SYS_CLOCK_HW_CYCLES_PER_SEC = 16 MHz) counts the XTAL; APB_CLK stays 100 MHz at
 * every CPLL divider and MEM_CLK at most 200; the UARTs, SDMMC, flash (SPLL) and PSRAM (MPLL)
 * have their own clocks; esp_rom_delay_us() and esp_clk_cpu_freq() follow the ticks per us
 * the HAL updates; the driver rescales mcycle. Only a CPLL relock (from XTAL or RC_FAST, never
 * between two v3 settings) parks the buses at 40 MHz while it calibrates. The driver also sets
 * the console UART back to CONFIG_ESP_CONSOLE_UART_BAUDRATE.
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/esp32_clock_control.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_SOC_SERIES_ESP32P4)
#include <hal/efuse_hal.h>
#endif

#include "app_clock.h"

LOG_MODULE_REGISTER(app_clock, LOG_LEVEL_INF);

#define CPU_SUBSYS ((clock_control_subsys_t)(uintptr_t)ESP32_CLOCK_CONTROL_SUBSYS_CPU)

/* Silicon revision as efuse_hal_chip_revision() counts it: major * 100 + minor. */
#define P4_REV_400 300U
#define P4_MHZ_MAX 400

static int cpu_mhz(const struct device *clk)
{
	uint32_t hz;
	int ret = clock_control_get_rate(clk, CPU_SUBSYS, &hz);

	return ret != 0 ? ret : (int)(hz / MHZ(1));
}

int app_clock_cpu_max(void)
{
	const struct device *clk = DEVICE_DT_GET(DT_NODELABEL(clock));
	int mhz;

	if (!device_is_ready(clk)) {
		return -ENODEV;
	}
	mhz = cpu_mhz(clk);
#if defined(CONFIG_SOC_SERIES_ESP32P4)
	if (CONFIG_SOC_ESP32P4_REV_MIN_FULL >= P4_REV_400 && mhz > 0 && mhz < P4_MHZ_MAX &&
	    efuse_hal_chip_revision() >= P4_REV_400) {
		struct esp32_clock_config cfg = {
			.cpu = {
				.clk_src = ESP32_CPU_CLK_SRC_PLL,
				.cpu_freq = P4_MHZ_MAX,
				.xtal_freq = DT_PROP(DT_NODELABEL(cpu0), xtal_freq) / MHZ(1),
			},
		};
		int ret;

		/* The driver locks interrupts on this CPU only; ESP-IDF stalls the other one. */
		if (arch_num_cpus() > 1) {
			LOG_WRN("CPU stays at %d MHz: a second CPU runs", mhz);
			return mhz;
		}
		ret = clock_control_configure(clk, CPU_SUBSYS, &cfg);
		if (ret != 0) {
			LOG_ERR("CPU to %d MHz: %d", P4_MHZ_MAX, ret);
		}
		mhz = cpu_mhz(clk);
		LOG_INF("CPU at %d MHz", mhz);
	}
#endif
	return mhz;
}
