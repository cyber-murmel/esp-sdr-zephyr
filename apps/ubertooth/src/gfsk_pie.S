/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The GFSK demodulator's FIR on the ESP32-S3 vector unit (PIE): z[n] = x[n] + x[n-1] + x[n-2]
 * + x[n-3] + 2 of the 8-bit I/Q planes (the channel at DC; each sample at the middle of the
 * floor's step, x + 1/2), as int16 planes, bit for bit what zp_fill_c() in gfsk_demod.c computes.
 * In two passes over blocks of 16 samples, both 16-byte aligned:
 *
 *   gfsk_pie_u: per plane u[n] = x[n] + x[n-2] + 1, the bytes sign-extended (a compare with zero
 *               gives the high bytes, EE.VZIP.8 interleaves them), x[n-2] taken from the
 *               concatenation with the previous block (EE.SRC.Q, SAR_BYTE 12), the 1 as the
 *               subtraction of a compare's all-ones lanes (-1);
 *   gfsk_pie_z: in place, per plane z[n] = u[n] + u[n-1] (SAR_BYTE 14 for the sample before).
 *
 * Until 2026-11-12 the channel sat at +1 MHz: u = x - x[n-2], z_i = u_i - u_q[n-1], z_q = u_q +
 * u_i[n-1] (the taps 1, j, -1, -j). |u| <= 255 and |z| <= 510: the saturating EE.VADDS/VSUBS
 * never saturate.
 *
 * void gfsk_pie_u(const int8_t *x, int16_t *u, uint32_t nblk);
 *   x: the plane at stream sample zb - 16 (the block before the first), u: 8 + 16 nblk int16,
 *   u[0..7] = u of samples zb - 8 .. zb - 1, u[8 + k] = u of zb + k.
 *
 * void gfsk_pie_z(int16_t *zi, int16_t *zq, uint32_t nblk);
 *   zi, zq: the two u planes as gfsk_pie_u() left them; afterwards zi[8 + k], zq[8 + k] = z of
 *   sample zb + k (the first 8 entries keep u).
 *
 * void gfsk_pie_e(const int16_t *zi, const int16_t *zq, int32_t *e, uint32_t nvec);
 *   The delay-2 discriminator x 256 of the full path, (z_i[n] z_q[n-2] - z_q[n] z_i[n-2]) << 8,
 *   in int32: zi, zq point at the 8 samples before the first (16-byte aligned), e[k] is the
 *   value of the k-th sample after them, for nvec blocks of 8. The 32-bit products are their
 *   low and high halves (EE.VMUL.S16 with SAR 0 and 16, which keeps the low 16 bits of the
 *   shifted product) zipped together; |e| < 2^19, so the saturating subtraction is exact.
 *
 * The kernels clobber q0..q7, SAR and SAR_BYTE, which Zephyr does not save per thread: the
 * caller locks interrupts and enables the unit (esp_sdr_pie_enable()) first. In IRAM, as the
 * demodulator's loops are.
 */

#include <esp_sdr/esp_sdr_place.h>

	ESP_SDR_ASM_SECTION(SAMPLE, gfsk_pie)

	.align	4
	.global	gfsk_pie_u
	.type	gfsk_pie_u, @function
gfsk_pie_u:
	entry	a1, 32
	movi.n	a5, 12
	wur.sar_byte	a5
	ee.zero.q	q7
	ee.vcmp.eq.s16	q5, q7, q7		/* -1 in every lane */
	/* The block before: its x lanes 8..15 (q6) and u of them. */
	ee.vld.128.ip	q0, a2, 16
	ee.vcmp.lt.s8	q1, q0, q7		/* the high bytes: 0xff where x < 0 */
	ee.vzip.8	q0, q1			/* q0 = x lanes 0..7, q1 = 8..15, int16 */
	ee.src.q	q2, q0, q1		/* x[n-2] of lanes 8..15 */
	ee.vadds.s16	q3, q1, q2
	ee.vsubs.s16	q3, q3, q5		/* + 1 */
	ee.vst.128.ip	q3, a3, 16
	ee.orq	q6, q1, q1
	loopnez	a4, .Lu_done
	ee.vld.128.ip	q0, a2, 16
	ee.vcmp.lt.s8	q1, q0, q7
	ee.vzip.8	q0, q1
	ee.src.q	q2, q6, q0		/* x[n-2] of lanes 0..7: the block before's 14, 15 */
	ee.vadds.s16	q3, q0, q2
	ee.vsubs.s16	q3, q3, q5
	ee.src.q	q2, q0, q1		/* x[n-2] of lanes 8..15 */
	ee.vadds.s16	q4, q1, q2
	ee.vsubs.s16	q4, q4, q5
	ee.vst.128.ip	q3, a3, 16
	ee.vst.128.ip	q4, a3, 16
	ee.orq	q6, q1, q1
.Lu_done:
	retw.n
	.size	gfsk_pie_u, . - gfsk_pie_u

	.align	4
	.global	gfsk_pie_z
	.type	gfsk_pie_z, @function
gfsk_pie_z:
	entry	a1, 32
	movi.n	a5, 14
	wur.sar_byte	a5
	ee.vld.128.ip	q6, a2, 16		/* u_i of the 8 samples before */
	ee.vld.128.ip	q7, a3, 16		/* u_q of the 8 samples before */
	mov.n	a6, a2				/* z goes where u was */
	mov.n	a7, a3
	loopnez	a4, .Lz_done
	ee.vld.128.ip	q0, a2, 16		/* u_i lanes 0..7 */
	ee.vld.128.ip	q1, a2, 16		/* u_i lanes 8..15 */
	ee.vld.128.ip	q2, a3, 16		/* u_q lanes 0..7 */
	ee.vld.128.ip	q3, a3, 16		/* u_q lanes 8..15 */
	ee.src.q	q4, q6, q0		/* u_i[n-1] of lanes 0..7 */
	ee.vadds.s16	q5, q0, q4
	ee.vst.128.ip	q5, a6, 16		/* z_i lanes 0..7 */
	ee.src.q	q4, q0, q1		/* u_i[n-1] of lanes 8..15 */
	ee.vadds.s16	q5, q1, q4
	ee.vst.128.ip	q5, a6, 16
	ee.src.q	q4, q7, q2		/* u_q[n-1] of lanes 0..7 */
	ee.vadds.s16	q5, q2, q4
	ee.vst.128.ip	q5, a7, 16		/* z_q lanes 0..7 */
	ee.src.q	q4, q2, q3		/* u_q[n-1] of lanes 8..15 */
	ee.vadds.s16	q5, q3, q4
	ee.vst.128.ip	q5, a7, 16
	ee.orq	q6, q1, q1
	ee.orq	q7, q3, q3
.Lz_done:
	retw.n
	.size	gfsk_pie_z, . - gfsk_pie_z

	.align	4
	.global	gfsk_pie_e
	.type	gfsk_pie_e, @function
gfsk_pie_e:
	entry	a1, 32
	movi.n	a6, 12
	wur.sar_byte	a6
	ee.vld.128.ip	q5, a2, 16		/* z_i of the 8 samples before */
	ee.vld.128.ip	q6, a3, 16		/* z_q of the 8 samples before */
	loopnez	a5, .Le_done
	ee.vld.128.ip	q0, a2, 16		/* z_i */
	ee.vld.128.ip	q1, a3, 16		/* z_q */
	ee.src.q	q2, q5, q0		/* z_i[n-2] */
	ee.src.q	q3, q6, q1		/* z_q[n-2] */
	ee.orq	q5, q0, q0
	ee.orq	q6, q1, q1
	ssai	0
	ee.vmul.s16	q4, q1, q2		/* low halves of z_q z_i[n-2] */
	ee.vmul.s16	q7, q0, q3		/* ... of z_i z_q[n-2] */
	ssai	16
	ee.vmul.s16	q1, q1, q2		/* high halves of z_q z_i[n-2] */
	ee.vmul.s16	q0, q0, q3		/* ... of z_i z_q[n-2] */
	ee.vzip.16	q4, q1			/* z_q z_i[n-2]: lanes 0..3 in q4, 4..7 in q1 */
	ee.vzip.16	q7, q0			/* z_i z_q[n-2]: lanes 0..3 in q7, 4..7 in q0 */
	ee.vsubs.s32	q4, q7, q4		/* z_i z_q[n-2] - z_q z_i[n-2] */
	ee.vsubs.s32	q1, q0, q1
	ssai	8
	ee.vsl.32	q4, q4
	ee.vsl.32	q1, q1
	ee.vst.128.ip	q4, a4, 16
	ee.vst.128.ip	q1, a4, 16
.Le_done:
	retw.n
	.size	gfsk_pie_e, . - gfsk_pie_e
