# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/multiapp/src/stage.c and include/multiapp/stage.h: the swap protocol, run on the host with
threads standing for the app's thread (which syncs) and the shell's (which swaps).

The invariants the protocol exists for: a swap never leaves the old kernel's function in a call when
the kernel is unloaded (an unload with a call inside, or a call into an unloaded kernel, is "bad"); a
stage the app never syncs times out, takes its request back and unloads the new kernel; revert and
the app's unload drop the kernels. stage.c is compiled as it is, against stubs of the kernel calls
and of the loader (apps.c: a kernel here is a record whose function counts its calls); the stage
macro of the header is the app's own. Skipped without a host C compiler."""

import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
MULTIAPP = REPO / "apps/multiapp"

SHIM = {
    "zephyr/device.h": "#pragma once\nstruct device;\n",
    "zephyr/logging/log.h": """
#pragma once
#include <stdio.h>
#define LOG_LEVEL_INF 3
#define LOG_MODULE_DECLARE(...)
#define LOG_ERR(...) do { fprintf(stderr, "ERR: " __VA_ARGS__); fputc('\\n', stderr); } while (0)
#define LOG_WRN(...) do { fprintf(stderr, "WRN: " __VA_ARGS__); fputc('\\n', stderr); } while (0)
""",
    "esp_sdr/esp_sdr_place.h": "#pragma once\n#define ESP_SDR_DATA(cat, nm)\n",
    "zephyr/llext/symbol.h": "#pragma once\n#define EXPORT_SYMBOL(x)\n",
    "zephyr/sys/util.h": "#pragma once\n#define BUILD_ASSERT(c, ...) _Static_assert(c, \"\" __VA_ARGS__)\n",
    "zephyr/kernel.h": r"""
#pragma once
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef struct { int ms; } k_timeout_t;
#define K_FOREVER ((k_timeout_t){-1})
#define K_NO_WAIT ((k_timeout_t){0})
#define K_MSEC(x) ((k_timeout_t){(int)(x)})

#include <unistd.h>
static inline int32_t k_msleep(int32_t ms) { usleep((useconds_t)ms * 1000U); return 0; }

static inline uint32_t k_cycle_get_32(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)(t.tv_sec * 1000000000ULL + t.tv_nsec);
}
static inline uint32_t k_cyc_to_us_floor32(uint32_t c) { return c / 1000U; }

typedef void *k_tid_t;
static inline k_tid_t k_current_get(void) { return (k_tid_t)(uintptr_t)pthread_self(); }

struct k_spinlock { pthread_mutex_t m; };
typedef int k_spinlock_key_t;
static inline k_spinlock_key_t k_spin_lock(struct k_spinlock *l)
{
	pthread_mutex_lock(&l->m);
	return 0;
}
static inline void k_spin_unlock(struct k_spinlock *l, k_spinlock_key_t k)
{
	(void)k;
	pthread_mutex_unlock(&l->m);
}

struct k_mutex { pthread_mutex_t m; };
#define K_MUTEX_DEFINE(name) struct k_mutex name = {PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP}
static inline int k_mutex_lock(struct k_mutex *m, k_timeout_t t) { (void)t; return pthread_mutex_lock(&m->m); }
static inline int k_mutex_unlock(struct k_mutex *m) { return pthread_mutex_unlock(&m->m); }

struct k_sem { pthread_mutex_t m; pthread_cond_t c; unsigned count, limit; };
static inline int k_sem_init(struct k_sem *s, unsigned init, unsigned limit)
{
	pthread_mutex_init(&s->m, NULL);
	pthread_cond_init(&s->c, NULL);
	s->count = init;
	s->limit = limit;
	return 0;
}
static inline void k_sem_give(struct k_sem *s)
{
	pthread_mutex_lock(&s->m);
	if (s->count < s->limit) {
		s->count++;
	}
	pthread_cond_signal(&s->c);
	pthread_mutex_unlock(&s->m);
}
static inline void k_sem_reset(struct k_sem *s)
{
	pthread_mutex_lock(&s->m);
	s->count = 0;
	pthread_mutex_unlock(&s->m);
}
static inline int k_sem_take(struct k_sem *s, k_timeout_t t)
{
	struct timespec end;
	int ret = 0;

	clock_gettime(CLOCK_REALTIME, &end);
	end.tv_sec += t.ms / 1000;
	end.tv_nsec += (t.ms % 1000) * 1000000L;
	if (end.tv_nsec >= 1000000000L) {
		end.tv_sec++;
		end.tv_nsec -= 1000000000L;
	}
	pthread_mutex_lock(&s->m);
	while (s->count == 0 && ret == 0) {
		if (t.ms < 0) {
			pthread_cond_wait(&s->c, &s->m);
		} else if (t.ms == 0) {
			ret = EBUSY;
		} else {
			ret = pthread_cond_timedwait(&s->c, &s->m, &end);
		}
	}
	if (ret == 0) {
		s->count--;
	}
	pthread_mutex_unlock(&s->m);
	return ret ? -EAGAIN : 0;
}
""",
}

