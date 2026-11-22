# SPDX-License-Identifier: GPL-3.0-or-later
"""The FAT volume as a USB drive while an app runs (apps/multiapp/src/drive_proxy.c, profile.h).

Two halves. The state machine that hands the volume between the PC (USB mass storage) and the
device is compiled as it is against a stub of the kernel calls it makes, with threads standing for
the MSC thread and for the device's users, and a scripted PC where the timing is the point. The
clock is the test's: it moves when someone sleeps.

- A read (an app's or a kernel file) does not hide the medium. It waits for the PC's command in
  flight (no disk access running and 100 ms since the last), makes the PC's next accesses wait, and
  lets them go on after it: a copy is delayed, never cut, even if the PC stalls in the middle of a
  command for longer than the quiet time. A PC that never pauses starves the read (-EBUSY).
- A write hides the medium from the start of its wait, refuses the PC's accesses while it has the
  volume (a command that stalls past the quiet time is cut), and keeps the medium away at least
  three seconds from when it went, longer while a ring runs, so that the PC's rescan does not land
  in a stream.
- The USB thread never claims (-EDEADLK), claims nest and give up on a timeout with the state as
  it was.

The USB side is arithmetic. Every boot's classes take IN endpoints first fit in registration
order, and an endpoint takes bulk only with a FIFO of at least one packet: the FIFO tables of the
controllers (the S31's overlay, the S3's devicetree) and the profile each app gets
(`multiapp_usb_profile()`) are checked against that, so that the drive is in every profile where it
fits and in no profile where it does not. The flash layout of the calibration partition is checked
too. Skipped without a host C compiler."""

import re
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
MULTIAPP = REPO / "apps/multiapp"

SHIM = {
    "zephyr/kernel.h": r"""
#pragma once
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

struct device;
typedef struct { int ms; } k_timeout_t;
#define K_FOREVER ((k_timeout_t){-1})
#define K_NO_WAIT ((k_timeout_t){0})
#define K_MSEC(x) ((k_timeout_t){(int)(x)})
#define K_SECONDS(x) K_MSEC((x) * 1000)

typedef long atomic_t;
typedef void *atomic_ptr_t;
static inline void atomic_set(atomic_t *t, long v) { __atomic_store_n(t, v, __ATOMIC_SEQ_CST); }
static inline long atomic_get(const atomic_t *t) { return __atomic_load_n(t, __ATOMIC_SEQ_CST); }
static inline long atomic_inc(atomic_t *t) { return __atomic_fetch_add(t, 1, __ATOMIC_SEQ_CST); }
static inline long atomic_dec(atomic_t *t) { return __atomic_fetch_sub(t, 1, __ATOMIC_SEQ_CST); }
static inline void *atomic_ptr_set(atomic_ptr_t *t, void *v) { return __atomic_exchange_n(t, v, __ATOMIC_SEQ_CST); }
static inline void *atomic_ptr_get(const atomic_ptr_t *t) { return __atomic_load_n(t, __ATOMIC_SEQ_CST); }

/*
 * The test's clock, in ms: it moves when a thread sleeps (a poll of the claim, or the test). A
 * script may act as the PC each time it moves.
 */
extern int64_t fake_now;
extern void (*sleep_hook)(void);
static inline int64_t k_uptime_get(void) { return __atomic_load_n(&fake_now, __ATOMIC_SEQ_CST); }
static inline int32_t k_msleep(int32_t ms)
{
	__atomic_fetch_add(&fake_now, ms, __ATOMIC_SEQ_CST);
	if (sleep_hook != NULL) {
		sleep_hook();
	}
	usleep(100);
	return 0;
}

typedef struct { int64_t end; bool forever; } k_timepoint_t;
static inline k_timepoint_t sys_timepoint_calc(k_timeout_t t)
{
	k_timepoint_t p = {k_uptime_get() + t.ms, t.ms < 0};

	return p;
}
static inline bool sys_timepoint_expired(k_timepoint_t p) { return !p.forever && k_uptime_get() >= p.end; }

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
static inline int k_mutex_init(struct k_mutex *m)
{
	pthread_mutexattr_t a;

	pthread_mutexattr_init(&a);
	pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
	return pthread_mutex_init(&m->m, &a);
}
static inline int k_mutex_lock(struct k_mutex *m, k_timeout_t t)
{
	struct timespec end;

	if (t.ms < 0) {
		return pthread_mutex_lock(&m->m);
	}
	if (t.ms == 0) {
		return pthread_mutex_trylock(&m->m) ? -EBUSY : 0;
	}
	clock_gettime(CLOCK_REALTIME, &end);
	end.tv_sec += t.ms / 1000;
	end.tv_nsec += (t.ms % 1000) * 1000000L;
	if (end.tv_nsec >= 1000000000L) {
		end.tv_sec++;
		end.tv_nsec -= 1000000000L;
	}
	return pthread_mutex_timedlock(&m->m, &end) ? -EAGAIN : 0;
}
static inline int k_mutex_unlock(struct k_mutex *m) { return pthread_mutex_unlock(&m->m); }
""",
    "zephyr/shell/shell.h": "#pragma once\nstruct shell_static_entry;\n",
}

