/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * PIE SIMD kernels of the fixed-point channelizer (esdr_pfbq.c) for the ESP32-P4's HP core.
 * Each computes what its C kernel in esdr_pfbq.c computes, bit for bit: those document the
 * arithmetic, the comments here the registers and the schedule.
 *
 * Assembled by Espressif's assembler (xesppie) with -mno-relax; Zephyr's linker takes the
 * object. Rules of the extension that shape the code:
 * - PIE instructions take GPRs x8 .. x15 and x24 .. x31 only (s0, s1, a0 .. a5, s8 .. s11,
 *   t3 .. t6).
 * - A hardware loop's setup is written as a word (LP_SETUP): the assembler's label form needs a
 *   relocation only Espressif's linker knows.
 * - A vector loaded by one instruction and used by the next stalls a cycle; unaligned vector
 *   access costs 3 to 4 cycles, so the vectors here are 16-byte aligned, but for the history
 *   loads of frames 1 and 3 when D is not a multiple of 4 (M = 20).
 * - srcmb with select 1 saturates, cmul's two modes write the low and the high half (so it
 *   works in place), vadd and vsub saturate.
 */

	.text

/* esp.lp.setup 0, x<regnum>, end: the loop runs from the next instruction to end. */
	.macro	LP_SETUP regnum, end
	.word	0x402b | (\regnum << 15) | ((((\end) - .) >> 1) << 20)
	.endm

	.macro	FN name
	.global	\name
	.type	\name, @function
	.align	2
\name:
	.endm

/* Register numbers for LP_SETUP. */
	.equ	X_A1, 11
	.equ	X_A2, 12

/*
 * void esdr_pfbq_pie_on(void): PIE and the hardware loops on (state CSRs 0x7f2, 0x7f1:
 * 1 = initial; Zephyr leaves them off), unaligned vector access allowed (cfg bit 1).
 */
FN esdr_pfbq_pie_on
	li	t0, 1
	csrw	0x7f2, t0
	csrw	0x7f1, t0
	esp.movx.r.cfg	t6
	ori	t6, t6, 2
	esp.movx.w.cfg	t6
	ret
	.size	esdr_pfbq_pie_on, . - esdr_pfbq_pie_on

/*
 * void esdr_pfbq_pie_conv(int16_t *dst, const int8_t *src, uint32_t pairs, const int16_t *kv)
 * pairs a multiple of 8, dst 16-byte aligned. kv: the multiplier in 8 lanes, then the offsets.
 */
FN esdr_pfbq_pie_conv
	esp.vld.128.ip	q6, a3, 16
	esp.vld.128.ip	q7, a3, 0
	li	t6, 0
	esp.movx.w.sar	t6
	srli	a2, a2, 3
	beqz	a2, 2f
	LP_SETUP X_A2, 1f
	esp.vldext.s8.ip	q0, q1, a1, 16
	esp.vmul.s16	q0, q0, q6
	esp.vmul.s16	q1, q1, q6
	esp.vadd.s16	q0, q0, q7
	esp.vadd.s16	q1, q1, q7
	esp.vst.128.ip	q0, a0, 16
1:	esp.vst.128.ip	q1, a0, 16
2:	ret
	.size	esdr_pfbq_pie_conv, . - esdr_pfbq_pie_conv

/*
 * void esdr_pfbq_pie_branch(const struct bk_args *a): the branch sums of one quad.
 *
 * struct bk_args: x0 0, work 4, taps 8, off 12, rnd 16, dstep 20, tstep 24, groups 28, t 32,
 * sh 36.
 *
 * Per group: four frames, each T multiply-adds of a taps vector (q0, q1 in turn) with a history
 * vector (q2, q3 in turn) into QACC (from the rounding addend), two instructions a tap (the
 * history load fused into the multiply-add), the sum to q4 .. q7; then the four transposed: zip
 * 32-bit lanes, and each branch's four frames are two 64-bit halves. A frame's tail loads the
 * next frame's first taps and history while the last products land (the next group's, after
 * the fourth frame), so no load or sum is used the instruction after it is made.
 *
 *   s0  group's history pointer (frame 0, tap 0)   a0  taps pointer, running
 *   s1  work                                        a1  group's taps
 *   a2  off, running                                a3  rnd
 *   a4  dstep    a5  tstep (history stride)         t3  history pointer, running
 *   t4  sh       t5, t6, s8, s9  store addresses    t0  groups left
 *   t1  T * 16 (a group's taps)                     t2  2 dstep    a6  3 dstep    a7  -16
 */