HARNESS = r"""
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <multiapp/stage.h>
/* A thread that ended inside a stage: the kernel's _THREAD_DEAD, here a flag the scenario sets. */
static volatile int owner_dead_flag;
static int harness_owner_dead(void *t) { (void)t; return owner_dead_flag; }
#include "stage.c"

/* The app: a stage of an int function, as an app declares it (the macro under test). */
typedef int (*demo_fn_t)(int);

/* Four functions: 0 the app's own, 1..3 kernels'. A call into one that is freed is the bug. */
#define NFN 4
static volatile int freed[NFN], inflight[NFN], bad, spin_us;
static int calls[NFN];

static int enter(int i)
{
	if (freed[i]) {
		__atomic_fetch_add(&bad, 1, __ATOMIC_SEQ_CST);
	}
	__atomic_fetch_add(&inflight[i], 1, __ATOMIC_SEQ_CST);
	__atomic_fetch_add(&calls[i], 1, __ATOMIC_SEQ_CST);
	if (spin_us) {
		usleep(spin_us);
	}
	return i;
}
static void leave(int i) { __atomic_fetch_sub(&inflight[i], 1, __ATOMIC_SEQ_CST); }
#define FN(i) static int fn##i(int v) { int r = enter(i); (void)v; leave(i); return r; }
FN(0) FN(1) FN(2) FN(3)
static demo_fn_t const fns[NFN] = {fn0, fn1, fn2, fn3};

MULTIAPP_STAGE_DEFINE(demo, demo_fn_t, fn0);
/* A stage that wants its kernels in internal RAM (the driver's tx_gen, rfprobe's ring_sink). */
MULTIAPP_STAGE_DEFINE_INTERNAL(fastx, demo_fn_t, fn0);
/* What the app sees of the stage: the object's pointer, as <name>_stage_fn() no longer exists. */
#define CUR() (((struct multiapp_stage *)&demo_stage_obj)->cur)

/* The loader (apps.c): a "file" is two bytes, the kernel's number and what it is built for. */
static struct multiapp_kernel_desc descs[NFN];
static int tokens[NFN], loaded_count, unloads, unload_with_call_inside, load_calls;

int multiapp_kernel_load(const char *name, const uint8_t *file, size_t len, const char *source, struct multiapp_kernel *k)
{
	int i = file[0];

	(void)source;
	memset(k, 0, sizeof(*k));
	if (len < 2 || i < 1 || i >= NFN) {
		return -ENOEXEC;
	}
	load_calls++;
	descs[i] = (struct multiapp_kernel_desc){
		MULTIAPP_KERNEL_MAGIC, file[1] == 1 ? "other" : file[1] >= 10 ? "fastx" : "demo",
		file[1] == 2 ? "int (*)(long)" : "demo_fn_t",
		file[1] == 3 ? MULTIAPP_STAGE_ABI_demo + 1 : MULTIAPP_STAGE_ABI_demo, (void *)fns[i]};
	snprintf(k->name, sizeof(k->name), "ksw%d", i);
	k->ext = (struct llext *)&tokens[i];
	k->desc = &descs[i];
	k->fn = (void *)fns[i];
	k->source = "test";
	/*
	 * Where the loader put it: variant 11 is a PSRAM kernel (no fast region), 12 one that says
	 * INTERNAL but whose function is not in its fast region (code left in .text); the rest is
	 * INTERNAL with the function in the fast region.
	 */
	k->place = file[1] == 11 || file[1] == 4 ? MULTIAPP_PLACE_PSRAM : MULTIAPP_PLACE_INTERNAL;
	if (k->place == MULTIAPP_PLACE_INTERNAL) {
		k->fast = (void *)((uintptr_t)fns[i] - (file[1] == 12 ? 4096 : 16));
		k->fast_size = 64;
	}
	loaded_count++;
	freed[i] = 0;
	return 0;
}

int multiapp_kernel_unload(struct multiapp_kernel *k)
{
	int i = k->name[3] - '0';

	if (k->ext == NULL) {
		return 0;
	}
	/* The moment its memory goes: nobody may be inside it. */
	if (inflight[i]) {
		unload_with_call_inside++;
	}
	freed[i] = 1;
	unloads++;
	loaded_count--;
	k->ext = NULL;
	k->fn = NULL;
	return 0;
}

#define CHECK(cond)                                                                                \
	do {                                                                                       \
		if (!(cond)) {                                                                     \
			printf("FAILED line %d: %s\n", __LINE__, #cond);                           \
			return 1;                                                                  \
		}                                                                                  \
	} while (0)

static const uint8_t file1[] = {1, 0}, file2[] = {2, 0}, file3[] = {3, 0};
static const uint8_t file_other_stage[] = {1, 1}, file_other_type[] = {2, 2}, file_other_abi[] = {3, 3};
static struct multiapp_stage_result res;
static uint8_t buf[2];

static int install_to(const char *stage, const uint8_t *f, uint32_t timeout_ms)
{
	return multiapp_stage_install(stage, "k", f, 2, "test", timeout_ms, &res);
}
static int install(const uint8_t *f, uint32_t timeout_ms)
{
	return install_to("demo", f, timeout_ms);
}

/* The app's thread: sync, call, sleep a little; until stopped; idle at the end. */
static volatile int stop, worker_calls, worker_sync_once;
static void *worker(void *arg)
{
	(void)arg;
	while (!stop) {
		demo_fn_t fn = demo_stage_sync();

		(void)fn(0);
		worker_calls++;
		usleep(200);
	}
	demo_stage_idle();
	return NULL;
}

/* An app thread that syncs once and then sits inside the stage's function for a while. */
static volatile int release_stuck, stuck_in;
static int stuck_fn(int v)
{
	(void)v;
	stuck_in = 1;
	while (!release_stuck) {
		usleep(200);
	}
	return 0;
}
static void *stuck_worker(void *arg)
{
	(void)arg;
	(void)demo_stage_sync();
	(void)stuck_fn(0);
	/* It leaves the function and syncs again: a request that timed out must be gone. */
	(void)demo_stage_sync();
	worker_sync_once = 1;
	while (!stop) {
		usleep(200);
	}
	demo_stage_idle();
	return NULL;
}

static int scenario_idle(void)
{
	/* The app has not synced: nothing is inside, so the swap is made at once. */
	CHECK(multiapp_stage_count() == 1);
	CHECK(CUR() == (void *)fn0);
	CHECK(install(file1, 500) == 0);
	CHECK(res.immediate && res.latency_us < 100000 && !res.replaced);
	CHECK(CUR() == (void *)fn1);
	/* A second kernel replaces the first, which is unloaded after the swap. */
	CHECK(install(file2, 500) == 0);
	CHECK(res.replaced && res.old_ret == 0 && CUR() == (void *)fn2);
	CHECK(freed[1] && !freed[2] && loaded_count == 1);
	/* The app syncs: it gets the new one. */
	CHECK(demo_stage_sync() == fn2);
	return 0;
}

static int scenario_busy(void)
{
	pthread_t t;
	int before;

	pthread_create(&t, NULL, worker, NULL);
	usleep(20000);
	/* Busy: the swap waits for the worker's next sync, and is acked. */
	CHECK(install(file1, 2000) == 0);
	CHECK(!res.immediate && CUR() == (void *)fn1);
	CHECK(install(file2, 2000) == 0);
	CHECK(freed[1] && CUR() == (void *)fn2);
	before = calls[2];
	usleep(20000);
	CHECK(calls[2] > before);
	/* Revert: back on the app's function, the kernel unloaded. */
	CHECK(multiapp_stage_revert("demo", 2000, &res) == 0);
	CHECK(res.reverted && CUR() == (void *)fn0 && freed[2] && loaded_count == 0);
	CHECK(multiapp_stage_revert("demo", 2000, &res) == -EALREADY);
	stop = 1;
	pthread_join(t, NULL);
	CHECK(bad == 0 && unload_with_call_inside == 0);
	return 0;
}

/* The worker's call takes long: the swap is acked only after the call, never during it. */
static int scenario_waits_for_the_call(void)
{
	pthread_t t;

	spin_us = 30000;
	pthread_create(&t, NULL, worker, NULL);
	usleep(10000);
	CHECK(install(file1, 2000) == 0);
	/* Kernel 1's function is in a call now (30 ms long): replacing it waits for the call to end. */
	for (int w = 0; w < 1000 && !inflight[1]; w++) {
		usleep(100);
	}
	CHECK(inflight[1] == 1);
	CHECK(install(file2, 2000) == 0);
	CHECK(unload_with_call_inside == 0 && bad == 0);
	/* Kernel 1 was unloaded: its last call had ended before the ack. */
	CHECK(freed[1] && inflight[1] == 0);
	stop = 1;
	pthread_join(t, NULL);
	return 0;
}

static int scenario_timeout(void)
{
	pthread_t t;

	pthread_create(&t, NULL, stuck_worker, NULL);
	while (!stuck_in) {
		usleep(100);
	}
	/* The app is inside the stage and does not leave: timed out, request gone, kernel unloaded. */
	CHECK(install(file1, 100) == -ETIMEDOUT);
	CHECK(CUR() == (void *)fn0 && freed[1] && loaded_count == 0 && unloads == 1);
	release_stuck = 1;
	while (!worker_sync_once) {
		usleep(100);
	}
	/* Its next sync finds nothing pending: the timed-out request is not applied late. */
	CHECK(CUR() == (void *)fn0);
	stop = 1;
	pthread_join(t, NULL);
	return 0;
}

static int scenario_refusals(void)
{
	CHECK(multiapp_stage_install("nostage", "k", file1, 2, "test", 100, &res) == -ENOENT);
	CHECK(multiapp_stage_revert("nostage", 100, &res) == -ENOENT);
	/* A kernel for another stage, one built for another function type: refused, unloaded. */
	CHECK(install(file_other_stage, 100) == -ENOEXEC);
	CHECK(install(file_other_type, 100) == -ENOEXEC);
	/* One built against another ABI of the stage's headers (a struct that grew): refused too. */
	CHECK(install(file_other_abi, 100) == -ENOEXEC);
	CHECK(loaded_count == 0 && unloads == 3 && CUR() == (void *)fn0);
	/* A file the loader refuses. */
	CHECK(multiapp_stage_install("demo", "k", buf, 1, "test", 100, &res) == -ENOEXEC);
	return 0;
}

/* A thread that calls what the test hands it, once. */
static void (*other_call)(void);
static void *other_thread(void *arg)
{
	(void)arg;
	other_call();
	return NULL;
}
static void call_in_other_thread(void (*fn)(void))
{
	pthread_t t;

	other_call = fn;
	pthread_create(&t, NULL, other_thread, NULL);
	pthread_join(t, NULL);
}
static void other_idle(void) { demo_stage_idle(); }
static void other_sync(void) { (void)demo_stage_sync(); }

/* Another thread's idle does not make the stage quiescent while the owner is still inside. */
static int scenario_second_idle(void)
{
	pthread_t t;

	pthread_create(&t, NULL, stuck_worker, NULL);
	while (!stuck_in) {
		usleep(100);
	}
	call_in_other_thread(other_idle);
	/* Still busy: the swap is not made at once, and not at all while the owner stays inside. */
	CHECK(install(file1, 100) == -ETIMEDOUT);
	CHECK(CUR() == (void *)fn0 && freed[1] && load_calls == 1);
	/* The owner leaves the call and syncs: nothing was pending, the stage is not shared either. */
	release_stuck = 1;
	while (!worker_sync_once) {
		usleep(100);
	}
	stop = 1;
	pthread_join(t, NULL);
	/* Its idle made the stage idle: now a swap is made at once, by a thread that is none of them. */
	CHECK(install(file2, 100) == 0 && res.immediate && CUR() == (void *)fn2);
	CHECK(bad == 0 && unload_with_call_inside == 0);
	return 0;
}

/* A second thread inside at the same time: no swap is safe, and none is attempted. */
static int scenario_shared(void)
{
	pthread_t t;

	pthread_create(&t, NULL, stuck_worker, NULL);
	while (!stuck_in) {
		usleep(100);
	}
	call_in_other_thread(other_sync);
	CHECK(install(file1, 100) == -EBUSY);
	/* Refused before the file is even loaded. */
	CHECK(load_calls == 0 && loaded_count == 0 && CUR() == (void *)fn0);
	release_stuck = 1;
	stop = 1;
	pthread_join(t, NULL);
	CHECK(install(file1, 100) == -EBUSY && load_calls == 0);
	return 0;
}

static void other_sync_idle(void)
{
	(void)demo_stage_sync();
	demo_stage_idle();
}

/* Threads that follow one another are fine; one that dies inside the stage keeps it busy. */
static int scenario_handover(void)
{
	call_in_other_thread(other_sync_idle);
	call_in_other_thread(other_sync_idle);
	call_in_other_thread(other_sync);
	/* The last one left without its idle call: nobody can say it is out, so the swap waits. */
	CHECK(install(file1, 100) == -ETIMEDOUT);
	CHECK(CUR() == (void *)fn0 && freed[1]);
	/* Still busy after that. */
	CHECK(multiapp_stage_install("demo", "k", file2, 2, "test", 100, &res) == -ETIMEDOUT);
	return 0;
}

static int scenario_drop(void)
{
	CHECK(install(file1, 100) == 0);
	CHECK(CUR() == (void *)fn1);
	/* The app has ended and unloads: every kernel goes, the default is back. */
	CHECK(multiapp_stage_drop_kernels() == 0);
	CHECK(CUR() == (void *)fn0 && freed[1] && loaded_count == 0);
	CHECK(multiapp_stage_drop_kernels() == 0);
	return 0;
}

static bool in_test(const void *obj, void *ctx) { (void)ctx; return obj != NULL; }

static int scenario_unregister(void)
{
	struct multiapp_stage *st = (struct multiapp_stage *)&demo_stage_obj;

	CHECK(install(file1, 100) == 0);
	/* The destructor of the app: reverts what runs, forgets the stage. */
	multiapp_stage_unregister(st);
	CHECK(st->cur == (void *)fn0 && multiapp_stage_count() == 0);
	/* The kernel it still had is unloaded, not forgotten. */
	CHECK(freed[1] && loaded_count == 0);
	CHECK(multiapp_stage_install("demo", "k", file2, 2, "test", 100, &res) == -ENOENT);
	/* A stage that registered again, and a failed start's sweep that forgets it. */
	multiapp_stage_register(st);
	CHECK(multiapp_stage_count() == 1);
	multiapp_stage_forget(in_test, NULL);
	CHECK(multiapp_stage_count() == 0);
	/* Unsynced and unregistered: the app's calls fall back to what it has. */
	CHECK(demo_stage_sync() == fn0);
	return 0;
}

static int scenario_view(void)
{
	struct multiapp_stage_view v;

	CHECK(multiapp_stage_view(0, &v) && strcmp(v.name, "demo") == 0);
	CHECK(strcmp(v.sig_text, "demo_fn_t") == 0 && v.sig != 0 && v.cur == v.dflt && !v.has_kernel);
	CHECK(v.idle);
	CHECK(install(file3, 100) == 0);
	CHECK(multiapp_stage_view(0, &v) && v.has_kernel && v.cur == (void *)fn3 && v.swaps == 1);
	CHECK(!multiapp_stage_view(1, &v));
	return 0;
}


/* A stage that asked for internal RAM takes only a kernel whose function is in the internal region. */
static int scenario_internal(void)
{
	static const uint8_t ok[] = {1, 10}, psram[] = {2, 11}, outside[] = {3, 12};
	static const uint8_t psram_for_demo[] = {1, 4};
	struct multiapp_stage *st = (struct multiapp_stage *)&fastx_stage_obj;

	CHECK(!((struct multiapp_stage *)&demo_stage_obj)->internal && st->internal);
	multiapp_stage_register(st);
	CHECK(multiapp_stage_count() == 2);
	/* PSRAM, or code left in .text whatever the trailer says: refused, and unloaded again. */
	CHECK(install_to("fastx", psram, 100) == -ENOEXEC);
	CHECK(install_to("fastx", outside, 100) == -ENOEXEC);
	CHECK(loaded_count == 0 && unloads == 2 && st->cur == (void *)fn0);
	CHECK(install_to("fastx", ok, 100) == 0 && st->cur == (void *)fn1);
	/* The stage that did not ask takes a PSRAM kernel. */
	CHECK(install_to("demo", psram_for_demo, 100) == 0);
	CHECK(((struct multiapp_stage *)&demo_stage_obj)->cur == (void *)fn1);
	return 0;
}

/* The extension that holds a stage's object, and only it, may be a kernel's dependency. */
static bool holds_demo(const struct llext *ext, uintptr_t addr)
{
	return ext == (const struct llext *)&tokens[1] && addr == (uintptr_t)&demo_stage_obj;
}

static int scenario_owner(void)
{
	const struct llext *module = (const struct llext *)&tokens[1];
	const struct llext *other = (const struct llext *)&tokens[2];

	CHECK(multiapp_stage_owned_by("demo", module, holds_demo));
	CHECK(!multiapp_stage_owned_by("demo", other, holds_demo));
	CHECK(!multiapp_stage_owned_by("nostage", module, holds_demo));
	/* The shell's thread holds the registry's mutex while it loads a kernel. */
	k_mutex_lock(&stage_lock, K_FOREVER);
	CHECK(multiapp_stage_owned_by("demo", module, holds_demo));
	k_mutex_unlock(&stage_lock);
	return 0;
}

/* The drop (an app's unload) waits for a thread inside the kernel, and does not unload it under it. */
static pthread_t drop_thread;
static volatile int drop_ret = 12345;
static void *drop_fn(void *arg)
{
	(void)arg;
	drop_ret = multiapp_stage_drop_kernels();
	return NULL;
}

static int scenario_drop_waits(void)
{
	pthread_t t;
	struct multiapp_stage_view v;

	CHECK(install(file1, 100) == 0 && CUR() == (void *)fn1);
	pthread_create(&t, NULL, stuck_worker, NULL);
	while (!stuck_in) {
		usleep(100);
	}
	/* The owner stays inside the kernel's function: the drop gives up, the kernel stays loaded. */
	CHECK(multiapp_stage_drop_kernels() == -EBUSY);
	CHECK(!freed[1] && loaded_count == 1 && unload_with_call_inside == 0);
	/* Nothing new enters it: back on the built-in function, the stage open again (not closed). */
	CHECK(CUR() == (void *)fn0);
	CHECK(multiapp_stage_view(0, &v) && v.has_kernel && !v.shared);
	/* A drop that the owner leaves during the wait unloads the kernel after it left. */
	pthread_create(&drop_thread, NULL, drop_fn, NULL);
	usleep(30000);
	release_stuck = 1;
	while (!worker_sync_once) {
		usleep(100);
	}
	CHECK(drop_ret == 12345);
	stop = 1;
	pthread_join(t, NULL);
	pthread_join(drop_thread, NULL);
	CHECK(drop_ret == 0 && freed[1] && loaded_count == 0 && unload_with_call_inside == 0);
	CHECK(multiapp_stage_view(0, &v) && !v.has_kernel && v.idle);
	return 0;
}

/* Closed: a sync during the drop answers the built-in function and makes nobody the owner. */
static int scenario_drop_closed(void)
{
	pthread_t t;
	struct multiapp_stage_view v;

	CHECK(install(file1, 100) == 0);
	pthread_create(&t, NULL, stuck_worker, NULL);
	while (!stuck_in) {
		usleep(100);
	}
	pthread_create(&drop_thread, NULL, drop_fn, NULL);
	usleep(30000);
	/* The drop is waiting for the owner: another thread's sync gets fn0 and is not "shared". */
	CHECK(drop_ret == 12345);
	call_in_other_thread(other_sync);
	call_in_other_thread(other_idle);
	release_stuck = 1;
	stop = 1;
	pthread_join(t, NULL);
	pthread_join(drop_thread, NULL);
	CHECK(drop_ret == 0 && freed[1] && unload_with_call_inside == 0 && bad == 0);
	/* (The listing waits for the registry's mutex, which the drop holds until it is done.) */
	CHECK(multiapp_stage_view(0, &v) && !v.shared && !v.has_kernel && v.idle);
	return 0;
}

/* A thread that ended inside the stage (an app's thread aborted before the drop) is not waited for. */
static int scenario_drop_dead_owner(void)
{
	CHECK(install(file1, 100) == 0);
	call_in_other_thread(other_sync);
	/* Alive as far as the kernel can tell: nobody leaves, the drop gives up. */
	owner_dead_flag = 0;
	CHECK(multiapp_stage_drop_kernels() == -EBUSY && !freed[1]);
	owner_dead_flag = 1;
	CHECK(multiapp_stage_drop_kernels() == 0 && freed[1] && loaded_count == 0);
	owner_dead_flag = 0;
	return 0;
}

/* Two threads were inside at once: nobody can say that both left, so the kernel stays. */
static int scenario_drop_shared(void)
{
	pthread_t t;

	CHECK(install(file1, 100) == 0);
	pthread_create(&t, NULL, stuck_worker, NULL);
	while (!stuck_in) {
		usleep(100);
	}
	call_in_other_thread(other_sync);
	CHECK(multiapp_stage_drop_kernels() == -EBUSY && !freed[1]);
	release_stuck = 1;
	stop = 1;
	pthread_join(t, NULL);
	CHECK(multiapp_stage_drop_kernels() == -EBUSY && !freed[1]);
	return 0;
}

/* Swaps in a loop against a worker that calls as fast as it can: no call into a freed kernel. */
static int scenario_stress(void)
{
	pthread_t t;

	pthread_create(&t, NULL, worker, NULL);
	/* Until the app's thread is in its loop, the stage is idle and a swap is made at once. */
	while (worker_calls == 0) {
		usleep(100);
	}
	for (int i = 0; i < 400; i++) {
		const uint8_t *f = i % 3 == 0 ? file1 : i % 3 == 1 ? file2 : file3;

		CHECK(install(f, 2000) == 0);
		if (i % 7 == 0) {
			CHECK(multiapp_stage_revert("demo", 2000, &res) == 0);
		}
	}
	stop = 1;
	pthread_join(t, NULL);
	CHECK(bad == 0 && unload_with_call_inside == 0);
	CHECK(worker_calls >= 400);
	return 0;
}

/*
 * The 802.15.4 driver's pattern (lib/ieee802154/ieee154_esp_sdr.c, tx_gen): the stage is the module's, and
 * the threads that call it follow one another through the radio's mutex. The ring thread syncs before
 * a run, calls the function through a trampoline of the module that reads what it synced, and idles
 * when the run ends, before it lets the radio go; a transmitting thread syncs and idles in its turn.
 * A swap is made between two holders, never with a call inside, whatever the trampoline is armed
 * with when a run ends.
 */
static pthread_mutex_t radio = PTHREAD_MUTEX_INITIALIZER;
static demo_fn_t volatile radio_cur = fn0;
static volatile int radio_stop, runs, frames;

static int trampoline(int v) { return radio_cur(v); }

static void *ring_thread(void *arg)
{
	(void)arg;
	while (!radio_stop) {
		demo_fn_t armed = trampoline;

		pthread_mutex_lock(&radio);
		radio_cur = demo_stage_sync();
		/* A run: the burst's builder is called a few times, like the ring does inside it. */
		for (int k = 0; k < 20; k++) {
			(void)armed(0);
			usleep(50);
		}
		/* Idle puts the module's own function back first, so a stray call finds one that stays. */
		radio_cur = fn0;
		demo_stage_idle();
		runs++;
		pthread_mutex_unlock(&radio);
		usleep(100);
	}
	return NULL;
}

static void *tx_thread(void *arg)
{
	(void)arg;
	while (!radio_stop) {
		pthread_mutex_lock(&radio);
		demo_fn_t fn = demo_stage_sync();

		(void)fn(0);
		usleep(100);
		demo_stage_idle();
		frames++;
		pthread_mutex_unlock(&radio);
		usleep(150);
	}
	return NULL;
}

static int scenario_radio_holders(void)
{
	pthread_t ring, tx;

	pthread_create(&ring, NULL, ring_thread, NULL);
	pthread_create(&tx, NULL, tx_thread, NULL);
	while (runs == 0 || frames == 0) {
		usleep(100);
	}
	for (int i = 0; i < 200; i++) {
		const uint8_t *f = i % 3 == 0 ? file1 : i % 3 == 1 ? file2 : file3;

		CHECK(install(f, 2000) == 0);
		if (i % 5 == 0) {
			CHECK(multiapp_stage_revert("demo", 2000, &res) == 0);
		}
		/* Let the holders take their turns between swaps. */
		usleep(300);
	}
	/* Both held the radio, and called whatever was installed, all through the loop. */
	for (int w = 0; w < 5000 && (runs < 50 || frames < 50); w++) {
		usleep(1000);
	}
	radio_stop = 1;
	pthread_join(ring, NULL);
	pthread_join(tx, NULL);
	CHECK(bad == 0 && unload_with_call_inside == 0);
	CHECK(runs >= 50 && frames >= 50);
	/* Two holders never made the stage "shared": it still swaps. */
	CHECK(install(file1, 100) == 0 && res.immediate);
	return 0;
}

int main(int argc, char **argv)
{
	const char *which = argc > 1 ? argv[1] : "";
	static const struct { const char *name; int (*fn)(void); } all[] = {
		{"idle", scenario_idle}, {"busy", scenario_busy},
		{"call", scenario_waits_for_the_call}, {"timeout", scenario_timeout},
		{"refusals", scenario_refusals}, {"drop", scenario_drop},
		{"unregister", scenario_unregister}, {"view", scenario_view},
		{"second_idle", scenario_second_idle}, {"shared", scenario_shared},
		{"handover", scenario_handover},
		{"stress", scenario_stress}, {"radio_holders", scenario_radio_holders},
		{"internal", scenario_internal}, {"owner", scenario_owner},
		{"drop_waits", scenario_drop_waits}, {"drop_closed", scenario_drop_closed},
		{"drop_dead_owner", scenario_drop_dead_owner}, {"drop_shared", scenario_drop_shared},
	};

	/* The stage that wants internal RAM is the "internal" scenario's own: the others count one. */
	multiapp_stage_unregister((struct multiapp_stage *)&fastx_stage_obj);

	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		if (strcmp(which, all[i].name) == 0) {
			int ret = all[i].fn();

			if (ret == 0) {
				printf("ok\n");
			}
			return ret;
		}
	}
	printf("no scenario %s\n", which);
	return 2;
}
"""