HARNESS = r"""
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int64_t fake_now;
void (*sleep_hook)(void);
#include "drive_proxy.c"
#include "profile.h"

static int remounts, syncs, remount_ret, usb_thread, ring;
static int do_remount(void) { remounts++; return remount_ret; }
static void do_sync(void) { syncs++; }
static bool in_usb(void) { return usb_thread != 0; }
static bool ring_runs(void) { return ring != 0; }
static struct drive_proxy px;

#define CHECK(cond)                                                                                \
	do {                                                                                       \
		if (!(cond)) {                                                                     \
			printf("FAILED line %d: %s\n", __LINE__, #cond);                           \
			return 1;                                                                  \
		}                                                                                  \
	} while (0)

static void reset(bool on_bus)
{
	static const struct drive_proxy_hooks hooks = {do_remount, do_sync, in_usb};

	memset(&px, 0, sizeof(px));
	fake_now = 1000;
	sleep_hook = NULL;
	remounts = syncs = remount_ret = usb_thread = ring = 0;
	drive_proxy_init(&px, &hooks);
	drive_proxy_on_bus(&px, on_bus);
}

/* The PC's disk access, as the proxy disk makes it: a begin, the work, an end. */
static bool pc_access(void)
{
	if (!drive_proxy_io_begin(&px)) {
		return false;
	}
	drive_proxy_io_end(&px);
	return true;
}

struct take_args { k_timeout_t timeout; bool write; int ret; };

static void *taker(void *p)
{
	struct take_args *a = p;

	a->ret = drive_take(&px, a->timeout, a->write);
	if (a->ret == 0) {
		drive_give(&px);
	}
	return NULL;
}

/* Another thread asks for the volume: what it gets (the device's other users are threads). */
static int take_in_thread(int ms, bool write)
{
	struct take_args a = {K_MSEC(ms), write, 99};
	pthread_t t;

	pthread_create(&t, NULL, taker, &a);
	pthread_join(t, NULL);
	return a.ret;
}

static void *op_thread(void *arg)
{
	*(volatile int *)arg = drive_proxy_io_begin(&px) ? 1 : 2;
	if (*(volatile int *)arg == 1) {
		drive_proxy_io_end(&px);
	}
	return NULL;
}

static int scenario_idle(void)
{
	reset(false);
	CHECK(drive_proxy_visible(&px));
	CHECK(drive_take(&px, K_FOREVER, true) == 0);
	/* No drive on the bus: only the lock. */
	CHECK(drive_proxy_visible(&px) && remounts == 0);
	CHECK(take_in_thread(20, false) == -EAGAIN);
	drive_give(&px);
	CHECK(syncs == 0 && drive_proxy_visible(&px));
	return 0;
}

static int scenario_write_hides(void)
{
	int64_t went;

	reset(true);
	CHECK(drive_proxy_visible(&px) && pc_access());
	went = fake_now;
	CHECK(drive_take(&px, K_FOREVER, true) == 0);
	CHECK(!drive_proxy_visible(&px) && !drive_proxy_io_begin(&px));
	CHECK(remounts == 1 && syncs == 0);
	drive_give(&px);
	CHECK(syncs == 1 && !drive_proxy_visible(&px) && !drive_proxy_io_begin(&px));
	/* Away for at least 3 s from when it went (the claim had waited out the quiet period). */
	fake_now = went + DRIVE_HIDE_MIN_MS - 1;
	CHECK(!drive_proxy_visible(&px) && !pc_access());
	fake_now = went + DRIVE_HIDE_MIN_MS;
	CHECK(drive_proxy_visible(&px) && pc_access());
	return 0;
}

/* A read does not hide the medium, makes the PC's accesses wait, and leaves no hold. */
static int scenario_read_keeps_the_medium(void)
{
	pthread_t t;
	volatile int got = 0;

	reset(true);
	CHECK(pc_access());
	CHECK(drive_take(&px, K_FOREVER, false) == 0);
	CHECK(drive_proxy_visible(&px) && remounts == 1);
	/* The PC's access waits for the end of the read, then goes on: no error. */
	pthread_create(&t, NULL, op_thread, (void *)&got);
	usleep(30000);
	CHECK(got == 0);
	CHECK(drive_proxy_visible(&px));
	drive_give(&px);
	pthread_join(t, NULL);
	CHECK(got == 1);
	CHECK(drive_proxy_visible(&px) && pc_access() && syncs == 0);
	return 0;
}

/* The read goes on while the command in flight finishes; the medium shows all the while. */
static int scenario_read_drains_visibly(void)
{
	pthread_t t;

	reset(true);
	CHECK(drive_proxy_io_begin(&px));
	pthread_create(&t, NULL, taker, &(struct take_args){K_FOREVER, false, 99});
	while (!px.draining) {
		usleep(100);
	}
	usleep(10000);
	CHECK(drive_proxy_visible(&px));
	CHECK(drive_proxy_io_begin(&px));
	drive_proxy_io_end(&px);
	drive_proxy_io_end(&px);
	pthread_join(t, NULL);
	CHECK(drive_proxy_visible(&px) && pc_access());
	return 0;
}

static int scenario_nest(void)
{
	reset(true);
	CHECK(drive_take(&px, K_FOREVER, true) == 0);
	CHECK(drive_take(&px, K_FOREVER, false) == 0);
	CHECK(drive_take(&px, K_FOREVER, true) == 0);
	drive_give(&px);
	drive_give(&px);
	/* Still the outer claim: one remount, nothing released. */
	CHECK(remounts == 1 && syncs == 0 && !pc_access());
	CHECK(take_in_thread(20, false) == -EAGAIN);
	drive_give(&px);
	CHECK(syncs == 1 && !drive_proxy_visible(&px));
	fake_now += DRIVE_HIDE_MIN_MS;
	CHECK(drive_proxy_visible(&px));

	/* A read does not become a write: it did not hide the medium. */
	CHECK(drive_take(&px, K_FOREVER, false) == 0);
	CHECK(drive_take(&px, K_FOREVER, true) == -EDEADLK);
	CHECK(drive_take(&px, K_FOREVER, false) == 0);
	drive_give(&px);
	drive_give(&px);
	CHECK(drive_proxy_visible(&px) && pc_access() && syncs == 1);
	/* And the lock is free. */
	CHECK(take_in_thread(100, true) == 0);
	return 0;
}

static volatile int claim_done, claim_ret = 99;

static void *claimer(void *arg)
{
	(void)arg;
	claim_ret = drive_take(&px, K_FOREVER, true);
	claim_done = 1;
	return NULL;
}

/* A PC command is in flight: the claim hides the medium and waits for it, and for the quiet. */
static int scenario_write_drains(void)
{
	int64_t last_io;
	pthread_t t;

	reset(true);
	CHECK(drive_proxy_io_begin(&px));
	pthread_create(&t, NULL, claimer, NULL);
	while (!px.draining) {
		usleep(100);
	}
	usleep(20000);
	CHECK(!claim_done && !drive_proxy_visible(&px));
	/* The command goes on (it may finish), but no new one starts: that is the status's job. */
	CHECK(drive_proxy_io_begin(&px));
	drive_proxy_io_end(&px);
	drive_proxy_io_end(&px);
	last_io = fake_now;
	pthread_join(t, NULL);
	CHECK(claim_done && claim_ret == 0);
	CHECK(fake_now - last_io >= DRIVE_QUIET_MS && DRIVE_QUIET_MS >= 100);
	CHECK(!pc_access() && remounts == 1);
	drive_give(&px);
	return 0;
}

/* The PC's command does not end: the claim gives up, the state is as it was. */
static int scenario_timeout(void)
{
	for (int write = 0; write < 2; write++) {
		reset(true);
		CHECK(drive_proxy_io_begin(&px));
		CHECK(drive_take(&px, K_MSEC(100), write != 0) == -EBUSY);
		CHECK(drive_proxy_visible(&px) && remounts == 0 && syncs == 0);
		/* The lock is free (a thread asks and meets the PC, not the lock), the PC goes on. */
		CHECK(take_in_thread(100, write != 0) == -EBUSY);
		CHECK(drive_proxy_io_begin(&px));
		drive_proxy_io_end(&px);
		drive_proxy_io_end(&px);
		CHECK(drive_take(&px, K_MSEC(100), write != 0) == 0);
		drive_give(&px);
	}
	return 0;
}

/* One period of hiding is counted from when the medium first went, however many claims it holds. */
static int scenario_continuous(void)
{
	reset(true);
	CHECK(drive_take(&px, K_FOREVER, true) == 0);
	drive_give(&px);
	fake_now = 1000 + 1500;
	CHECK(!drive_proxy_visible(&px));
	CHECK(drive_take(&px, K_FOREVER, true) == 0);
	drive_give(&px);
	fake_now = 1000 + DRIVE_HIDE_MIN_MS - 1;
	CHECK(!drive_proxy_visible(&px));
	fake_now = 1000 + DRIVE_HIDE_MIN_MS;
	CHECK(drive_proxy_visible(&px));
	/* A claim after that starts a period of its own. */
	fake_now = 5000;
	CHECK(drive_take(&px, K_FOREVER, true) == 0);
	drive_give(&px);
	fake_now = 5000 + DRIVE_HIDE_MIN_MS - 1;
	CHECK(!drive_proxy_visible(&px));
	fake_now = 5000 + DRIVE_HIDE_MIN_MS;
	CHECK(drive_proxy_visible(&px));
	/* A long claim: back at its end, the 3 s are over. */
	CHECK(drive_take(&px, K_FOREVER, true) == 0);
	fake_now += 10000;
	drive_give(&px);
	CHECK(drive_proxy_visible(&px));
	/* A read inside a hold does not shorten or extend it. */
	CHECK(drive_take(&px, K_FOREVER, true) == 0);
	drive_give(&px);
	fake_now += 1000;
	CHECK(drive_take(&px, K_FOREVER, false) == 0);
	CHECK(!drive_proxy_visible(&px));
	drive_give(&px);
	CHECK(!drive_proxy_visible(&px));
	fake_now += DRIVE_HIDE_MIN_MS;
	CHECK(drive_proxy_visible(&px));
	return 0;
}

static int scenario_remount_fails(void)
{
	reset(true);
	remount_ret = -EIO;
	CHECK(drive_take(&px, K_FOREVER, true) == -EIO);
	CHECK(syncs == 0 && !px.claimed && !px.draining);
	/* The volume may be in any state: the PC reads it again after the hold. */
	CHECK(!drive_proxy_visible(&px));
	fake_now += DRIVE_HIDE_MIN_MS;
	CHECK(drive_proxy_visible(&px));
	/* A reader's failure leaves the medium as it was, and the lock is free. */
	CHECK(take_in_thread(100, false) == -EIO);
	CHECK(drive_proxy_visible(&px) && pc_access());
	remount_ret = 0;
	CHECK(take_in_thread(100, false) == 0);
	return 0;
}

/* The USB thread's callbacks carry the PC's data: a claim there would wait for itself. */
static int scenario_usb_thread(void)
{
	reset(true);
	usb_thread = 1;
	CHECK(drive_take(&px, K_FOREVER, true) == -EDEADLK);
	CHECK(drive_take(&px, K_FOREVER, false) == -EDEADLK);
	CHECK(drive_proxy_visible(&px) && pc_access() && remounts == 0);
	/* The lock is free and nothing is held: no hold either. */
	usb_thread = 0;
	CHECK(take_in_thread(100, true) == 0);
	return 0;
}

/* While a ring runs, a write's hold is not over: the PC's rescan must not land in a stream. */
static int scenario_ring_defers_the_medium(void)
{
	int64_t went;

	reset(true);
	drive_proxy_set_busy(&px, ring_runs);
	ring = 1;
	/* A read has no hold: the ring does not matter. */
	CHECK(drive_take(&px, K_FOREVER, false) == 0);
	drive_give(&px);
	CHECK(drive_proxy_visible(&px) && pc_access());
	went = fake_now;
	CHECK(drive_take(&px, K_FOREVER, true) == 0);
	drive_give(&px);
	fake_now = went + DRIVE_HIDE_MIN_MS + 60000;
	CHECK(!drive_proxy_visible(&px) && !pc_access());
	ring = 0;
	CHECK(drive_proxy_visible(&px) && pc_access());
	/* Without a ring, the 3 s are enough. */
	went = fake_now;
	CHECK(drive_take(&px, K_FOREVER, true) == 0);
	drive_give(&px);
	fake_now = went + DRIVE_HIDE_MIN_MS;
	CHECK(drive_proxy_visible(&px));
	/* The module goes: its function is not called any more. */
	drive_proxy_set_busy(&px, NULL);
	ring = 1;
	went = fake_now;
	CHECK(drive_take(&px, K_FOREVER, true) == 0);
	drive_give(&px);
	fake_now = went + DRIVE_HIDE_MIN_MS;
	CHECK(drive_proxy_visible(&px) && atomic_get(&px.busy_calls) == 0);
	return 0;
}

/* The PC's script: disk accesses at given times (ms after t0), each a begin and an end at once. */
static int64_t t0, script[8];
static int script_n, script_next, script_ok, script_cut;
static volatile int script_gated;

static void script_tick(void)
{
	while (script_next < script_n && k_uptime_get() - t0 >= script[script_next]) {
		script_next++;
		if (drive_proxy_io_begin(&px)) {
			drive_proxy_io_end(&px);
			script_ok++;
		} else {
			script_cut++;
		}
	}
}

/* Accesses 60 ms apart: no gap of the quiet time, so the claim waits for the last one. */
static int scenario_a_command_that_keeps_going_is_waited_for(void)
{
	int64_t last, landed;

	for (int write = 0; write < 2; write++) {
		reset(true);
		t0 = fake_now;
		script[0] = 0;
		script[1] = 60;
		script[2] = 120;
		script[3] = 180;
		script_n = 4;
		script_next = script_ok = script_cut = 0;
		CHECK(pc_access());
		script_next = 1;
		script_n = 4;
		sleep_hook = script_tick;
		last = t0 + 180;
		CHECK(drive_take(&px, K_MSEC(2000), write != 0) == 0);
		landed = fake_now;
		sleep_hook = NULL;
		CHECK(script_ok == 3 && script_cut == 0);
		CHECK(landed - last >= DRIVE_QUIET_MS);
		drive_give(&px);
	}
	return 0;
}

static void *stall_pc(void *arg)
{
	/* The PC's second access after a stall: whether it goes ahead, and when. */
	(void)arg;
	script_ok = drive_proxy_io_begin(&px);
	if (script_ok) {
		drive_proxy_io_end(&px);
	}
	script_gated = 1;
	return NULL;
}

static volatile int device_has_volume, device_release;

static void *stall_claimer(void *arg)
{
	int write = (int)(intptr_t)arg;

	claim_ret = drive_take(&px, K_FOREVER, write != 0);
	device_has_volume = 1;
	while (!device_release) {
		usleep(100);
	}
	if (claim_ret == 0) {
		drive_give(&px);
	}
	return NULL;
}

/*
 * The PC stalls mid-command (it waits for data) for 150 ms, longer than the quiet time: the claim
 * lands in the stall. The PC's next access of the command is cut by a write and only delayed by a
 * read.
 */
static int stall(int write)
{
	pthread_t pc, dev;

	reset(true);
	CHECK(pc_access());
	t0 = fake_now;
	device_has_volume = device_release = script_gated = 0;
	pthread_create(&dev, NULL, stall_claimer, (void *)(intptr_t)write);
	while (!device_has_volume) {
		usleep(100);
	}
	CHECK(claim_ret == 0);
	CHECK(fake_now - t0 >= DRIVE_QUIET_MS && fake_now - t0 < 150);
	/* The stall is over: the command goes on. */
	fake_now = t0 + 150;
	pthread_create(&pc, NULL, stall_pc, NULL);
	usleep(30000);
	if (write) {
		CHECK(script_gated && script_ok == 0);
	} else {
		/* Waiting for the read to end. */
		CHECK(!script_gated);
	}
	device_release = 1;
	pthread_join(dev, NULL);
	pthread_join(pc, NULL);
	CHECK(script_gated);
	CHECK(write ? script_ok == 0 : script_ok == 1);
	return 0;
}

static int scenario_a_stall_cuts_a_write_and_only_delays_a_read(void)
{
	return stall(1) || stall(0);
}

/* A PC that copies without a pause: the read does not get the volume, and nobody is hurt. */
static int scenario_a_copy_that_does_not_stop_starves_the_read(void)
{
	for (int write = 0; write < 2; write++) {
		reset(true);
		t0 = fake_now;
		for (int i = 0; i < 8; i++) {
			script[i] = 60 * i;
		}
		script_n = 8;
		script_next = script_ok = script_cut = 0;
		script_tick();
		/* The script ends at 420 ms: a timeout of 300 ms is over before the PC pauses. */
		sleep_hook = script_tick;
		CHECK(drive_take(&px, K_MSEC(300), write != 0) == -EBUSY);
		sleep_hook = NULL;
		/* Every access of the command went through, and the medium is as it was. */
		CHECK(script_cut == 0 && script_ok == script_next && script_next >= 5);
		CHECK(drive_proxy_visible(&px) && pc_access() && remounts == 0);
	}
	return 0;
}

static volatile int stop, inside, device_has, violations, refused;
static long ops;

/*
 * The MSC thread: a command starts when the status says the medium is there, and runs a few disk
 * accesses (each may be refused once the device writes, or wait while it reads).
 */
static void *pc_thread(void *arg)
{
	(void)arg;
	while (!stop) {
		if (!drive_proxy_visible(&px)) {
			usleep(50);
			continue;
		}
		for (int i = 0; i < 3; i++) {
			if (!drive_proxy_io_begin(&px)) {
				refused++;
				break;
			}
			inside = 1;
			if (device_has) {
				violations++;
			}
			sched_yield();
			if (device_has) {
				violations++;
			}
			inside = 0;
			drive_proxy_io_end(&px);
			ops++;
		}
		/* The next command comes after a pause: a PC that never paused would starve a read. */
		k_msleep(DRIVE_QUIET_MS + 20);
		usleep(1000);
	}
	return NULL;
}

static int race(bool reads_only)
{
	pthread_t t;

	reset(true);
	stop = violations = refused = 0;
	ops = 0;
	pthread_create(&t, NULL, pc_thread, NULL);
	for (int i = 0; i < 300; i++) {
		CHECK(drive_take(&px, K_FOREVER, !reads_only && (i & 1) != 0) == 0);
		/* No PC access is running while the device has the volume. */
		CHECK(px.io == 0);
		device_has = 1;
		sched_yield();
		CHECK(px.io == 0);
		device_has = 0;
		drive_give(&px);
		/* Let the medium come back (a hold is over after 3 s), and the PC run a while. */
		k_msleep(DRIVE_HIDE_MIN_MS + 1);
		usleep(200);
	}
	stop = 1;
	pthread_join(t, NULL);
	CHECK(violations == 0 && ops > 0);
	/* A reader never makes the PC fail. */
	CHECK(!reads_only || refused == 0);
	return 0;
}

static int scenario_no_pc_access_overlaps_the_devices(void)
{
	return race(false);
}

static int scenario_a_read_never_fails_the_pc(void)
{
	return race(true);
}

static int profile(const char *declared, const char *built)
{
	printf("%u\n", multiapp_usb_profile((uint32_t)strtoul(declared, NULL, 0), strcmp(built, "0") != 0));
	return 0;
}

int main(int argc, char **argv)
{
	const char *which = argc > 1 ? argv[1] : "";

	if (strcmp(which, "profile") == 0 && argc == 4) {
		return profile(argv[2], argv[3]);
	}
#define SCENARIO(n)                                                                                \
	if (strcmp(which, #n) == 0) {                                                              \
		int ret = scenario_##n();                                                          \
		if (ret == 0) {                                                                    \
			printf("ok\n");                                                            \
		}                                                                                  \
		return ret;                                                                        \
	}
	SCENARIO(idle) SCENARIO(write_hides) SCENARIO(read_keeps_the_medium)
	SCENARIO(read_drains_visibly) SCENARIO(nest) SCENARIO(write_drains) SCENARIO(timeout)
	SCENARIO(continuous) SCENARIO(remount_fails) SCENARIO(usb_thread)
	SCENARIO(ring_defers_the_medium) SCENARIO(a_command_that_keeps_going_is_waited_for)
	SCENARIO(a_stall_cuts_a_write_and_only_delays_a_read)
	SCENARIO(a_copy_that_does_not_stop_starves_the_read)
	SCENARIO(no_pc_access_overlaps_the_devices) SCENARIO(a_read_never_fails_the_pc)
	printf("no scenario %s\n", which);
	return 2;
}
"""


