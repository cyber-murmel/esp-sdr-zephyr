/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See ubt_regs.h.
 */

#include <string.h>

#include "ubt_regs.h"

void ubt_regs_init(struct ubt_regs *g)
{
	memset(g->r, 0, sizeof(g->r));
	/* What upstream's cc2400_init() leaves programmed that the host tools may look at. */
	g->r[UBT_REG_MDMCTRL] = 0x0029; /* 160 kHz deviation */
	g->r[UBT_REG_FREND] = 0x000b;
	g->r[UBT_REG_FSDIV] = 2441U - 1U;
}

uint16_t ubt_regs_read(const struct ubt_regs *g, uint8_t reg)
{
	return reg <= UBT_REG_MAX ? g->r[reg] : 0;
}

void ubt_regs_write(struct ubt_regs *g, uint8_t reg, uint16_t val)
{
	if (reg > UBT_REG_MAX) {
		return;
	}
	g->r[reg] = val;
	if (g->on_write != NULL) {
		g->on_write(g->ctx, reg, val);
	}
}

size_t ubt_regs_dump(const struct ubt_regs *g, uint8_t *buf, size_t max)
{
	size_t n = 0;

	/* Upstream writes 0x2e records but reports 0x2d: keep the host's view. */
	for (unsigned int i = 0; i < UBT_REG_MAX && n + 3U <= max; i++) {
		buf[n++] = (uint8_t)i;
		buf[n++] = (uint8_t)(g->r[i] >> 8);
		buf[n++] = (uint8_t)g->r[i];
	}
	return n;
}
