/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file
 * @brief ESP32-S3 CPU helpers shared by the library and the apps: the cycle counter and the
 * vector unit (PIE).
 */

#ifndef ESP_SDR_ESP_SDR_XTENSA_H_
#define ESP_SDR_ESP_SDR_XTENSA_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CONFIG_SOC_SERIES_ESP32S3)
/** @return This CPU's cycle counter (CCOUNT). */
static inline __attribute__((always_inline)) uint32_t esp_sdr_ccount(void)
{
	uint32_t c;

	__asm__ volatile("rsr.ccount %0" : "=r"(c));
	return c;
}

/**
 * @brief Enable the vector unit, coprocessor 3, on this CPU.
 *
 * Zephyr neither enables nor saves it: while a thread's vector registers are live, no other
 * thread may use them on its CPU.
 */
static inline void esp_sdr_pie_enable(void)
{
	uint32_t cp;

	__asm__ volatile("rsr.cpenable %0" : "=r"(cp));
	cp |= 1U << 3;
	__asm__ volatile("wsr.cpenable %0\n\trsync" : : "r"(cp));
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* ESP_SDR_ESP_SDR_XTENSA_H_ */