@pytest.fixture(scope="module")
def harness(tmp_path_factory):
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        pytest.skip("no host C compiler")
    work = tmp_path_factory.mktemp("drive")
    for rel, text in SHIM.items():
        path = work / "shim" / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    src = work / "harness.c"
    src.write_text(HARNESS)
    exe = work / "harness"
    subprocess.run([cc, "-O1", "-Wall", "-pthread", "-D_GNU_SOURCE", "-I", str(work / "shim"),
                    "-I", str(MULTIAPP / "include"), "-I", str(MULTIAPP / "src"),
                    "-o", str(exe), str(src)], check=True)
    return exe


def run(harness, *args):
    out = subprocess.run([harness, *args], capture_output=True, text=True, timeout=60)
    assert out.returncode == 0, out.stdout + out.stderr
    return out.stdout.strip()


@pytest.mark.parametrize("scenario", [
    "idle", "write_hides", "read_keeps_the_medium", "read_drains_visibly", "nest", "write_drains",
    "timeout", "continuous", "remount_fails", "usb_thread", "ring_defers_the_medium",
    "a_command_that_keeps_going_is_waited_for", "a_stall_cuts_a_write_and_only_delays_a_read",
    "a_copy_that_does_not_stop_starves_the_read"])
def test_claim(harness, scenario):
    assert run(harness, scenario) == "ok"