/*
 * One frame of T taps, its first taps (q0, q1) and history (q2) loaded: the sum to \out; then
 * the next frame's first loads, its history from s0 + \next, its taps from a1 (\retap) or where
 * the tap pointer is (the next group's).
 */
	.macro	FRAME T, out, next, retap
	esp.ldqa.s16.128.ip	a3, 0
	.set	k, 0
	.rept	\T - 1
	.if	(k % 2) == 0
	esp.vmulas.s16.qacc.ld.xp	q3, t3, a5, q0, q2
	.if	k + 2 < \T
	esp.vld.128.ip	q0, a0, 16
	.endif
	.else
	esp.vmulas.s16.qacc.ld.xp	q2, t3, a5, q1, q3
	.if	k + 2 < \T
	esp.vld.128.ip	q1, a0, 16
	.endif
	.endif
	.set	k, k + 1
	.endr
	add	t3, s0, \next
	esp.vmulas.s16.qacc	q1, q3
	.if	\retap
	mv	a0, a1
	.endif
	esp.vld.128.ip	q0, a0, 16
	esp.vld.128.xp	q2, t3, a5
	esp.srcmb.s16.qacc	\out, t4, 1
	esp.vld.128.ip	q1, a0, 16
	.endm

/* The group loop for T taps (labels 1x for T = 4, 2x for 8, 3x for 12). */
	.macro	GROUPS T
	mv	t3, s0
	mv	a0, a1
	esp.vld.128.ip	q0, a0, 16
	esp.vld.128.xp	q2, t3, a5
	esp.vld.128.ip	q1, a0, 16
\T\()0:
	FRAME	\T, q4, a4, 1
	FRAME	\T, q5, t2, 1
	FRAME	\T, q6, a6, 1
	FRAME	\T, q7, a7, 0
	add	a1, a1, t1
	lhu	t5, 0(a2)
	lhu	t6, 2(a2)
	lhu	s8, 4(a2)
	lhu	s9, 6(a2)
	esp.vzip.32	q4, q5
	esp.vzip.32	q6, q7
	add	t5, t5, s1
	add	t6, t6, s1
	add	s8, s8, s1
	add	s9, s9, s1
	/* Lane 0: low halves of q4 and q6; lane 1: high halves; lanes 2, 3: q5 and q7. */
	esp.vst.l.64.ip	q4, t5, 8
	esp.vst.l.64.ip	q6, t5, 0
	esp.vst.h.64.ip	q4, t6, 8
	esp.vst.h.64.ip	q6, t6, 0
	esp.vst.l.64.ip	q5, s8, 8
	esp.vst.l.64.ip	q7, s8, 0
	esp.vst.h.64.ip	q5, s9, 8
	esp.vst.h.64.ip	q7, s9, 0
	addi	a2, a2, 8
	addi	s0, s0, -16
	addi	t0, t0, -1
	bnez	t0, \T\()0b
	j	9f
	.endm

FN esdr_pfbq_pie_branch
	addi	sp, sp, -16
	sw	s0, 0(sp)
	sw	s1, 4(sp)
	sw	s8, 8(sp)
	sw	s9, 12(sp)
	lw	s0, 0(a0)
	lw	s1, 4(a0)
	lw	a1, 8(a0)
	lw	a2, 12(a0)
	lw	a3, 16(a0)
	lw	a4, 20(a0)
	lw	a5, 24(a0)
	lw	t0, 28(a0)
	lw	t1, 32(a0)
	lw	t4, 36(a0)
	slli	t2, a4, 1
	add	a6, t2, a4
	li	a7, -16
	li	t6, 8
	beq	t1, t6, 2f
	li	t6, 12
	beq	t1, t6, 3f
	slli	t1, t1, 4
	GROUPS	4
2:	slli	t1, t1, 4
	GROUPS	8
