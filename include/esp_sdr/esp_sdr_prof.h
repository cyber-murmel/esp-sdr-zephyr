/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file
 * @brief Profile of the other CPU, sampled by the receive ring's CPU (ESP32-S3).
 */

#ifndef ESP_SDR_PROF_H_
#define ESP_SDR_PROF_H_

#include <stdbool.h>
#include <stdint.h>

/**
 * @addtogroup esp_sdr_prof
 * @{
 *
 * While esp_sdr_ring_run() runs, its CPU reads the other CPU's program counter from the debug
 * assist's PC recorder, the other CPU's current thread and whether it is in an interrupt, once a
 * period, into PSRAM. The ring checks the period in its poll loop and before every sink call,
 * with interrupts masked, so a sample lands at most one slice of filter work late. A period that
 * passes without a check is counted as missed, and the next sample comes one period after a late
 * one: no catch-up. The samples carry the sampling CPU's cycle count, to check the cadence.
 *
 * The sampled CPU takes no interrupt and runs no code for a sample (the samples reach PSRAM
 * through the data cache both CPUs share), and the code it runs with interrupts masked is sampled
 * like the rest. There are no stacks, and no samples while the ring is stopped; for those, Zephyr's
 * perf (subsys/profiling/perf) samples stacks in a timer interrupt, and its record command's CPU
 * mask can leave the ring's CPU out.
 */

/** One sample. */
struct esp_sdr_prof_rec {
	/** The sampled CPU's program counter. */
	uint32_t pc;
	/** Its current thread (`struct k_thread *`); bit 0 set: it was in an interrupt. */
	uint32_t th;
	/** The sampling CPU's cycle count. */
	uint32_t t;
};

/** State of a capture. */
struct esp_sdr_prof_info {
	/** Samples taken. */
	uint32_t n;
	/** Samples the buffer holds (CONFIG_ESP_SDR_CPU_PROF_SAMPLES). */
	uint32_t cap;
	/** Sampling period in CPU cycles. */
	uint32_t period_cyc;
	/** Periods that passed without a sample. */
	uint32_t missed;
	/** The CPU sampled; 0xff before the first sample. */
	uint8_t cpu;
	/** Sampling armed (it stops by itself when the buffer is full). */
	bool on;
};

/**
 * @brief Clear the samples and arm sampling.
 *
 * Also enables both CPUs' PC recorders (never disables them: other users rely on them).
 *
 * @param period_us Sampling period in microseconds.
 * @retval 0 Armed; samples come while the receive ring runs.
 * @retval -EINVAL period_us is 0 or above 1000000.
 */
int esp_sdr_prof_start(uint32_t period_us);

/** @brief Stop sampling. The samples stay until the next start. */
void esp_sdr_prof_stop(void);

/**
 * @brief The state of the capture.
 * @param info Filled in.
 */
void esp_sdr_prof_info(struct esp_sdr_prof_info *info);

/**
 * @brief The samples, in the order taken.
 *
 * Valid while sampling is stopped; the caller may reorder them in place.
 *
 * @return esp_sdr_prof_info().n samples.
 */
struct esp_sdr_prof_rec *esp_sdr_prof_samples(void);

/** @} */

#endif /* ESP_SDR_PROF_H_ */
