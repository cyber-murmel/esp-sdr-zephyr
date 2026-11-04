/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The gapless receiver's threshold servo, its decision alone: what one period's bytes say about
 * the threshold. Plain C, no Zephyr: tests/unit/gapless_servo runs it on native_sim. 32 bits and
 * no division: it runs in the receive loop, where a libgcc call would overrun the deadline.
 */

#ifndef ESP_SDR_GAPLESS_SERVO_H_
#define ESP_SDR_GAPLESS_SERVO_H_

#include <stdint.h>

enum thr_step {
	THR_STEP_HOLD,
	THR_STEP_UP,
	THR_STEP_UP_FAST,
	THR_STEP_DOWN,
};

/*
 * One period's step. Bytes, each at most UINT32_MAX / 100: @p kept what the windows kept came to,
 * @p sent that less what width records saved, @p lost what the CPU abandoned, @p budget what the
 * link carries in a period. @p lo and @p hi: the link's headroom in percent, what it must keep
 * (past it, up) and what makes the threshold needlessly high (below it, down). @p lost_pct: the
 * share of what is kept that may be lost before the threshold rises.
 */
static inline __attribute__((always_inline)) enum thr_step
thr_servo_step(uint32_t kept, uint32_t sent, uint32_t lost, uint32_t budget, uint32_t lo,
	       uint32_t hi, uint32_t lost_pct)
{
	if (lost * 100U > kept * lost_pct || sent > budget) {
		return THR_STEP_UP_FAST;
	}
	if (sent * 100U > budget * (100U - lo)) {
		return THR_STEP_UP;
	}
	if (sent * 100U < budget * (100U - hi)) {
		return THR_STEP_DOWN;
	}
	return THR_STEP_HOLD;
}

#endif /* ESP_SDR_GAPLESS_SERVO_H_ */