3:	slli	t1, t1, 4
	GROUPS	12
9:	lw	s0, 0(sp)
	lw	s1, 4(sp)
	lw	s8, 8(sp)
	lw	s9, 12(sp)
	addi	sp, sp, 16
	ret
	.size	esdr_pfbq_pie_branch, . - esdr_pfbq_pie_branch

/*
 * Radix-4 butterfly of q0 .. q3 (twiddled already) in place, q7 = -j: t0 .. t3 in q4, q5, q6,
 * q3; X0, X1, X2, X3 to q0, q1, q2, q3.
 */
	.macro	BF4
	esp.vadd.s16	q4, q0, q2
	esp.vsub.s16	q5, q0, q2
	esp.vadd.s16	q6, q1, q3
	esp.vsub.s16	q3, q1, q3
	esp.cmul.s16	q3, q3, q7, 0
	esp.cmul.s16	q3, q3, q7, 1
	esp.vadd.s16	q0, q4, q6
	esp.vsub.s16	q2, q4, q6
	esp.vadd.s16	q1, q5, q3
	esp.vsub.s16	q3, q5, q3
	.endm

/*
 * void esdr_pfbq_pie_r4f(int16_t *dst, const int16_t *src, uint32_t n, const struct consts *c)
 * consts: k5 0, r5 16, j 32, sign 48.
 */
FN esdr_pfbq_pie_r4f
	addi	t3, a3, 32
	esp.vld.128.ip	q7, t3, 0
	li	t6, 15
	esp.movx.w.sar	t6
	LP_SETUP X_A2, 1f
	esp.vld.128.ip	q0, a1, 16
	esp.vld.128.ip	q1, a1, 16
	esp.vld.128.ip	q2, a1, 16
	esp.vld.128.ip	q3, a1, 16
	BF4
	esp.vst.128.ip	q0, a0, 16
	esp.vst.128.ip	q1, a0, 16
	esp.vst.128.ip	q2, a0, 16
1:	esp.vst.128.ip	q3, a0, 16
	ret
	.size	esdr_pfbq_pie_r4f, . - esdr_pfbq_pie_r4f

/*
 * void esdr_pfbq_pie_r4(int16_t *buf, uint32_t m, uint32_t span, const int16_t *tw,
 *                       const struct consts *c)
 * Column 0 without twiddles, then columns 1 .. span - 1 with theirs (3 vectors each, reloaded
 * per butterfly: the butterfly needs their registers).
 *
 *   a5  load pointer   t3  store pointer   t4  span * 16 (stride)   t5  twiddle pointer
 *   t6  groups a column (m / (4 span))     a6  groups left          a7  columns left
 *   t2  column base
 */
FN esdr_pfbq_pie_r4
	addi	t3, a4, 32
	esp.vld.128.ip	q7, t3, 0
	li	t6, 15
	esp.movx.w.sar	t6
	slli	t4, a2, 4
	slli	t6, a2, 2
	divu	t6, a1, t6
	mv	a5, a0
	mv	a6, t6
1:
	mv	t3, a5
	esp.vld.128.xp	q0, a5, t4
	esp.vld.128.xp	q1, a5, t4
	esp.vld.128.xp	q2, a5, t4
	esp.vld.128.xp	q3, a5, t4
	BF4
	esp.vst.128.xp	q0, t3, t4
	esp.vst.128.xp	q1, t3, t4
	esp.vst.128.xp	q2, t3, t4
	esp.vst.128.xp	q3, t3, t4
	addi	a6, a6, -1
	bnez	a6, 1b
	addi	a7, a2, -1
	beqz	a7, 4f
	addi	t2, a0, 16
2:
	mv	a5, t2
	mv	a6, t6
