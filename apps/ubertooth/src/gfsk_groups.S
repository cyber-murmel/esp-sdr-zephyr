/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The GFSK demodulator's per group logic (gfsk_demod.c, groups_c()) in assembly, bit for bit:
 * for each group of 4 samples the full path completed, the smoothed power, every fourth group
 * its pwr value, the detector with the tracked floor (no squelch) or the fixed gate, the burst
 * edge (two groups above the gate after a pause or a step up: a new acquisition, the cursor
 * moved up to it; a step up inside a run starts the run over) and the squelch hold. The C keeps every field in memory around the byte
 * store of the power ring; here the loop's state stays in registers, the configuration in a
 * register of flags, and the groups run as a zero-overhead loop whose common path in the modes
 * (a squelch, a group below or above the gate) falls through.
 *
 * int gfsk_groups_fast(struct gfsk_demod *d);
 *   From d->lg while d->f - lg >= 4; returns 1 when the squelch closes (lg is then the end of
 *   the closing group), 0 otherwise. Every field it changes is written back to d. The offsets
 *   are gfsk_demod_off.h's; the parameters the reference's (SQ_SHIFT 2, FLOOR_UP 12,
 *   FLOOR_MUL 6, IDLE_RESET 8, LEAD_IN 8, PWR_COMP_BLE 1, PWR_COMP_BR 0, GFSK_PRING 32).
 *
 * The pwr value is gfsk_pwr_from_s() plus the compensation: log2 in Q8 from the leading one
 * and the six bits after it (gfsk_log2_lut), 1541 l + 1697548 >> 16. It never exceeds 218, so
 * neither of the C's clamps to 255 can act.
 *
 * Registers: a2 d, a3 lg, a4 flags (bit 0 s_init, 1 a squelch (pwr_thr), 2 always, 3 the BLE
 * compensation), a5 savg, a6 floor, a7 gate, a8 run, a9 off, a10 pw_c; a11..a15 scratch (a12
 * the group's index in the rings, a13 its power p through the group). The smoothed power
 * before the group sits at a1 + 0.
 */

#include <esp_sdr/esp_sdr_place.h>

#include "gfsk_demod_off.h"

	ESP_SDR_ASM_SECTION(SAMPLE, gfsk_groups)
	.align	4
	.global	gfsk_groups_fast
	.type	gfsk_groups_fast, @function
gfsk_groups_fast:
	entry	a1, 32
	l32i	a3, a2, GD_LG
	l32i	a5, a2, GD_SAVG
	l32i	a6, a2, GD_FLOOR
	l32i	a7, a2, GD_S_GATE
	l8ui	a8, a2, GD_RUN
	l8ui	a9, a2, GD_OFF
	l8ui	a10, a2, GD_PW_C
	/* The flags. */
	l8ui	a4, a2, GD_S_INIT
	movi	a11, 0
	movi	a12, 1
	movnez	a4, a12, a4		/* bit 0 */
	l8ui	a13, a2, GD_PWR_THR
	movi	a14, 2
	movnez	a11, a14, a13
	or	a4, a4, a11		/* bit 1 */
	l8ui	a13, a2, GD_ALWAYS
	movi	a11, 0
	movi	a14, 4
	movnez	a11, a14, a13
	or	a4, a4, a11		/* bit 2 */
	l8ui	a13, a2, GD_MOD
	movi	a11, 0
	movi	a14, 8
	moveqz	a11, a14, a13
	or	a4, a4, a11		/* bit 3: BLE */
	/* Groups: (f - lg) >> 2 while f - lg >= 4, none if it is negative. */
	l32i	a11, a2, GD_F
	sub	a11, a11, a3
	srai	a12, a11, 2
	movi	a13, 0
	movltz	a12, a13, a11
	loopnez	a12, .Lend

.Lgrp:
	extui	a12, a3, 2, 5		/* g & 31 */
	addx4	a13, a12, a2
	l32i	a13, a13, GD_PRING	/* p */
	s32i	a5, a1, 0
	addi	a3, a3, 4
	bbci	a4, 0, .Linit
	sub	a11, a13, a5
	srai	a11, a11, 2
	add	a5, a5, a11		/* savg += (p - savg) >> 2 */
.Lpw:
	extui	a11, a12, 0, 2
	beqz	a11, .Lpwr		/* every fourth group: pw_c anew */
.Lwr:
	add	a11, a2, a12
	addmi	a11, a11, 512
	s8i	a10, a11, GD_WRING - 512
	bbci	a4, 1, .Lfloor		/* no squelch: the gate follows the floor */
.Lon:
	bgeu	a13, a7, .Lup
	movi	a8, 0			/* below the gate */
	addi	a14, a9, 1
	movi	a11, 255
	minu	a9, a14, a11
.Lhold:
	/* Squelch hold (not while replaying the pre-roll). */
	bbsi	a4, 2, .Lnext
	l32i	a11, a2, GD_REPLAY_END
	sub	a11, a3, a11
	bltz	a11, .Lnext
	l32i	a11, a2, GD_S_CLOSE
	bltu	a5, a11, .Ldec
	l16ui	a11, a2, GD_HOLD
	s16i	a11, a2, GD_HOLD_CNT
.Lnext:
	nop				/* the branches above land here, inside the loop */
.Lend:
	movi	a15, 0

.Lstore:
	s32i	a3, a2, GD_LG
	s32i	a5, a2, GD_SAVG
	s32i	a6, a2, GD_FLOOR
	s32i	a7, a2, GD_S_GATE
	s8i	a8, a2, GD_RUN
	s8i	a9, a2, GD_OFF
	s8i	a10, a2, GD_PW_C
	mov	a2, a15
	retw.n

	/* The hold runs down; at its end the squelch closes after this group. */
.Ldec:
	l16ui	a11, a2, GD_HOLD_CNT
	bltui	a11, 2, .Lclose
	addi	a11, a11, -1
	s16i	a11, a2, GD_HOLD_CNT
	j	.Lnext
.Lclose:
	movi	a15, 1
	j	.Lstore

	/* The first group after a reset seeds the smoothing and the floor. */
.Linit:
	mov	a5, a13
	moveqz	a6, a13, a6
	movi	a11, 1
	s8i	a11, a2, GD_S_INIT
	or	a4, a4, a11
	j	.Lpw

	/* pw_c: the smoothed power in pwr units, with the boxcar's compensation for BLE. */
.Lpwr:
	extui	a10, a4, 3, 1
	beqz	a5, .Lwr
	nsau	a11, a5
	ssl	a11
	sll	a15, a5
	extui	a15, a15, 25, 6
	movi	a14, gfsk_log2_lut
	add	a14, a14, a15
	l8ui	a14, a14, 0
	movi	a15, 31
	sub	a15, a15, a11
	slli	a15, a15, 8
	add	a15, a15, a14		/* log2(savg) in Q8 */
	movi	a14, 1541
	mull	a15, a15, a14
	movi	a14, 1697548
	add	a15, a15, a14
	srli	a15, a15, 16
	add	a10, a10, a15
	j	.Lwr

	/* No squelch: the tracked noise floor and the gate 6 times it. */
.Lfloor:
	srli	a11, a13, 2
	bgeu	a11, a6, .Lfl_up
	bltu	a13, a6, .Lfl_down
	sub	a11, a13, a6
	srli	a11, a11, 6
	add	a6, a6, a11
	j	.Lfl_gate
.Lfl_down:
	sub	a11, a6, a13
	srli	a11, a11, 6
	sub	a6, a6, a11
	j	.Lfl_gate
.Lfl_up:
	srli	a11, a6, 12
	addi	a11, a11, 1
	add	a6, a6, a11
.Lfl_gate:
	movi	a11, 715827882		/* UINT32_MAX / 6 */
	movi	a7, -1
	bltu	a11, a6, .Lon
	addx2	a7, a6, a6
	slli	a7, a7, 1
	j	.Lon

	/*
	 * At or above the gate: count the run, a new burst on its second group. A step up inside a
	 * run (over 4 times the smoothed power before the group, which is below 2^30) starts it over.
	 */
.Lup:
	bltui	a8, 2, .Lcount
	l32i	a11, a1, 0
	slli	a11, a11, 2
	bgeu	a11, a13, .Lcount
	movi	a8, 0
.Lcount:
	movi	a11, 255
	addi	a14, a8, 1
	minu	a8, a14, a11
	bnei	a8, 1, .Lrun2
	addi	a11, a3, -4
	s32i	a11, a2, GD_PEND
	l32i	a11, a1, 0
	s32i	a11, a2, GD_SBASE
	j	.Lhold
.Lrun2:
	bnei	a8, 2, .Lhold
	bgeui	a9, 8, .Ledge
	l32i	a11, a2, GD_SBASE
	slli	a11, a11, 2
	bgeu	a11, a13, .Lnoedge
.Ledge:
	l32i	a11, a2, GD_PEND
	s32i	a11, a2, GD_EDGE
	movi	a14, 0
	s8i	a14, a2, GD_SEEDED
	l8ui	a14, a2, GD_CUR_VALID
	beqz	a14, .Lnoedge
	l8ui	a14, a2, GD_SLICED
	bnez	a14, .Lnoedge
	addi	a11, a11, -8		/* the cursor up to pend - LEAD_IN at the phase */
	l8ui	a14, a2, GD_PH
	sub	a14, a14, a11
	extui	a14, a14, 0, 2
	add	a11, a11, a14
	l32i	a14, a2, GD_CUR
	sub	a14, a11, a14
	blti	a14, 1, .Lnoedge
	s32i	a11, a2, GD_CUR
.Lnoedge:
	movi	a9, 0
	j	.Lhold
	.size	gfsk_groups_fast, . - gfsk_groups_fast
