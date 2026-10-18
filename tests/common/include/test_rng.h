/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Reproducible random numbers for the unit tests: xorshift64 (shifts 13, 7, 17), the top 32
 * bits per draw, and Box-Muller normal deviates. Each test seeds the state itself, usually
 * before every case, so that a case sees the same numbers whatever runs before it.
 */

#ifndef TEST_RNG_H_
#define TEST_RNG_H_

#include <math.h>
#include <stdint.h>

#include <esp_sdr/esp_sdr_dsp.h>

static uint64_t test_rng_state;

static inline void test_rng_seed(uint64_t seed)
{
	test_rng_state = seed;
}

/* The next 32 random bits. */
static inline uint32_t test_rng_u32(void)
{
	test_rng_state ^= test_rng_state << 13;
	test_rng_state ^= test_rng_state >> 7;
	test_rng_state ^= test_rng_state << 17;
	return (uint32_t)(test_rng_state >> 32);
}

/* Uniform in (0, 1): never 0, so that log() stays finite. */
static inline double test_rng_uniform(void)
{
	return (test_rng_u32() + 1.0) / 4294967297.0;
}

/* A standard normal deviate. */
static inline double test_rng_gauss(void)
{
	return sqrt(-2 * log(test_rng_uniform())) * cos(ESP_SDR_TAU * test_rng_uniform());
}

#endif /* TEST_RNG_H_ */
