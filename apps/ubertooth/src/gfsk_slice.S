/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The GFSK demodulator's slicer with the early/late comparison (gfsk_demod.c, slice_c() with
 * early): bit for bit the C loop, in about half its instructions. Before the burst is acquired
 * it stops where the C runs the acquisition (the cursor LEAD_IN before the burst's edge), so
 * the C can run it and call it again. The C keeps
 * every value in memory around the symbol stores (char stores alias everything) and spills;
 * here the levels, the early/late sums, the output pointer and its end stay in registers, the
 * ring window is five loads off one address (the one symbol in 32 whose window wraps the ring's
 * end masks each index), and the symbols run as a zero-overhead loop. The cursor is aligned to
 * the sampling phase only when the phase changed (it keeps the alignment while it steps by 4);
 * as in C it is checked against @p last before that alignment, so the symbol at the aligned
 * cursor is sliced even if the alignment moved it past last, and the count of the ones after
 * it follows from the aligned cursor.
 *
 * struct gfsk_sym *gfsk_slice_fast(struct gfsk_demod *d, uint32_t last, struct gfsk_sym *op,
 *                                  struct gfsk_sym *oend, uint32_t acq_on, uint32_t acq);
 *   Slices from d->cur, writes the symbols from op on (dropped at oend), returns the next op.
 *   With acq_on, stops before a symbol whose cursor reaches acq (checked, as in C, before the
 *   alignment of the first one).
 *   The state lives in d (gfsk_demod_off.h); the parameters are the reference's: WIN 2, DD_K 5,
 *   EL_PERIOD 32, EL_SHIFT 2, TOFF 3, GFSK_RING 128, GFSK_PRING 32.
 *
 * Registers: a2 d, a3 cur, a4 m0, a5 m1, a6 el_e, a7 el_a, a8 op, a9 last, a10 oend; a11 cur - 3
 * through a symbol, a12..a15 scratch (a13 the bit after the decision).
 */

#include <esp_sdr/esp_sdr_place.h>

#include "gfsk_demod_off.h"

	ESP_SDR_ASM_SECTION(SAMPLE, gfsk_slice)
	.align	4
	.global	gfsk_slice_fast
	.type	gfsk_slice_fast, @function
gfsk_slice_fast:
	entry	a1, 32
	s32i	a6, a1, 0		/* acq_on */
	s32i	a7, a1, 4		/* acq */
	mov	a9, a3
	mov	a8, a4
	mov	a10, a5
	l32i	a3, a2, GD_CUR
	l32i	a4, a2, GD_M0
	l32i	a5, a2, GD_M1
	l32i	a6, a2, GD_EL_E
	l32i	a7, a2, GD_EL_A

	/* Check the cursor, align it to ph (a step of -1 to +2), count the symbols to last. */
.Lcheck:
	sub	a11, a9, a3
	bltz	a11, .Ldone
	/* Not acquired yet: stop where the C runs the acquisition. */
	l32i	a12, a1, 0
	beqz	a12, .Lalign
	l32i	a12, a1, 4
	sub	a12, a3, a12
	bgez	a12, .Ldone
.Lalign:
	l8ui	a12, a2, GD_PH
	sub	a12, a12, a3
	extui	a12, a12, 0, 2
	addi	a13, a12, -3
	bnez	a13, .Lfwd
	addi	a3, a3, -1
	addi	a11, a11, 1
	j	.Lcount
.Lfwd:
	add	a3, a3, a12
	sub	a11, a11, a12
.Lcount:
	/* This one, and those at +4, +8, ... up to last: 1 + max(0, (last - cur) >> 2). */
	srai	a12, a11, 2
	movi	a13, 0
	movltz	a12, a13, a11
	addi	a12, a12, 1
	/* Not acquired: those after this one only while their cursor stays below acq. */
	l32i	a13, a1, 0
	beqz	a13, .Lrun
	l32i	a13, a1, 4
	sub	a13, a13, a3
	addi	a13, a13, -1
	srai	a13, a13, 2
	movi	a14, 0
	movltz	a13, a14, a13
	addi	a13, a13, 1
	minu	a12, a12, a13
.Lrun:
	loop	a12, .Lend

	/* One symbol: the window sums at the cursor and one sample either side. */
.Lbody:
	addi	a11, a3, -3
	extui	a12, a11, 0, 7
	addi	a13, a12, 4
	bbsi	a13, 7, .Lwrap
	addx4	a12, a12, a2
	l32i	a13, a12, GD_CRING + 12	/* cr[cur] */
	l32i	a14, a12, GD_CRING + 4	/* cr[cur - 2] */
	l32i	a15, a12, GD_CRING + 16	/* cr[cur + 1] */
	sub	a13, a13, a14		/* w */
	l32i	a14, a12, GD_CRING + 8	/* cr[cur - 1] */
	l32i	a12, a12, GD_CRING	/* cr[cur - 3] */
.Lwin:
	sub	a15, a15, a14		/* one sample later */
	sub	a14, a14, a12		/* one sample earlier */
	sub	a15, a15, a14
	srai	a15, a15, 4		/* dl */
	srai	a12, a4, 1
	srai	a14, a5, 1
	add	a12, a12, a14		/* thr, the middle of the levels */
	sub	a14, a13, a12
	srai	a14, a14, 4
	abs	a14, a14
	add	a7, a7, a14		/* el_a += |w - thr| >> 4 */
	blt	a13, a12, .Lone
	sub	a14, a13, a4
	srai	a14, a14, 5
	add	a4, a4, a14		/* m0 follows a 0 */
	add	a6, a6, a15
	movi	a13, 0
	j	.Lcnt
.Lone:
	sub	a14, a13, a5
	srai	a14, a14, 5
	add	a5, a5, a14		/* m1 follows a 1 */
	sub	a6, a6, a15
	movi	a13, 1
.Lcnt:
	l8ui	a14, a2, GD_EL_CNT
	addi	a14, a14, -1
	s8i	a14, a2, GD_EL_CNT
	beqz	a14, .Lperiod

	/* The symbol out: t = cur - 3, the bit and the power of its group. */
.Lsym:
	extui	a14, a11, 2, 5
	add	a14, a14, a2
	addmi	a14, a14, 512
	l8ui	a14, a14, GD_WRING - 512
	bgeu	a8, a10, .Lnext
	slli	a14, a14, 8
	or	a14, a14, a13
	s32i	a11, a8, 0
	s16i	a14, a8, 4
	addi	a8, a8, 8
.Lnext:
	addi	a3, a3, 4
.Lend:

.Ldone:
	s32i	a3, a2, GD_CUR
	s32i	a4, a2, GD_M0
	s32i	a5, a2, GD_M1
	s32i	a6, a2, GD_EL_E
	s32i	a7, a2, GD_EL_A
	mov	a2, a8
	retw.n

	/* The window wraps the ring's end: each index masked (a12 = (cur - 3) & 127). */
.Lwrap:
	addi	a15, a11, 3
	extui	a15, a15, 0, 7
	addx4	a15, a15, a2
	l32i	a13, a15, GD_CRING	/* cr[cur] */
	addi	a15, a11, 1
	extui	a15, a15, 0, 7
	addx4	a15, a15, a2
	l32i	a15, a15, GD_CRING	/* cr[cur - 2] */
	sub	a13, a13, a15		/* w */
	addi	a15, a11, 2
	extui	a15, a15, 0, 7
	addx4	a15, a15, a2
	l32i	a14, a15, GD_CRING	/* cr[cur - 1] */
	addi	a15, a11, 4
	extui	a15, a15, 0, 7
	addx4	a15, a15, a2
	l32i	a15, a15, GD_CRING	/* cr[cur + 1] */
	addx4	a12, a12, a2
	l32i	a12, a12, GD_CRING	/* cr[cur - 3] */
	j	.Lwin

	/*
	 * Every 32 symbols: move the phase where the eye is clearly better on one side (a13 bit
	 * and a11 cur - 3 of this symbol stay).
	 */
.Lperiod:
	movi	a14, 32
	s8i	a14, a2, GD_EL_CNT
	srli	a14, a7, 2		/* lim = el_a >> EL_SHIFT */
	movi	a7, 0
	blt	a14, a6, .Llater	/* el_e > lim */
	neg	a14, a14
	bge	a6, a14, .Lkeep		/* el_e >= -lim */
	movi	a6, 0
	l8ui	a15, a2, GD_PH
	addi	a15, a15, -1
	j	.Lmoved
.Llater:
	movi	a6, 0
	l8ui	a15, a2, GD_PH
	addi	a15, a15, 1
.Lmoved:
	extui	a15, a15, 0, 2
	s8i	a15, a2, GD_PH
	/* This symbol at the old phase, then check the next and align it to the new one. */
	extui	a14, a11, 2, 5
	add	a14, a14, a2
	addmi	a14, a14, 512
	l8ui	a14, a14, GD_WRING - 512
	bgeu	a8, a10, .Lmnext
	slli	a14, a14, 8
	or	a14, a14, a13
	s32i	a11, a8, 0
	s16i	a14, a8, 4
	addi	a8, a8, 8
.Lmnext:
	addi	a3, a3, 4
	j	.Lcheck
.Lkeep:
	movi	a6, 0
	j	.Lsym
	.size	gfsk_slice_fast, . - gfsk_slice_fast