3:
	mv	t3, a5
	mv	t5, a3
	esp.vld.128.xp	q0, a5, t4
	esp.vld.128.xp	q1, a5, t4
	esp.vld.128.ip	q4, t5, 16
	esp.vld.128.xp	q2, a5, t4
	esp.vld.128.ip	q5, t5, 16
	esp.vld.128.xp	q3, a5, t4
	esp.vld.128.ip	q6, t5, 16
	esp.cmul.s16	q1, q1, q4, 0
	esp.cmul.s16	q1, q1, q4, 1
	esp.cmul.s16	q2, q2, q5, 0
	esp.cmul.s16	q2, q2, q5, 1
	esp.cmul.s16	q3, q3, q6, 0
	esp.cmul.s16	q3, q3, q6, 1
	BF4
	esp.vst.128.xp	q0, t3, t4
	esp.vst.128.xp	q1, t3, t4
	esp.vst.128.xp	q2, t3, t4
	esp.vst.128.xp	q3, t3, t4
	addi	a6, a6, -1
	bnez	a6, 3b
	addi	a3, a3, 48
	addi	t2, t2, 16
	addi	a7, a7, -1
	bnez	a7, 2b
4:	ret
	.size	esdr_pfbq_pie_r4, . - esdr_pfbq_pie_r4

/*
 * void esdr_pfbq_pie_r2(int16_t *buf, uint32_t m, uint32_t span, const int16_t *tw)
 * Registers as esdr_pfbq_pie_r4; q4 the column's twiddle.
 */
FN esdr_pfbq_pie_r2
	li	t6, 15
	esp.movx.w.sar	t6
	slli	t4, a2, 4
	slli	t6, a2, 1
	divu	t6, a1, t6
	mv	a5, a0
	mv	a6, t6
1:
	mv	t3, a5
	esp.vld.128.xp	q0, a5, t4
	esp.vld.128.xp	q1, a5, t4
	esp.vadd.s16	q2, q0, q1
	esp.vsub.s16	q3, q0, q1
	esp.vst.128.xp	q2, t3, t4
	esp.vst.128.xp	q3, t3, t4
	addi	a6, a6, -1
	bnez	a6, 1b
	addi	a7, a2, -1
	beqz	a7, 4f
	addi	t2, a0, 16
2:
	esp.vld.128.ip	q4, a3, 16
	mv	a5, t2
	mv	a6, t6
3:
	mv	t3, a5
	esp.vld.128.xp	q0, a5, t4
	esp.vld.128.xp	q1, a5, t4
	esp.cmul.s16	q1, q1, q4, 0
	esp.cmul.s16	q1, q1, q4, 1
	esp.vadd.s16	q2, q0, q1
	esp.vsub.s16	q3, q0, q1
	esp.vst.128.xp	q2, t3, t4
	esp.vst.128.xp	q3, t3, t4
	addi	a6, a6, -1
	bnez	a6, 3b
	addi	t2, t2, 16
	addi	a7, a7, -1
	bnez	a7, 2b
4:	ret
	.size	esdr_pfbq_pie_r2, . - esdr_pfbq_pie_r2

/*
 * void esdr_pfbq_pie_r5(int16_t *dst, const int16_t *src, uint32_t n, const struct consts *c)
 * q7 the constants (lanes: 1, C1, C2, S1, S2, -S1, -S2), q6 = -j; per butterfly a0 q0, b1 q1,
 * b2 q2, j1 q3, j2 q4. X1 .. X4 each start from the rounding addend (t3) and take five
 * products, shift 14 (t5); they come out in q6 and q5 in turn, each stored while the next one's
 * products run (so -j is loaded again for each butterfly).
 */
	.macro	R5OUT out, prev, l1, l2, l3, l4
	esp.ldqa.s16.128.ip	t3, 0
	esp.vsmulas.s16.qacc	q0, q7, 0
	esp.vsmulas.s16.qacc	q1, q7, \l1
	esp.vst.128.ip	\prev, a0, 16
	esp.vsmulas.s16.qacc	q2, q7, \l2
	esp.vsmulas.s16.qacc	q3, q7, \l3
	esp.vsmulas.s16.qacc	q4, q7, \l4
	esp.srcmb.s16.qacc	\out, t5, 1
	.endm