def test_no_pc_access_overlaps_the_devices(harness):
    assert run(harness, "no_pc_access_overlaps_the_devices") == "ok"


def test_a_read_never_fails_the_pc(harness):
    assert run(harness, "a_read_never_fails_the_pc") == "ok"


# ---- the USB side: which classes a boot has and whether the endpoints and FIFOs hold them ----

NCM, VENDOR, MSC, OWN_DISKS = 1, 2, 4, 8

# IN endpoints a class asks for, as (type, packet size at full speed, at high speed): the
# descriptors of the classes (zephyr/subsys/usb/device_next/class/*.c, apps/osmosdr/src/usb_sdr.c).
# Out endpoints are bulk in every class and all the controllers' OUT endpoints can be bulk.
CLASSES = {
    "cdc_acm_0": {"in": [("int", 16, 16), ("bulk", 64, 512)], "out": 1},
    "cdc_ncm_0": {"in": [("int", 16, 16), ("bulk", 64, 512)], "out": 1},
    "dfu_dfu": {"in": [], "out": 0},
    "esp_sdr_usb": {"in": [("bulk", 64, 512)], "out": 1},
    "msc_0": {"in": [("bulk", 64, 512)], "out": 1},
}
# The order they register in: the iterable section of the classes is sorted by name.
ORDER = sorted(CLASSES)

