/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A sampling profiler (CONFIG_APP_PROF): at every kernel tick a timer's callback, which runs in
 * the tick interrupt, stores the PC that interrupt left and the return address register then, from
 * the frame the interrupt entry saved (CPU 0's: the tick is CPU 0's under SMP too). In a leaf such
 * as memcpy() the return address names the caller. A PC inside an interrupt-locked stretch shows
 * up where the lock ends. The samples sit in PSRAM and outlive soft resets, so a run started in
 * the REPL covers an app the launcher starts; tools/prof.py reads them and maps them to the
 * image's functions.
 */

#include <string.h>

#include <zephyr/arch/cpu.h>
#include <zephyr/kernel.h>
#include <zephyr/kernel_structs.h>
#include <zephyr/sys/util.h>
#include <esp_sdr/esp_sdr_place.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "esdr_defs.h"
#include "esdr_prof.h"

/* Pairs of words: the PC, the return address. */
#define PROF_SAMPLES 32768U
#define PROF_READ_MAX 2048U

static ESP_SDR_DATA(BULK, samples) uint32_t samples[2U * PROF_SAMPLES];
static volatile uint32_t taken;

static void prof_tick(struct k_timer *timer)
{
	_cpu_t *cpu = arch_curr_cpu();
	uint32_t pc, ra = 0U;

	if (cpu->nested == 1U) {
		/* A thread was interrupted: the entry left its stack pointer, the frame, there. */
		const struct arch_esf *esf =
			*(const struct arch_esf *const *)((uintptr_t)cpu->irq_stack - 16U);

		pc = (uint32_t)esf->mepc;
		ra = (uint32_t)esf->ra;
	} else {
		__asm__ volatile("csrr %0, mepc" : "=r"(pc));
	}
	if (taken < PROF_SAMPLES) {
		samples[2U * taken] = pc;
		samples[2U * taken + 1U] = ra;
		taken = taken + 1U;
	} else {
		k_timer_stop(timer);
	}
}

static K_TIMER_DEFINE(prof_timer, prof_tick, NULL);

/* prof_start(): from no samples, one a tick until PROF_SAMPLES or prof_stop(). */
static mp_obj_t esdr_prof_start_fn(void)
{
	k_timer_stop(&prof_timer);
	taken = 0U;
	k_timer_start(&prof_timer, K_TICKS(1), K_TICKS(1));
	return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(esdr_prof_start_obj, esdr_prof_start_fn);

/* prof_stop() -> the number of samples, and the tick rate: (n, hz). */
static mp_obj_t esdr_prof_stop_fn(void)
{
	mp_obj_t t[2];

	k_timer_stop(&prof_timer);
	t[0] = mp_obj_new_int_from_uint(taken);
	t[1] = mp_obj_new_int_from_uint(CONFIG_SYS_CLOCK_TICKS_PER_SEC);
	return mp_obj_new_tuple(2, t);
}
MP_DEFINE_CONST_FUN_OBJ_0(esdr_prof_stop_obj, esdr_prof_stop_fn);

/* prof_read(first, count) -> bytes: samples [first, first + count), each the PC and the return
 * address as little-endian words. */
static mp_obj_t esdr_prof_read_fn(mp_obj_t first_in, mp_obj_t count_in)
{
	uint32_t first = (uint32_t)mp_obj_get_int(first_in);
	uint32_t count = (uint32_t)mp_obj_get_int(count_in);
	uint32_t have = taken;

	if (first >= have) {
		return mp_obj_new_bytes(NULL, 0);
	}
	count = MIN(MIN(count, PROF_READ_MAX), have - first);
	return mp_obj_new_bytes((const byte *)&samples[2U * first], 2U * count * sizeof(uint32_t));
}
MP_DEFINE_CONST_FUN_OBJ_2(esdr_prof_read_obj, esdr_prof_read_fn);

#if defined(CONFIG_SMP)
/*
 * Who takes the SMP global lock (irq_lock()) on CPU 0, how long it waited for it and held it
 * (cycles): the image links with --wrap for both functions (micropython.cmake).
 */
#define LOCK_SLOTS 24U

unsigned int __real_z_smp_global_lock(void);
void __real_z_smp_global_unlock(unsigned int key);

static struct {
	uint32_t ra, calls;
	uint64_t wait, held;
} locks[LOCK_SLOTS];
static uint32_t lock_t0, lock_slot;
static volatile bool locks_on;

static inline uint32_t prof_cycles(void)
{
	uint32_t v;

	__asm__ volatile("csrr %0, mcycle" : "=r"(v));
	return v;
}

unsigned int __wrap_z_smp_global_lock(void)
{
	uint32_t ra = (uint32_t)(uintptr_t)__builtin_return_address(0);
	uint32_t t0 = prof_cycles();
	unsigned int key = __real_z_smp_global_lock();
	uint32_t t1 = prof_cycles();

	if (locks_on && arch_curr_cpu()->id == 0U && _current->base.global_lock_count == 1U) {
		uint32_t k;

		for (k = 0; k < LOCK_SLOTS && locks[k].ra != ra && locks[k].ra != 0U; k++) {
		}
		if (k < LOCK_SLOTS) {
			locks[k].ra = ra;
			locks[k].calls++;
			locks[k].wait += t1 - t0;
			lock_slot = k;
			lock_t0 = t1;
		}
	}
	return key;
}

void __wrap_z_smp_global_unlock(unsigned int key)
{
	if (locks_on && arch_curr_cpu()->id == 0U && _current->base.global_lock_count == 1U &&
	    lock_slot < LOCK_SLOTS) {
		locks[lock_slot].held += prof_cycles() - lock_t0;
		lock_slot = LOCK_SLOTS;
	}
	__real_z_smp_global_unlock(key);
}

/* prof_locks(on) -> list of (return address, calls, wait cycles, held cycles); on clears. */
static mp_obj_t esdr_prof_locks_fn(mp_obj_t on_in)
{
	mp_obj_t out = mp_obj_new_list(0, NULL);

	for (uint32_t k = 0; k < LOCK_SLOTS && locks[k].ra != 0U; k++) {
		mp_obj_t t[4] = {
			mp_obj_new_int_from_uint(locks[k].ra),
			mp_obj_new_int_from_uint(locks[k].calls),
			mp_obj_new_int_from_ull(locks[k].wait),
			mp_obj_new_int_from_ull(locks[k].held),
		};

		mp_obj_list_append(out, mp_obj_new_tuple(4, t));
	}
	if (mp_obj_is_true(on_in)) {
		locks_on = false;
		memset(locks, 0, sizeof(locks));
		lock_slot = LOCK_SLOTS;
		locks_on = true;
	} else {
		locks_on = false;
	}
	return out;
}
MP_DEFINE_CONST_FUN_OBJ_1(esdr_prof_locks_obj, esdr_prof_locks_fn);
#endif /* CONFIG_SMP */
