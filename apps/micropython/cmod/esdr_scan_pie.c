/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Scan app's FFT and the frame passes before it on the ESP32-S3's vector unit (esdr_scan.h):
 * esp-dsp's 16-bit radix-2 kernel (apps/common/esp_dsp.cmake) and esdr_scan_vec.S, which the
 * app's build assembles beside MicroPython.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/arch/cpu.h>

#include <esp_sdr/esp_sdr_xtensa.h>

#include "esdr_scan.h"

int dsps_fft2r_sc16_aes3_(int16_t *data, int n, int16_t *w);
/* esdr_scan_vec.S */
void esdr_scan_minmax_pie(const int16_t *x, int16_t *mm);
void esdr_scan_window_pie(const int16_t *x, int16_t *d, const int16_t *w2, const uint32_t *c,
			  uint32_t sar);

/*
 * What the kernel expects of its surroundings: it checks that its table (here passed in) was
 * initialised and that N is a power of two. Hidden: its call8 cannot take a dynamic symbol (a
 * multiapp extension's).
 */
__attribute__((visibility("hidden"))) uint8_t dsps_fft2r_sc16_initialized = 1;

__attribute__((visibility("hidden"))) int dsp_is_power_of_two(int x)
{
	return x > 0 && (x & (x - 1)) == 0;
}

/*
 * The vector registers are not part of a thread's context: no other thread may run on this CPU
 * while they are live, so interrupts stay masked here for the kernel's 15 to 20 us (3500 to 4900
 * cycles on the XIAO; a scheduler lock would take an SMP kernel's global lock).
 */
void esdr_scan_fft16(int16_t *data, const int16_t *tw)
{
	unsigned int key = arch_irq_lock();

	esp_sdr_pie_enable();
	(void)dsps_fft2r_sc16_aes3_(data, (int)ESDR_SCAN_N, (int16_t *)tw);
	arch_irq_unlock(key);
}

static bool aligned16(const void *p)
{
	return ((uintptr_t)p & 15U) == 0U;
}

/* A frame from a buffer the caller passed: in C where it is not 16-byte aligned. */
void esdr_scan_minmax(const int16_t *x, int16_t *mm)
{
	unsigned int key;

	if (!aligned16(x) || !aligned16(mm)) {
		esdr_scan_minmax_c(x, mm);
		return;
	}
	key = arch_irq_lock();
	esp_sdr_pie_enable();
	esdr_scan_minmax_pie(x, mm);
	arch_irq_unlock(key);
}

void esdr_scan_window(const int16_t *x, int16_t *d, const int16_t *w2, uint32_t c, uint32_t sar)
{
	unsigned int key;

	if (!aligned16(x) || !aligned16(d) || !aligned16(w2)) {
		esdr_scan_window_c(x, d, w2, c, sar);
		return;
	}
	key = arch_irq_lock();
	esp_sdr_pie_enable();
	esdr_scan_window_pie(x, d, w2, &c, sar);
	arch_irq_unlock(key);
}
