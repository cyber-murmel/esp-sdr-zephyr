/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The profiler's tick (esp_sdr_prof.h), inlined into the ring's poll loop and sink calls. Without
 * CONFIG_ESP_SDR_CPU_PROF it is empty, so the ring compiles as before.
 */

#ifndef ESP_SDR_PROF_PRIV_H_
#define ESP_SDR_PROF_PRIV_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/toolchain.h>

#if defined(CONFIG_ESP_SDR_CPU_PROF)

#include <esp_sdr/esp_sdr_xtensa.h>

struct esp_sdr_prof_state {
	/* Set by esp_sdr_prof_start() on any CPU, read by the ring's CPU. */
	volatile bool on;
	/* The ring's CPU takes its first sample at once: next is in its own cycle count. */
	volatile bool fresh;
	uint32_t next;
	uint32_t period;
	volatile uint32_t n;
	uint32_t missed;
	uint8_t cpu;
};

extern struct esp_sdr_prof_state esp_sdr_prof_st;

void esp_sdr_prof_sample(uint32_t now);

static ALWAYS_INLINE void cpu_prof_tick(void)
{
	if (esp_sdr_prof_st.on) {
		uint32_t now = esp_sdr_ccount();

		if (esp_sdr_prof_st.fresh || (int32_t)(now - esp_sdr_prof_st.next) >= 0) {
			esp_sdr_prof_sample(now);
		}
	}
}

#else

static ALWAYS_INLINE void cpu_prof_tick(void)
{
}

#endif

#endif /* ESP_SDR_PROF_PRIV_H_ */