@pytest.fixture(scope="module")
def harness(tmp_path_factory):
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        pytest.skip("no host C compiler")
    work = tmp_path_factory.mktemp("stage")
    for rel, text in SHIM.items():
        path = work / "shim" / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    src = work / "harness.c"
    src.write_text(HARNESS)
    exe = work / "harness"
    # MULTIAPP_NAME: the macro of an app's stage (constructor and destructor included) is the
    # one under test; stage.c itself does not care.
    subprocess.run([cc, "-O1", "-Wall", "-pthread", "-D_GNU_SOURCE", "-DMULTIAPP_NAME=demo",
                    "-DCONFIG_MULTIAPP_STAGES=1", "-DMULTIAPP_STAGE_ABI_demo=0x1234abcdU",
                    "-DMULTIAPP_STAGE_ABI_fastx=0x1234abcdU", "-DMULTIAPP_STAGE_QUIET_MS=150",
                    "-DMULTIAPP_STAGE_OWNER_DEAD(t)=harness_owner_dead(t)", "-I", str(work / "shim"),
                    "-I", str(MULTIAPP / "include"), "-I", str(MULTIAPP / "src"),
                    "-o", str(exe), str(src)], check=True)
    return exe


def run(harness, scenario):
    out = subprocess.run([harness, scenario], capture_output=True, text=True, timeout=60)
    assert out.returncode == 0, out.stdout + out.stderr
    assert out.stdout.strip() == "ok"


