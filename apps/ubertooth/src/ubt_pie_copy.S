/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * void ubt_pie_copy(void *dst, const void *src, uint32_t len);
 *
 * len bytes, a multiple of 16, both pointers 16-byte aligned, through the vector unit (q0, q1):
 * the ring reader's copy of the 8-bit planes (memcpy() is the ROM's, a byte at a time). Zephyr
 * neither enables nor saves the unit: the caller locks interrupts and calls esp_sdr_pie_enable()
 * first. In IRAM, as the reader is.
 */

#include <esp_sdr/esp_sdr_place.h>

	ESP_SDR_ASM_SECTION(CHUNK, ubt_pie_copy)
	.align	4
	.global	ubt_pie_copy
	.type	ubt_pie_copy, @function
ubt_pie_copy:
	entry	a1, 32
	srli	a4, a4, 4		/* vectors */
	bbci	a4, 0, .Lcopy_pairs
	ee.vld.128.ip	q0, a3, 16
	ee.vst.128.ip	q0, a2, 16
.Lcopy_pairs:
	srli	a4, a4, 1
	loopnez	a4, .Lcopy_done
	ee.vld.128.ip	q0, a3, 16
	ee.vld.128.ip	q1, a3, 16
	ee.vst.128.ip	q0, a2, 16
	ee.vst.128.ip	q1, a2, 16
.Lcopy_done:
	retw.n
	.size	ubt_pie_copy, . - ubt_pie_copy
