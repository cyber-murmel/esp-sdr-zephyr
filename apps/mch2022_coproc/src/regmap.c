/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <esp_sdr/mch2022_coproc.h>

#include "regmap.h"

bool regmap_read_only(uint8_t reg)
{
	switch (reg) {
	case REG_FW_VER:
	case REG_GPIO_IN:
	case REG_INPUT1:
	case REG_INPUT2:
	case REG_INTERRUPT1:
	case REG_INTERRUPT2:
	case REG_ADC_VUSB:
	case REG_ADC_VUSB + 1:
	case REG_ADC_VBAT:
	case REG_ADC_VBAT + 1:
	case REG_USB:
	case REG_CRASH_DEBUG:
	case REG_CHARGING:
	case REG_TEMP:
	case REG_TEMP + 1:
	case COPROC_REG_ID:
	case COPROC_REG_API_MAJOR:
	case COPROC_REG_API_MINOR:
	case COPROC_REG_FEATURES:
	case COPROC_REG_LINK_STATE:
	case COPROC_REG_UART0_BAUD:
	case COPROC_REG_UART0_BAUD + 1:
	case COPROC_REG_UART0_BAUD + 2:
		return true;
	default:
		return reg >= REG_UID && reg < REG_UID + 8;
	}
}

void regmap_init(struct regmap *m)
{
	memset(m, 0, sizeof(*m));
	m->regs[REG_FW_VER] = REGMAP_FW_VERSION;
	m->regs[COPROC_REG_ID] = COPROC_ID;
	m->regs[COPROC_REG_API_MAJOR] = COPROC_API_MAJOR;
	m->regs[COPROC_REG_API_MINOR] = COPROC_API_MINOR;
}

void regmap_write_start(struct regmap *m)
{
	m->address_next = true;
}

void regmap_write_byte(struct regmap *m, uint8_t val)
{
	if (m->address_next) {
		m->ptr = val;
		m->address_next = false;
		return;
	}
	if (!regmap_read_only(m->ptr)) {
		m->regs[m->ptr] = val;
		atomic_set_bit(m->modified, m->ptr);
	}
	m->ptr++;
}

uint8_t regmap_read_byte(struct regmap *m, bool *cleared)
{
	uint8_t val = m->regs[m->ptr];

	*cleared = m->ptr == REG_INTERRUPT2;
	if (*cleared) {
		m->regs[REG_INTERRUPT1] = 0;
		m->regs[REG_INTERRUPT2] = 0;
	}
	m->ptr++;
	return val;
}

int regmap_next_modified(struct regmap *m, unsigned int from)
{
	for (unsigned int reg = from; reg < ARRAY_SIZE(m->regs); reg++) {
		if (atomic_test_and_clear_bit(m->modified, reg)) {
			return (int)reg;
		}
	}
	return -1;
}

bool regmap_update_input(struct regmap *m, uint8_t reg, uint8_t value, uint8_t ignore)
{
	const uint8_t irq_reg = reg == REG_INPUT1 ? REG_INTERRUPT1 : REG_INTERRUPT2;
	unsigned int key = irq_lock();
	uint8_t changed = (m->regs[reg] ^ value) & ~ignore;

	m->regs[irq_reg] |= changed;
	m->regs[reg] = value;
	irq_unlock(key);
	return changed != 0U;
}
