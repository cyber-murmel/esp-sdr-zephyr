/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See esp_sdr_prof.h. esp_sdr_prof_sample() runs on the ring's CPU with interrupts masked, once a
 * period from the ring loop: paced, as the loop is.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/kernel_structs.h>
#include <zephyr/sys/barrier.h>

#include <soc/assist_debug_reg.h>
#include <soc/soc.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_place.h>
#include <esp_sdr/esp_sdr_prof.h>
#include <esp_sdr/esp_sdr_xtensa.h>

#include "esp_sdr_prof_priv.h"

ESP_SDR_BUF_DEFINE(BULK, struct esp_sdr_prof_rec, prof_buf, [CONFIG_ESP_SDR_CPU_PROF_SAMPLES], 4,
		   NONE);

struct esp_sdr_prof_state esp_sdr_prof_st;

ESP_SDR_CODE(PACED_CHUNK, esp_sdr_prof_sample)
__attribute__((noinline)) void esp_sdr_prof_sample(uint32_t now)
{
	struct esp_sdr_prof_state *s = &esp_sdr_prof_st;
	uint32_t n = s->n;
	uint32_t late;
	const struct _cpu *c;

	if (s->fresh) {
		s->fresh = false;
		s->next = now;
		s->cpu = arch_curr_cpu()->id == 0U ? 1U : 0U;
	}
	late = now - s->next;
	if (late >= s->period) {
		s->missed += late / s->period;
	}
	s->next = now + s->period;
	if (n >= CONFIG_ESP_SDR_CPU_PROF_SAMPLES) {
		s->on = false;
		return;
	}
	c = &_kernel.cpus[s->cpu];
	prof_buf[n].pc = REG_READ(s->cpu == 0U ? ASSIST_DEBUG_CORE_0_RCD_PDEBUGPC_REG
					       : ASSIST_DEBUG_CORE_1_RCD_PDEBUGPC_REG);
	prof_buf[n].th = (uint32_t)(uintptr_t)c->current | (c->nested != 0U ? 1U : 0U);
	prof_buf[n].t = now;
	s->n = n + 1U;
}

int esp_sdr_prof_start(uint32_t period_us)
{
	struct esp_sdr_prof_state *s = &esp_sdr_prof_st;

	if (period_us == 0U || period_us > 1000000U) {
		return -EINVAL;
	}
	s->on = false;
	/* Set only: the 802.15.4 driver's stall catcher reads the recorders too. */
	REG_WRITE(ASSIST_DEBUG_CORE_0_RCD_PDEBUGENABLE_REG, 1);
	REG_WRITE(ASSIST_DEBUG_CORE_0_RCD_RECORDING_REG, 1);
	REG_WRITE(ASSIST_DEBUG_CORE_1_RCD_PDEBUGENABLE_REG, 1);
	REG_WRITE(ASSIST_DEBUG_CORE_1_RCD_RECORDING_REG, 1);
	s->period = period_us * (sys_clock_hw_cycles_per_sec() / 1000000U);
	s->n = 0;
	s->missed = 0;
	s->cpu = 0xff;
	s->fresh = true;
	barrier_dmem_fence_full();
	s->on = true;
	return 0;
}

void esp_sdr_prof_stop(void)
{
	esp_sdr_prof_st.on = false;
}

void esp_sdr_prof_info(struct esp_sdr_prof_info *info)
{
	const struct esp_sdr_prof_state *s = &esp_sdr_prof_st;

	info->n = s->n;
	info->cap = CONFIG_ESP_SDR_CPU_PROF_SAMPLES;
	info->period_cyc = s->period;
	info->missed = s->missed;
	info->cpu = s->cpu;
	info->on = s->on;
}

struct esp_sdr_prof_rec *esp_sdr_prof_samples(void)
{
	return prof_buf;
}