@pytest.mark.parametrize("scenario", ["idle", "busy", "call", "timeout", "refusals", "drop",
                                      "unregister", "view", "second_idle", "shared", "handover",
                                      "internal", "owner"])
def test_swap_protocol(harness, scenario):
    run(harness, scenario)


@pytest.mark.parametrize("scenario", ["drop_waits", "drop_closed", "drop_dead_owner", "drop_shared"])
def test_the_drop_of_a_stage_that_a_thread_is_inside_leaves_the_kernel_loaded(harness, scenario):
    # The module's stage (the driver's tx_gen) has threads that outlive the app: the drop closes
    # the stage, waits a bounded time, and fails instead of unloading a kernel with a call inside.
    run(harness, scenario)


def test_a_swap_loop_never_calls_a_freed_kernel(harness):
    run(harness, "stress")


def test_a_stage_handed_on_between_radio_holders_swaps_between_them(harness):
    # The 802.15.4 driver's tx_gen: a module's stage, owners that follow one another.
    run(harness, "radio_holders")


def test_standalone_macros_are_plain_calls(tmp_path):
    # Without MULTIAPP_NAME the macros reduce to the default: no object, no registration, so a
    # standalone image is the code it was before the stage was declared.
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        pytest.skip("no host C compiler")
    src = tmp_path / "plain.c"
    src.write_text("""
#include <multiapp/stage.h>
typedef int (*op_t)(int);
static int op(int v) { return v + 1; }
MULTIAPP_STAGE_DEFINE(demo, op_t, op);
int run(int v) { op_t f = demo_stage_sync(); int r = f(v); demo_stage_idle(); return r; }
""")
    asm = tmp_path / "plain.s"
    subprocess.run([cc, "-O2", "-S", "-Wall", "-Werror", "-Wpedantic", "-I", str(MULTIAPP / "include"),
                    "-o", str(asm), str(src)], check=True)
    text = asm.read_text()
    assert "multiapp_stage" not in text and "constructor" not in text and ".init_array" not in text