# The S31's OTG HS: 896 words of FIFO RAM (GHWCFG3), each IN FIFO's power-on depth 512
# (doc/samples/transports.md, "TX FIFO sizing").
S31_DFIFO_WORDS = 896
S31_POWER_ON_WORDS = 512


def workspace_file(rel):
    for base in REPO.parents:
        if (base / rel).is_file():
            return base / rel
    pytest.skip(f"no {rel} in the workspace")


def dt_values(text, name):
    m = re.search(rf"\b{re.escape(name)}\s*=\s*<([^>]*)>\s*;", text)
    assert m, name
    return [int(v, 0) for v in m.group(1).split()]


def usb_otg(dtsi, board_overlay=None):
    """Properties of a controller node: its dtsi's, with the board overlay's g-tx-fifo-size."""
    text = workspace_file(dtsi).read_text()
    node = text[text.index("usb_otg: usb_otg@"):]
    node = node[:node.index("\n\t\t};")]
    props = {name: dt_values(node, name) for name in
             ("num-out-eps", "num-in-eps", "ghwcfg4", "g-rx-fifo-size", "g-np-tx-fifo-size",
              "g-tx-fifo-size")}
    if board_overlay is not None:
        props["g-tx-fifo-size"] = dt_values(board_overlay.read_text(), "g-tx-fifo-size")
    return props


