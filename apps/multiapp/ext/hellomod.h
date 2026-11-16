/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The test modules hello loads (CONFIG_MULTIAPP_MODULE_TESTS): hellolib in PSRAM
 * calls hellofast in internal RAM, as an app calls a driver module.
 */

#ifndef HELLOMOD_H_
#define HELLOMOD_H_

#include <stdint.h>

#include <zephyr/kernel.h>

/* hellofast: from a global, so the other modules reach it through a data relocation. */
extern int hellofast_value;
/* 2 * v, or -1 if the module's constructor has not run. */
int hellofast_twice(int v);
uint32_t hellofast_loop(uint32_t rounds);

/* hellolib: hellofast_twice(v) + hellofast_value. */
int hellolib_chain(int v);
uint32_t hellolib_loop(uint32_t rounds);
/* How often the host ran the init function of hellolib's device. */
int hellolib_dev_inits(void);

/* The cycles a loop takes, the same code wherever a module runs. */
static inline uint32_t hellomod_loop(uint32_t rounds)
{
	volatile uint32_t sum = 0;
	uint32_t start = k_cycle_get_32();

	for (uint32_t i = 0; i < rounds; i++) {
		sum += i * 3U;
	}
	return k_cycle_get_32() - start;
}

#endif /* HELLOMOD_H_ */
