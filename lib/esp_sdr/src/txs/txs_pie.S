/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ESP32-S31 PIE (xespv2p2, CPU 1 only): the 8+8 bit payload to DAC words, 16 samples an iteration:
 * (I & 0xff) << 2 | (Q & 0xff) << 12 for the 16 bit value I | Q << 8, as txs_iq8_word(). Assembled
 * by Espressif's assembler from the preprocessed file (apps/txstream_eth/CMakeLists.txt), the rest
 * of the image by Zephyr's. After rfprobe's grp pie (wt/s31-dma), measured there at 2.0 cycles a
 * sample for a source at 0 mod 4 and 2.4 at 2 mod 4, no hardware loop.
 *
 *   vldext.u8 x2     16 bytes to two vectors of u16: I0 Q0 I1 Q1 ... in two registers
 *   vunzip.16        I0..I7 in the first, Q0..Q7 in the second
 *   zero + vzip.16   I (Q) halfwords against zero: the words I0..I3 and I4..I7
 *   vsl.32           by 2 (I) and 12 (Q), two SAR writes for the whole iteration
 *   orq, vst.128 x4
 */

#include <esp_sdr/esp_sdr_place.h>

/* uint32_t txs_pie_enable(void): CSR 0x7f2 on, unaligned access on in the PIE cfg; returns the cfg */
	ESP_SDR_ASM_SECTION(PACED_SAMPLE, txs_pie_enable)
	.align	2
	.global	txs_pie_enable
	.type	txs_pie_enable, @function
txs_pie_enable:
	li	a0, 1
	csrw	0x7f2, a0
	esp.movx.r.cfg	a1
	ori	a1, a1, 2
	esp.movx.w.cfg	a1
	esp.movx.r.cfg	a0
	ret
	.size	txs_pie_enable, . - txs_pie_enable

/* void txs_pie_expand(uint32_t *dst, const uint8_t *src, uint32_t blocks16)  a0, a1, a2 */
	ESP_SDR_ASM_SECTION(PACED_SAMPLE, txs_pie_expand)
	.align	2
	.global	txs_pie_expand
	.type	txs_pie_expand, @function
txs_pie_expand:
	li	a3, 2
	li	a4, 12
1:
	esp.vldext.u8.ip	q0, q1, a1, 16
	esp.vldext.u8.ip	q2, q3, a1, 16
	esp.vunzip.16	q0, q1
	esp.vunzip.16	q2, q3
	esp.zero.q	q4
	esp.zero.q	q5
	esp.zero.q	q6
	esp.zero.q	q7
	esp.vzip.16	q0, q4
	esp.vzip.16	q1, q5
	esp.vzip.16	q2, q6
	esp.vzip.16	q3, q7
	esp.movx.w.sar	a3
	esp.vsl.32	q0, q0
	esp.vsl.32	q4, q4
	esp.vsl.32	q2, q2
	esp.vsl.32	q6, q6
	esp.movx.w.sar	a4
	esp.vsl.32	q1, q1
	esp.vsl.32	q5, q5
	esp.vsl.32	q3, q3
	esp.vsl.32	q7, q7
	esp.orq	q0, q0, q1
	esp.orq	q4, q4, q5
	esp.orq	q2, q2, q3
	esp.orq	q6, q6, q7
	esp.vst.128.ip	q0, a0, 16
	esp.vst.128.ip	q4, a0, 16
	esp.vst.128.ip	q2, a0, 16
	esp.vst.128.ip	q6, a0, 16
	addi	a2, a2, -1
	bnez	a2, 1b
	ret
	.size	txs_pie_expand, . - txs_pie_expand

/* void txs_pie_copy(uint32_t *dst, const uint8_t *src, uint32_t blocks8)  a0, a1, a2
 * Ready DAC words: 8 words (32 bytes) an iteration; the source may be at 2 mod 4 (unaligned access
 * is on after txs_pie_enable()), and so may the destination: packets start at multiples of 351 words
 * and a step ends wherever the clock cuts it, so it is only 4 byte aligned (the 128 bit stores rely
 * on the same unaligned mode). Calibration checks an aligned destination only; `tx tone ... verify 1`
 * reads the written window back.
 */
	ESP_SDR_ASM_SECTION(PACED_SAMPLE, txs_pie_copy)
	.align	2
	.global	txs_pie_copy
	.type	txs_pie_copy, @function
txs_pie_copy:
1:
	esp.vld.128.ip	q0, a1, 16
	esp.vld.128.ip	q1, a1, 16
	esp.vst.128.ip	q0, a0, 16
	esp.vst.128.ip	q1, a0, 16
	addi	a2, a2, -1
	bnez	a2, 1b
	ret
	.size	txs_pie_copy, . - txs_pie_copy