class Controller:
    def __init__(self, props, hs):
        self.hs = hs
        ghwcfg4 = props["ghwcfg4"][0]
        # udc_dwc2_preinit(): EP0 plus the ones the hardware has, capped by the devicetree.
        ineps = ((ghwcfg4 >> 26) & 0xF) + 1
        self.n_in = min(ineps, props["num-in-eps"][0]) - 1
        self.n_out = props["num-out-eps"][0] - 1
        self.fifo = props["g-tx-fifo-size"]
        assert len(self.fifo) == ineps - 1, "BUILD_ASSERT of the driver"
        self.rx = props["g-rx-fifo-size"][0]
        self.np = props["g-np-tx-fifo-size"][0]

    def in_ok(self, idx, kind, mps_fs, mps_hs):
        """udc_ep_try_config() of IN endpoint idx for this request."""
        if idx > self.n_in:
            return False
        bytes_ = self.fifo[idx - 1] * 4
        mps = mps_hs if self.hs else mps_fs
        bulk_bytes = 512 if self.hs else 64
        if mps > min(bytes_, 1024 if self.hs else 1023):
            return False
        return kind != "bulk" or bytes_ >= bulk_bytes

    def assign(self, profile):
        """First fit in registration order: {(class, n): (ep, words)} or None if a class has no endpoint."""
        classes = ["cdc_acm_0", "dfu_dfu"]
        if profile & NCM:
            classes.append("cdc_ncm_0")
        if profile & VENDOR:
            classes.append("esp_sdr_usb")
        if profile & MSC:
            classes.append("msc_0")
        classes.sort()
        used_in, used_out, out = set(), 0, {}
        for name in classes:
            for n, (kind, fs, hs) in enumerate(CLASSES[name]["in"]):
                for idx in range(1, self.n_in + 1):
                    if idx not in used_in and self.in_ok(idx, kind, fs, hs):
                        used_in.add(idx)
                        out[(name, n)] = (idx, self.fifo[idx - 1])
                        break
                else:
                    return None
            used_out += CLASSES[name]["out"]
        return out if used_out <= self.n_out else None


