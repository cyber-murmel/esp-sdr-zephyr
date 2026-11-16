/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Bluetooth clock of the device (upstream ubertooth_clock.c): CLKN counts
 * 312.5 us ticks, CLK100NS counts 100 ns units inside 2^20 ticks. One 64 bit
 * timeline in 100 ns units derived from the CPU cycle counter feeds both, so
 * the timestamps stay monotonic across retunes and mode changes; SET_CLOCK,
 * START_HOPPING, TRIM_CLOCK and FIX_CLOCK_DRIFT only move its offset and slope.
 *
 * The caller supplies the cycle count (k_cycle_get_64(), 240 MHz: 24 cycles
 * per 100 ns), so the code runs on the host unchanged. Setters rebase the
 * timeline; call ubt_clock_rebase() at least once a minute so the scaling of
 * the elapsed cycles cannot overflow.
 */

#ifndef UBT_CLOCK_H_
#define UBT_CLOCK_H_

#include <stdint.h>

struct ubt_clock {
	uint32_t cyc_per_100ns;
	uint64_t base_cyc;
	uint64_t base_t100;
	/* Sum of the FIX_CLOCK_DRIFT requests: the local clock runs this many ppm too fast (+) or slow (-). */
	int32_t drift_ppm;
};

void ubt_clock_init(struct ubt_clock *c, uint32_t cyc_per_100ns, uint64_t now_cyc);

/* The timeline in 100 ns units at cycle count @p cyc. */
uint64_t ubt_clock_t100(const struct ubt_clock *c, uint64_t cyc);

/* The cycle count at which the timeline reaches @p t100 (for timers). */
uint64_t ubt_clock_cyc_at(const struct ubt_clock *c, uint64_t t100);

void ubt_clock_rebase(struct ubt_clock *c, uint64_t now_cyc);

/* CLKN (3.2 kHz ticks, 32 bit as the CC2400 firmware keeps it). */
uint32_t ubt_clock_clkn(const struct ubt_clock *c, uint64_t cyc);

/* The packet fields for a time: clkn_high = (CLKN >> 20) & 0xff, clk100ns = 3125 * (CLKN & 0xfffff) + sub tick. */
void ubt_clock_split(uint64_t t100, uint8_t *clkn_high, uint32_t *clk100ns);

/* SET_CLOCK: CLKN becomes @p clkn, the sub tick phase stays. */
void ubt_clock_set_clkn(struct ubt_clock *c, uint64_t now_cyc, uint32_t clkn);

/*
 * START_HOPPING: @p clkn_offset ticks are added, then the TRIM_CLOCK phase
 * shift below. TRIM_CLOCK: the clock ticks are delayed by @p offset 100 ns
 * units (0 to 3124), or advanced by 6250 - @p offset (3125 to 6249), as the
 * timer reload does upstream.
 */
void ubt_clock_skew(struct ubt_clock *c, uint64_t now_cyc, uint32_t clkn_offset, uint16_t offset);
void ubt_clock_trim(struct ubt_clock *c, uint64_t now_cyc, uint16_t offset);

/* FIX_CLOCK_DRIFT: add @p ppm to the drift sum (int16 like upstream, saturating). */
void ubt_clock_fix_drift(struct ubt_clock *c, uint64_t now_cyc, int16_t ppm);

#endif /* UBT_CLOCK_H_ */
