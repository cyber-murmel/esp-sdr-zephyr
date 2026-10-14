/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See ubt_clock.h.
 */

#include "ubt_clock.h"
#include "ubt_proto.h"

#define PPM 1000000LL

void ubt_clock_init(struct ubt_clock *c, uint32_t cyc_per_100ns, uint64_t now_cyc)
{
	c->cyc_per_100ns = cyc_per_100ns;
	c->base_cyc = now_cyc;
	c->base_t100 = 0;
	c->drift_ppm = 0;
}

uint64_t ubt_clock_t100(const struct ubt_clock *c, uint64_t cyc)
{
	int64_t units = (int64_t)((cyc - c->base_cyc) / c->cyc_per_100ns);

	/* A clock that runs too fast is slowed down by its drift, one that is too slow sped up. */
	units -= units * c->drift_ppm / PPM;
	return c->base_t100 + (uint64_t)units;
}

uint64_t ubt_clock_cyc_at(const struct ubt_clock *c, uint64_t t100)
{
	int64_t units = (int64_t)(t100 - c->base_t100);

	units = units * PPM / (PPM - c->drift_ppm);
	return c->base_cyc + (uint64_t)units * c->cyc_per_100ns;
}

void ubt_clock_rebase(struct ubt_clock *c, uint64_t now_cyc)
{
	c->base_t100 = ubt_clock_t100(c, now_cyc);
	c->base_cyc = now_cyc;
}

uint32_t ubt_clock_clkn(const struct ubt_clock *c, uint64_t cyc)
{
	return (uint32_t)(ubt_clock_t100(c, cyc) / UBT_CLK100NS_PER_CLKN);
}

void ubt_clock_split(uint64_t t100, uint8_t *clkn_high, uint32_t *clk100ns)
{
	uint32_t clkn = (uint32_t)(t100 / UBT_CLK100NS_PER_CLKN);
	uint32_t sub = (uint32_t)(t100 % UBT_CLK100NS_PER_CLKN);

	*clkn_high = (uint8_t)((clkn >> 20) & 0xffU);
	*clk100ns = UBT_CLK100NS_PER_CLKN * (clkn & 0xfffffU) + sub;
}

void ubt_clock_set_clkn(struct ubt_clock *c, uint64_t now_cyc, uint32_t clkn)
{
	uint64_t t = ubt_clock_t100(c, now_cyc);

	c->base_cyc = now_cyc;
	c->base_t100 = (uint64_t)UBT_CLK100NS_PER_CLKN * clkn + t % UBT_CLK100NS_PER_CLKN;
}

void ubt_clock_trim(struct ubt_clock *c, uint64_t now_cyc, uint16_t offset)
{
	int32_t delay = offset <= 3124U ? (int32_t)offset : (int32_t)offset - 6250;

	ubt_clock_rebase(c, now_cyc);
	c->base_t100 -= (uint64_t)(int64_t)delay;
}

void ubt_clock_skew(struct ubt_clock *c, uint64_t now_cyc, uint32_t clkn_offset, uint16_t offset)
{
	ubt_clock_rebase(c, now_cyc);
	c->base_t100 += (uint64_t)UBT_CLK100NS_PER_CLKN * clkn_offset;
	ubt_clock_trim(c, now_cyc, offset);
}

void ubt_clock_fix_drift(struct ubt_clock *c, uint64_t now_cyc, int16_t ppm)
{
	int32_t sum = c->drift_ppm + ppm;

	ubt_clock_rebase(c, now_cyc);
	if (sum > INT16_MAX) {
		sum = INT16_MAX;
	} else if (sum < INT16_MIN) {
		sum = INT16_MIN;
	}
	c->drift_ppm = sum;
}