def s31():
    overlay = MULTIAPP / "boards/esp32s31_function_coreboard_esp32s31_hpcore.overlay"
    return Controller(usb_otg("modules/zephyr/dts/riscv/espressif/esp32s31/esp32s31_common.dtsi", overlay),
                      hs=True)


def s3():
    return Controller(usb_otg("modules/zephyr/dts/xtensa/espressif/esp32s3/esp32s3_common.dtsi"), hs=False)


def declared_masks(block):
    """name -> USB mask of the multiapp_extension() calls in a CMake block."""
    out = {}
    for m in re.finditer(r"multiapp_extension\((\w+)([^)]*)\)", block):
        usb = re.search(r"\bUSB (\d+)", m.group(2))
        out[m.group(1)] = int(usb.group(1)) if usb else 0
    return out


CMAKE = (MULTIAPP / "CMakeLists.txt").read_text()
S3_APPS = declared_masks(CMAKE[CMAKE.index("if(CONFIG_SOC_SERIES_ESP32S3)"):
                               CMAKE.index("elseif(CONFIG_SOC_SERIES_ESP32S31)")])
S3_APPS.update(declared_masks((MULTIAPP / "micropython.cmake").read_text()))
S31_APPS = declared_masks(CMAKE[CMAKE.index("elseif(CONFIG_SOC_SERIES_ESP32S31)"):
                                CMAKE.index("# Hot-swap kernels")])


def effective(harness, declared, built=True):
    return int(run(harness, "profile", str(declared), "1" if built else "0"))


def test_the_s31_fifo_table_is_the_one_the_controller_has():
    c = s31()
    assert c.fifo == [16, 128, 288, 128, 16, 16, 16]
    # RX, EP0 and the IN FIFOs: all of the RAM, and none of them deeper than at power-on (the
    # driver's checks).
    assert c.rx + c.np + sum(c.fifo) == S31_DFIFO_WORDS
    assert max(c.fifo) <= S31_POWER_ON_WORDS


def test_osmosdr_hs_keeps_its_stream_endpoint_and_the_drive_gets_the_next(harness):
    c = s31()
    profile = effective(harness, S31_APPS["osmosdr_hs"])
    assert profile == VENDOR | MSC
    got = c.assign(profile)
    assert got[("cdc_acm_0", 0)] == (1, 16)      # notification: interrupt only
    assert got[("cdc_acm_0", 1)] == (2, 128)
    assert got[("esp_sdr_usb", 0)] == (3, 288)   # the stream: at least 256 words
    assert got[("msc_0", 0)] == (4, 128)         # one 512-byte bulk packet
    assert got[("esp_sdr_usb", 0)][1] >= 256


def test_a_boot_without_the_stream_gives_the_drive_the_third_endpoint(harness):
    got = s31().assign(effective(harness, 0))
    assert got[("msc_0", 0)] == (3, 288)


def test_the_old_table_could_not_hold_the_stream_and_the_drive():
    c = s31()
    c.fifo = [16, 128, 400, 16, 16, 16, 16]
    assert c.assign(VENDOR) is not None
    assert c.assign(VENDOR | MSC) is None


@pytest.mark.parametrize("soc", ["s3", "s31"])
def test_every_apps_profile_fits_its_controller(harness, soc):
    c = s3() if soc == "s3" else s31()
    apps = S3_APPS if soc == "s3" else S31_APPS
    assert apps
    for name, declared in apps.items():
        profile = effective(harness, declared)
        assert c.assign(profile) is not None, (soc, name, profile)


def test_the_drive_is_in_every_profile_where_it_fits(harness):
    for soc, c, apps in (("s3", s3(), S3_APPS), ("s31", s31(), S31_APPS)):
        for name, declared in apps.items():
            if declared & OWN_DISKS:
                continue
            profile = effective(harness, declared)
            with_drive = c.assign((declared | MSC) & ~OWN_DISKS)
            # In the profile exactly when it fits.
            assert bool(profile & MSC) == (with_drive is not None), (soc, name)


def test_stream_has_no_endpoint_left_for_the_drive(harness):
    c = s3()
    assert S3_APPS["stream"] == NCM
    assert effective(harness, NCM) == NCM
    assert c.assign(NCM) is not None
    # The shell's two and NCM's two are the controller's four; a fifth would not exist.
    assert c.n_in == 4
    assert c.assign(NCM | MSC) is None


def test_s3_osmosdr_has_the_drive_as_its_fourth_endpoint(harness):
    got = s3().assign(effective(harness, S3_APPS["osmosdr"]))
    assert sorted(ep for ep, _ in got.values()) == [1, 2, 3, 4]
    assert got[("esp_sdr_usb", 0)][0] == 3 and got[("msc_0", 0)][0] == 4


def test_micropython_keeps_the_profile_it_declares(harness):
    declared = S3_APPS["micropython"]
    assert declared == MSC | OWN_DISKS
    assert effective(harness, declared) == declared


def test_the_profile_needs_the_class_built(harness):
    assert effective(harness, 0, built=False) == 0
    assert effective(harness, VENDOR, built=False) == VENDOR


# ---- the calibration partition: the flash layout and the wiring of the S31 host ----

def size_of(text):
    m = re.fullmatch(r"DT_SIZE_([KM])\((\d+)\)", text)
    if m:
        return int(m.group(2)) * (1024 if m.group(1) == "K" else 1024 * 1024)
    return int(text, 0)


