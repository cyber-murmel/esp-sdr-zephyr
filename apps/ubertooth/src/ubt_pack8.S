/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The ring sink's samples to the sample ring's 8 bits on the vector unit (ubt_pack.c, pack_c()):
 * di[k] = MIN(i[k] >> 6, 127), dq[k] likewise, for k < n, the int8 of the low byte. The sink
 * runs on CPU 1 inside the ring loop, which has the vector unit enabled and keeps no vector
 * registers across its calls; the scalar loop cost 15 instructions a sample, some 3.75 of the
 * ring's 12 cycles a pair.
 *
 * void ubt_pack8(int8_t *di, int8_t *dq, const int16_t *i, const int16_t *q, uint32_t n,
 *                const int16_t k[2]);
 *
 * di and dq share their alignment (the ring's two planes), so do i and q (the ring's output
 * blocks, 16 byte aligned). k = {1, 127}. The head up to di's 16 byte boundary and the tail are
 * scalar; between them blocks of 16 samples a plane: the sources through unaligned loads
 * (ee.ld.128.usar, ee.src.q.qup), >> 6 as a multiply by 1 with SAR 6, the clamp, and the low
 * bytes of the 16 lanes by ee.vunzip.8. The loads run one block ahead of the samples taken.
 */

#include <esp_sdr/esp_sdr_place.h>

	ESP_SDR_ASM_SECTION(PACED_SAMPLE, ubt_pack8)
	.align	4
	.global	ubt_pack8
	.type	ubt_pack8, @function

/* a2 di, a3 dq, a4 i, a5 q, a6 n, a7 k */
ubt_pack8:
	entry	a1, 32
	movi	a10, 127
	/* head: up to di's 16 byte boundary, at most n */
	movi	a8, 15
	neg	a9, a2
	and	a9, a9, a8
	minu	a9, a9, a6
	sub	a6, a6, a9
	loopnez	a9, .Lhead_end
	l16si	a11, a4, 0
	l16si	a12, a5, 0
	addi	a4, a4, 2
	addi	a5, a5, 2
	srai	a11, a11, 6
	srai	a12, a12, 6
	min	a11, a11, a10
	min	a12, a12, a10
	s8i	a11, a2, 0
	s8i	a12, a3, 0
	addi	a2, a2, 1
	addi	a3, a3, 1
.Lhead_end:
	srli	a9, a6, 4
	beqz	a9, .Ltail
	ee.vldbc.16.ip	q6, a7, 2		/* 1 in every lane */
	ee.vldbc.16.ip	q7, a7, 0		/* 127 */
	movi	a11, 6
	ee.ld.128.usar.ip	q0, a4, 16
	ee.ld.128.usar.ip	q4, a5, 16
	loopnez	a9, .Lbody_end
	/* I: 16 samples, two unaligned vectors of 8 */
	ee.ld.128.usar.ip	q1, a4, 16
	ee.src.q.qup	q2, q0, q1
	ee.ld.128.usar.ip	q1, a4, 16
	ee.src.q.qup	q3, q0, q1
	wsr.sar	a11
	ee.vmul.s16	q2, q2, q6
	ee.vmul.s16	q3, q3, q6
	ee.vmin.s16	q2, q2, q7
	ee.vmin.s16	q3, q3, q7
	ee.vunzip.8	q2, q3			/* q2: the low bytes of the 16 lanes */
	ee.vst.128.ip	q2, a2, 16
	/* Q */
	ee.ld.128.usar.ip	q5, a5, 16
	ee.src.q.qup	q2, q4, q5
	ee.ld.128.usar.ip	q5, a5, 16
	ee.src.q.qup	q3, q4, q5
	wsr.sar	a11
	ee.vmul.s16	q2, q2, q6
	ee.vmul.s16	q3, q3, q6
	ee.vmin.s16	q2, q2, q7
	ee.vmin.s16	q3, q3, q7
	ee.vunzip.8	q2, q3
	ee.vst.128.ip	q2, a3, 16
.Lbody_end:
	/* The loads ran one block ahead: back to the first sample not taken. */
	addi	a4, a4, -16
	addi	a5, a5, -16
.Ltail:
	extui	a9, a6, 0, 4
	loopnez	a9, .Ltail_end
	l16si	a11, a4, 0
	l16si	a12, a5, 0
	addi	a4, a4, 2
	addi	a5, a5, 2
	srai	a11, a11, 6
	srai	a12, a12, 6
	min	a11, a11, a10
	min	a12, a12, a10
	s8i	a11, a2, 0
	s8i	a12, a3, 0
	addi	a2, a2, 1
	addi	a3, a3, 1
.Ltail_end:
	retw.n
	.size	ubt_pack8, . - ubt_pack8