FN esdr_pfbq_pie_r5
	esp.vld.128.ip	q7, a3, 16
	mv	t3, a3
	addi	t4, a3, 16
	li	t6, 15
	esp.movx.w.sar	t6
	li	t5, 14
	LP_SETUP X_A2, 1f
	esp.vld.128.ip	q0, a1, 16
	esp.vld.128.ip	q1, a1, 16
	esp.vld.128.ip	q2, a1, 16
	esp.vld.128.ip	q3, a1, 16
	esp.vld.128.ip	q4, a1, 16
	esp.vld.128.ip	q6, t4, 0
	esp.vsub.s16	q5, q1, q4
	esp.vadd.s16	q1, q1, q4
	esp.vsub.s16	q4, q2, q3
	esp.vadd.s16	q2, q2, q3
	esp.cmul.s16	q3, q5, q6, 0
	esp.cmul.s16	q3, q5, q6, 1
	esp.cmul.s16	q4, q4, q6, 0
	esp.cmul.s16	q4, q4, q6, 1
	esp.vadd.s16	q5, q0, q1
	esp.vadd.s16	q5, q5, q2
	R5OUT	q6, q5, 1, 2, 3, 4
	R5OUT	q5, q6, 2, 1, 4, 5
	R5OUT	q6, q5, 2, 1, 6, 3
	R5OUT	q5, q6, 1, 2, 5, 6
	nop
1:	esp.vst.128.ip	q5, a0, 16
	ret
	.size	esdr_pfbq_pie_r5, . - esdr_pfbq_pie_r5

/*
 * void esdr_pfbq_pie_r2l(int16_t *buf, uint32_t span, const int16_t *tw): the last radix-2
 * stage, one group: column 0 without a twiddle, columns 1 .. span - 1 with theirs. a0 and t3
 * the outputs' (and first inputs') pointers, t5 the next second input's; q1 and q4 that input
 * and its twiddle, loaded a turn ahead (the last turn reads a vector past each, unused).
 */
FN esdr_pfbq_pie_r2l
	li	t6, 15
	esp.movx.w.sar	t6
	slli	t4, a1, 4
	add	t3, a0, t4
	esp.vld.128.ip	q0, a0, 0
	esp.vld.128.ip	q1, t3, 0
	esp.vld.128.ip	q4, a2, 16
	esp.vadd.s16	q2, q0, q1
	esp.vsub.s16	q3, q0, q1
	esp.vst.128.ip	q2, a0, 16
	esp.vst.128.ip	q3, t3, 16
	esp.vld.128.ip	q1, t3, 0
	addi	t5, t3, 16
	addi	a1, a1, -1
	LP_SETUP X_A1, 1f
	esp.vld.128.ip	q0, a0, 0
	esp.cmul.s16	q1, q1, q4, 0
	esp.cmul.s16	q1, q1, q4, 1
	esp.vld.128.ip	q4, a2, 16
	esp.vadd.s16	q2, q0, q1
	esp.vsub.s16	q3, q0, q1
	esp.vld.128.ip	q1, t5, 16
	esp.vst.128.ip	q2, a0, 16
1:	esp.vst.128.ip	q3, t3, 16
	ret
	.size	esdr_pfbq_pie_r2l, . - esdr_pfbq_pie_r2l

/*
 * void esdr_pfbq_pie_r4l(int16_t *buf, uint32_t span, const int16_t *tw,
 *                        const struct consts *c)
 * The last radix-4 stage, one group: column j's inputs span vectors apart from buf + j, its
 * twiddles in order. t2 the column, a5 and t3 its load and store pointers, t4 the stride.
 */
FN esdr_pfbq_pie_r4l
	addi	t3, a3, 32
	esp.vld.128.ip	q7, t3, 0
	li	t6, 15
	esp.movx.w.sar	t6
	slli	t4, a1, 4
	mv	t2, a0
	mv	a5, t2
	mv	t3, t2
	esp.vld.128.xp	q0, a5, t4
	esp.vld.128.xp	q1, a5, t4
	esp.vld.128.xp	q2, a5, t4
	esp.vld.128.xp	q3, a5, t4
	BF4
	esp.vst.128.xp	q0, t3, t4
	esp.vst.128.xp	q1, t3, t4
	esp.vst.128.xp	q2, t3, t4
	esp.vst.128.xp	q3, t3, t4
	addi	a1, a1, -1
	LP_SETUP X_A1, 1f
	addi	t2, t2, 16
	mv	a5, t2
	mv	t3, t2
	esp.vld.128.xp	q0, a5, t4
	esp.vld.128.xp	q1, a5, t4
	esp.vld.128.ip	q4, a2, 16
	esp.vld.128.xp	q2, a5, t4
	esp.vld.128.ip	q5, a2, 16
	esp.vld.128.xp	q3, a5, t4
	esp.vld.128.ip	q6, a2, 16
	esp.cmul.s16	q1, q1, q4, 0
	esp.cmul.s16	q1, q1, q4, 1
	esp.cmul.s16	q2, q2, q5, 0
	esp.cmul.s16	q2, q2, q5, 1
	esp.cmul.s16	q3, q3, q6, 0
	esp.cmul.s16	q3, q3, q6, 1
	BF4
	esp.vst.128.xp	q0, t3, t4
	esp.vst.128.xp	q1, t3, t4
	esp.vst.128.xp	q2, t3, t4
