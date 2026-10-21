/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Definitions the esdr module's sources share, MicroPython's and the plain Zephyr ones
 * (esdr_remote.c, esdr_local.c, esdr_gl.c) alike: no MicroPython headers here.
 */

#ifndef ESDR_DEFS_H
#define ESDR_DEFS_H

#include <math.h>
#include <stdint.h>

#include <esp_attr.h>

/*
 * A static buffer in PSRAM. EXT_RAM_BSS_ATTR is empty on the ESP32-P4 (the HAL enables
 * SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY for the ESP32, S2 and S3 only), whose linker script puts
 * .ext_ram.bss in PSRAM all the same: named directly there, so that the SRAM stays free for the
 * kernel, the drivers and LVGL's draw buffers.
 */
#if defined(CONFIG_SOC_SERIES_ESP32P4)
#define ESDR_PSRAM __attribute__((section(".ext_ram.bss")))
#else
#define ESDR_PSRAM EXT_RAM_BSS_ATTR
#endif

/* The transmit generator's waveforms: gnuradio.analog's (GR_CONST_WAVE and so on, 100 + n). */
enum esdr_wave {
	ESDR_WAVE_CONST,
	ESDR_WAVE_SIN,
	ESDR_WAVE_COS,
	ESDR_WAVE_SQR,
	ESDR_WAVE_TRI,
	ESDR_WAVE_SAW,
};

/* Generator settings: @p cycles is the frequency over the rate, @p amp and the offsets in full
 * scale. */
struct esdr_gen {
	enum esdr_wave wave;
	float cycles, amp, dc_i, dc_q;
};

/*
 * @p hz as the LO takes it (esp_sdr_set_freq(), esp_sdr_set_freq_offset()): the nearest whole
 * MHz and the rest in kHz, rounded.
 */
static inline void esdr_split_hz(double hz, uint32_t *mhz, int32_t *khz)
{
	*mhz = (uint32_t)llround(hz / 1e6);
	*khz = (int32_t)llround((hz - (double)*mhz * 1e6) / 1e3);
}

#endif /* ESDR_DEFS_H */