def partitions(text):
    """label -> (start, size) of the partition nodes of a dtsi."""
    out = {}
    for m in re.finditer(r"partition@([0-9a-f]+)\s*\{[^}]*?label = \"([^\"]+)\";[^}]*?"
                         r"reg = <(0x[0-9a-f]+) ([^>]+?)>;", text):
        start = int(m.group(3), 0)
        assert start == int(m.group(1), 16), m.group(2)
        out[m.group(2)] = (start, size_of(m.group(4).strip()))
    return out


def s31_table():
    vendor = partitions(workspace_file(
        "modules/zephyr/dts/vendor/espressif/partitions_0x2000_default_8M.dtsi").read_text())
    ours = partitions((MULTIAPP / "boards/partitions_s31.dtsi").read_text())
    return vendor, ours


def test_the_calibration_partition_sits_in_free_flash_and_moves_nothing():
    vendor, ours = s31_table()
    table = {**vendor, **ours}
    assert ours["calibration"] == (0x7E0000, 16 * 1024)
    # Four erase sectors of 4 KiB: ZMS wants at least two.
    assert ours["calibration"][1] % 4096 == 0 and ours["calibration"][1] >= 2 * 4096
    # The FAT volume starts where it did, the application's slots and MCUboot are Espressif's.
    assert ours["fat"] == (0x800000, 8 * 1024 * 1024)
    assert table["mcuboot"] == (0x2000, 56 * 1024)
    assert table["image-0"] == (0x20000, 3840 * 1024) and table["image-1"] == (0x3E0000, 3840 * 1024)
    # Nothing overlaps anything, and the new partition is in the lower 8 MB.
    spans = sorted((start, start + size, label) for label, (start, size) in table.items())
    for (s0, e0, l0), (s1, e1, l1) in zip(spans, spans[1:]):
        assert e0 <= s1, (l0, l1)
    assert ours["calibration"][0] + ours["calibration"][1] <= 0x800000
    assert spans[-1][2] == "fat"


def test_the_s31_host_keeps_the_calibration_in_that_partition_with_zms():
    conf = (MULTIAPP / "socs/esp32s31_hpcore.conf").read_text()
    lines = {l.split("=")[0]: l.split("=")[1] for l in conf.splitlines()
             if re.match(r"CONFIG_\w+=", l)}
    assert lines["CONFIG_SETTINGS_ZMS"] == "y" and lines["CONFIG_ZMS"] == "y"
    assert lines["CONFIG_ESP_SDR_CALSTORE"] == "y"
    assert "CONFIG_SETTINGS_FILE" not in lines and "CONFIG_SETTINGS_FILE_PATH" not in lines
    overlay = (MULTIAPP / "socs/esp32s31_hpcore.overlay").read_text()
    assert re.search(r"zephyr,settings-partition = &cal_partition;", overlay)
    # No read-write mount of the volume for settings any more, and no claim in the store.
    for rel in ("src/drive.c", "src/apps.c", "src/apps.h"):
        text = (MULTIAPP / rel).read_text()
        assert "mounted_rw" not in text and "mount_rw" not in text and "SETTINGS_FILE" not in text, rel
    store = (REPO / "lib/esp_sdr/src/esp_sdr_calstore.c").read_text()
    assert "multiapp" not in store.replace("apps/multiapp", "") and "STORE_BEGIN" not in store


def test_the_standalone_apps_and_other_boards_keep_their_calibration_backends():
    conf = (REPO / "apps/common/calstore.conf").read_text()
    assert "CONFIG_SETTINGS_FILE=y" in conf and "CONFIG_ZMS" not in conf
    assert "calibration" not in (MULTIAPP / "boards/partitions.dtsi").read_text()
    assert "calibration" not in (MULTIAPP / "sysbuild/mcuboot.overlay").read_text()


def test_the_usb_thread_is_the_one_zephyr_names_usbd():
    drive = (MULTIAPP / "src/drive.c").read_text()
    guard = drive[drive.index("static bool in_usb_thread"):]
    guard = guard[:guard.index("\n}\n")]
    assert 'strcmp(name, "usbd") == 0' in guard
    core = workspace_file("modules/zephyr/subsys/usb/device_next/usbd_core.c").read_text()
    assert 'k_thread_name_set(&usbd_thread_data, "usbd")' in core
    # The claim refuses there, and nothing else claims.
    assert "-EDEADLK" in drive and "hooks.in_usb_thread" in (MULTIAPP / "src/drive_proxy.c").read_text()


# ---- the wiring, read from the sources ----

def test_the_fat_object_is_out_of_ring_bank_0():
    ld = (MULTIAPP / "multiapp_bank0.ld").read_text()
    section = ld[ld.index(".multiapp_bank0"):ld.index("}")]
    assert "multiapp_fat_fs" not in section and "drive.c" not in section
    assert "usbd_cdc_ncm" in section
    drive = (MULTIAPP / "src/drive.c").read_text()
    assert "ENOTSUP" not in drive
    assert "multiapp_fat_fs" in drive


def test_the_host_registers_the_drive_for_the_profile_it_keeps():
    apps = (MULTIAPP / "src/apps.c").read_text()
    classes = apps[apps.index("void app_usb_classes"):apps.index("void multiapp_shell_register")]
    assert "multiapp_drive_msc_set(true)" in classes
    assert "MULTIAPP_USB_OWN_DISKS" in classes
    # Every comparison of profiles goes through profile_of().
    assert "app->usb" not in apps.replace("multiapp_usb_profile(app->usb", "")
