/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The I2C register map as the ESP32 sees it (the stock firmware's i2c_peripheral.c): 256 bytes,
 * the first byte of a write sets the address, which goes on by itself and wraps. Plain logic,
 * tests/unit/mch2022_coproc runs it on native_sim; i2c_regs.c ties it to the bus.
 */

#ifndef REGMAP_H_
#define REGMAP_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/sys/atomic.h>

#define REG_FW_VER          0
#define REG_GPIO_DIR        1
#define REG_GPIO_IN         2
#define REG_GPIO_OUT        3
#define REG_BACKLIGHT       4
#define REG_FPGA            5
#define REG_INPUT1          6
#define REG_INPUT2          7
#define REG_INTERRUPT1      8
#define REG_INTERRUPT2      9
#define REG_ADC_TRIGGER     10
#define REG_ADC_VUSB        11
#define REG_ADC_VBAT        13
#define REG_USB             15
#define REG_BL_TRIGGER      16
#define REG_WEBUSB_MODE     17
#define REG_CRASH_DEBUG     18
#define REG_RESET_LOCK      19
#define REG_RESET_ATTEMPTED 20
#define REG_CHARGING        21
#define REG_TEMP            22
#define REG_UID             24
#define REG_IR_ADDRESS_LO   96
#define REG_IR_ADDRESS_HI   97
#define REG_IR_COMMAND      98
#define REG_IR_TRIGGER      99
#define REG_WS2812_MODE     104
#define REG_WS2812_TRIGGER  105
#define REG_WS2812_LENGTH   106
#define REG_WS2812_LED0     108

/* The stock firmware: the first version the launcher accepts, and no MSC registers. */
#define REGMAP_FW_VERSION 0x0CU

/* Register 6, bit 5: the FPGA's CDONE, inverted. */
#define REG_INPUT1_NOT_CDONE 0x20U

struct regmap {
	uint8_t regs[256];
	ATOMIC_DEFINE(modified, 256);
	/* The next byte read or written. */
	uint8_t ptr;
	/* The next byte written sets the address. */
	bool address_next;
};

void regmap_init(struct regmap *m);
bool regmap_read_only(uint8_t reg);

/* A write transfer started: its first byte is an address. */
void regmap_write_start(struct regmap *m);
void regmap_write_byte(struct regmap *m, uint8_t val);
/* The byte at the address, which moves on. @p cleared: this read cleared the interrupt bits. */
uint8_t regmap_read_byte(struct regmap *m, bool *cleared);

/*
 * The next register written since the last call, from @p from up, its mark taken; -1 if none.
 * Ascending, as the stock firmware applies them.
 */
int regmap_next_modified(struct regmap *m, unsigned int from);

/*
 * A new value for an input register (REG_INPUT1 or REG_INPUT2): the changed bits accumulate in
 * its interrupt register; true if any bit outside @p ignore changed. Interrupt safe.
 */
bool regmap_update_input(struct regmap *m, uint8_t reg, uint8_t value, uint8_t ignore);

#endif /* REGMAP_H_ */
