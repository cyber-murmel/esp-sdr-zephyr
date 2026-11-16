/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APP_CLOCK_H_
#define APP_CLOCK_H_

/*
 * Raise the CPU clock to 400 MHz on an ESP32-P4 of silicon revision v3.0 or later, in a build
 * for that family (CONFIG_SOC_ESP32P4_REV_MIN_FULL >= 300) whose devicetree chose less. Pre-v3
 * builds (90, 180 or 360 MHz; their HAL has no 400 MHz setting) and other chips keep their
 * clock. Single CPU only: with a second CPU running it leaves the clock as it is.
 *
 * Call from a thread early in boot, before SDIO, USB or the display start: the switch runs
 * with interrupts locked. Timing API spans (mcycle) across the call are meaningless.
 *
 * Returns the CPU clock in MHz afterwards (unchanged and logged if the switch failed), or a
 * negative errno if it cannot be read (-ENODEV: the clock driver is not ready).
 */
int app_clock_cpu_max(void);

#endif /* APP_CLOCK_H_ */
