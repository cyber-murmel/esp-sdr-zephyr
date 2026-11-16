/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Scan app's frame passes on the ESP32-S3's vector unit (esdr_scan.h): eight int16 lanes a
 * step, I in the even lanes and Q in the odd. Uses q0-q7 and SAR; the caller enables the unit
 * and keeps interrupts masked (esdr_scan_pie.c). esdr_scan.c has the same arithmetic in C, which
 * the host tests run.
 *
 * void esdr_scan_minmax_pie(const int16_t *x, int16_t *mm)
 *   x   a frame: ESDR_SCAN_N pairs, 16-byte aligned
 *   mm  16-byte aligned: each lane's largest of the frame (8 int16), then its smallest
 *
 * void esdr_scan_window_pie(const int16_t *x, int16_t *d, const int16_t *w2, const uint32_t *c,
 *                           uint32_t sar)
 *   d = ((x - c) saturated to int16) times w2, shifted right by sar (floor), per lane
 *   x, d, w2  ESDR_SCAN_N pairs, 16-byte aligned; w2 the window, each value twice (I, Q)
 *   c         I in bits 15:0, Q in bits 31:16
 */

	.text
	.align	4
	.global	esdr_scan_minmax_pie
	.type	esdr_scan_minmax_pie, @function
esdr_scan_minmax_pie:
	entry	a1, 32
	ee.vld.128.ip	q0, a2, 16
	mv.qr	q1, q0			/* largest */
	mv.qr	q2, q0			/* smallest */
	movi.n	a8, 63			/* the other 63 of 64 steps */
	loopnez	a8, .Lmm_end
	ee.vld.128.ip	q0, a2, 16
	ee.vmax.s16	q1, q1, q0
	ee.vmin.s16	q2, q2, q0
.Lmm_end:
	ee.vst.128.ip	q1, a3, 16
	ee.vst.128.ip	q2, a3, 16
	retw.n
	.size	esdr_scan_minmax_pie, . - esdr_scan_minmax_pie

	.align	4
	.global	esdr_scan_window_pie
	.type	esdr_scan_window_pie, @function
esdr_scan_window_pie:
	entry	a1, 32
	ee.vldbc.32.ip	q7, a5, 0	/* I, Q, I, Q, ... */
	wsr.sar	a6
	movi.n	a8, 64
	loopnez	a8, .Lw_end
	ee.vld.128.ip	q0, a2, 16
	ee.vld.128.ip	q1, a4, 16
	ee.vsubs.s16	q0, q0, q7
	ee.vmul.s16	q0, q0, q1
	ee.vst.128.ip	q0, a3, 16
.Lw_end:
	retw.n
	.size	esdr_scan_window_pie, . - esdr_scan_window_pie