1:	esp.vst.128.xp	q3, t3, t4
	ret
	.size	esdr_pfbq_pie_r4l, . - esdr_pfbq_pie_r4l

/*
 * void esdr_pfbq_pie_flip(int16_t *buf, uint32_t m, const struct consts *c): the odd channels
 * times the sign vector (lanes of frames 1 and 3 negated), two channels a turn (m / 4).
 */
FN esdr_pfbq_pie_flip
	addi	t3, a2, 48
	esp.vld.128.ip	q7, t3, 0
	li	t6, 0
	esp.movx.w.sar	t6
	addi	a0, a0, 16
	addi	t4, a0, 32
	li	t5, 64
	srli	a1, a1, 2
	LP_SETUP X_A1, 1f
	esp.vld.128.ip	q0, a0, 0
	esp.vld.128.ip	q1, t4, 0
	esp.vmul.s16	q0, q0, q7
	esp.vmul.s16	q1, q1, q7
	esp.vst.128.xp	q0, a0, t5
1:	esp.vst.128.xp	q1, t4, t5
	ret
	.size	esdr_pfbq_pie_flip, . - esdr_pfbq_pie_flip

/*
 * void esdr_pfbq_pie_power4(const int16_t *const *q, uint32_t m, uint32_t *pw): per channel
 * the four quads' squares summed in XACC (the loads of the next channel fused), >> 6 saturated.
 * The last turn loads a vector past each quad, which it does not use.
 */
FN esdr_pfbq_pie_power4
	lw	t3, 0(a0)
	lw	t4, 4(a0)
	lw	t5, 8(a0)
	lw	t6, 12(a0)
	li	a3, 6
	esp.vld.128.ip	q0, t3, 16
	esp.vld.128.ip	q1, t4, 16
	esp.vld.128.ip	q2, t5, 16
	esp.vld.128.ip	q3, t6, 16
	LP_SETUP X_A1, 1f
	esp.zero.xacc
	esp.vmulas.s16.xacc.ld.ip	q0, t3, 16, q0, q0
	esp.vmulas.s16.xacc.ld.ip	q1, t4, 16, q1, q1
	esp.vmulas.s16.xacc.ld.ip	q2, t5, 16, q2, q2
	esp.vmulas.s16.xacc.ld.ip	q3, t6, 16, q3, q3
	esp.srs.s.xacc	a4, a3
	sw	a4, 0(a2)
1:	addi	a2, a2, 4
	ret
	.size	esdr_pfbq_pie_power4, . - esdr_pfbq_pie_power4

/*
 * void esdr_pfbq_pie_move(int16_t *dst, const int16_t *src, uint32_t n16): n16 vectors, dst at
 * or below src, ascending (each turn loads before it stores).
 */
FN esdr_pfbq_pie_move
	andi	t0, a2, 1
	beqz	t0, 1f
	esp.vld.128.ip	q0, a1, 16
	esp.vst.128.ip	q0, a0, 16
1:	srli	a2, a2, 1
	beqz	a2, 3f
	LP_SETUP X_A2, 2f
	esp.vld.128.ip	q0, a1, 16
	esp.vld.128.ip	q1, a1, 16
	esp.vst.128.ip	q0, a0, 16
2:	esp.vst.128.ip	q1, a0, 16
3:	ret
	.size	esdr_pfbq_pie_move, . - esdr_pfbq_pie_move
