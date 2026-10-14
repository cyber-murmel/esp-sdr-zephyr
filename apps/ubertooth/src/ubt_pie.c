/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The 256 point 16 bit FFT of the spectrum analyser on the ESP32-S3 vector unit: the esp-dsp kernel
 * dsps_fft2r_sc16_aes3_() with the contract ubt_specan.h states (natural order in, bit reversed out,
 * every stage halves) and the twiddle table of ubt_specan_w256.
 */

#include <stdint.h>

#include <zephyr/kernel.h>

#include "ubt_pie.h"
#include "ubt_specan.h"

extern int dsps_fft2r_sc16_aes3_(int16_t *data, int N, int16_t *w);

/* The twiddle table the kernel reads: a RAM copy of ubt_specan_w256 (set by ubt_pie_init()). */
static int16_t *w256;

/* What the esp-dsp 16 bit kernel expects of its surroundings: its table is ours, passed in. */
uint8_t dsps_fft2r_sc16_initialized = 1;

__attribute__((visibility("hidden"))) int dsp_is_power_of_two(int x)
{
	return x > 0 && (x & (x - 1)) == 0;
}

static inline void pie_enable(void)
{
	uint32_t cp;

	__asm__ volatile("rsr.cpenable %0" : "=r"(cp));
	cp |= 1U << 3;
	__asm__ volatile("wsr.cpenable %0\n\trsync" : : "r"(cp));
}

void ubt_pie_init(int16_t *w_ram)
{
	for (unsigned int i = 0; i < UBT_SPECAN_N; i++) {
		w_ram[i] = ubt_specan_w256[i];
	}
	w256 = w_ram;
}

/*
 * The vector registers are not part of the thread context: no other thread may run on this CPU while
 * they are live, so interrupts stay masked for the 10 us of one transform.
 */
void ubt_pie_fft256(int16_t *iq)
{
	unsigned int key = arch_irq_lock();

	pie_enable();
	(void)dsps_fft2r_sc16_aes3_(iq, (int)UBT_SPECAN_N, w256);
	arch_irq_unlock(key);
}
