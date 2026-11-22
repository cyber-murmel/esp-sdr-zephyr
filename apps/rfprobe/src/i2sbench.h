/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * "probe i2s": I2S0 TX at a bit clock divider of 1 and a GPIO-matrix inverted copy of the data
 * pad, measured with PCNT reading the pads back (ESP32-S3).
 */

#ifndef I2SBENCH_H_
#define I2SBENCH_H_

#include <stdint.h>

#include <zephyr/shell/shell.h>

/* clk_mhz 160, 240 or 40 (XTAL); slot_bits 1..32. Returns 0 or -errno. */
int i2sbench_run(const struct shell *sh, uint32_t bck_div, uint32_t pattern, uint32_t slot_bits,
		 uint32_t clk_mhz);

#endif
