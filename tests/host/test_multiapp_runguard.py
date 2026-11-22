# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/multiapp/src/runguard.c: the delayed "it is running" mark of the crash-loop guard, and how
src/apps.c uses it.

An app whose main() never returns (osmosdr_hs) would stay at STEP_MAIN in the boot record, and
after a warm reboot boot_pick() would take its autostart for an app that died before running and
skip it once. The guard marks it as running after CONFIG_MULTIAPP_RUNNING_AFTER_MS, unless it was
cancelled (unload) or armed again (another app started) first. runguard.c is compiled as it is
against a work queue of threads; the wiring in apps.c is checked in its source. Skipped without a
host C compiler."""

import re
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
MULTIAPP = REPO / "apps/multiapp"

SHIM = {
    "zephyr/sys/util.h": """
#pragma once
#include <stddef.h>
#define CONTAINER_OF(ptr, type, field) ((type *)((char *)(ptr) - offsetof(type, field)))
""",
    "zephyr/kernel.h": r"""
#pragma once
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct { int ms; } k_timeout_t;
#define K_MSEC(x) ((k_timeout_t){(int)(x)})

struct k_spinlock { pthread_mutex_t m; };
typedef int k_spinlock_key_t;
static inline k_spinlock_key_t k_spin_lock(struct k_spinlock *l) { pthread_mutex_lock(&l->m); return 0; }
static inline void k_spin_unlock(struct k_spinlock *l, k_spinlock_key_t k) { (void)k; pthread_mutex_unlock(&l->m); }

/* A delayed work item: a thread per scheduling that sleeps, then runs the handler unless it was
 * rescheduled or cancelled (the generation moved), as the system work queue's does. */
struct k_work;
typedef void (*k_work_handler_t)(struct k_work *work);
struct k_work { k_work_handler_t handler; };
struct k_work_delayable {
	struct k_work work;
	pthread_mutex_t m;
	unsigned gen;
	int running;
};
struct k_work_sync { int unused; };

static inline struct k_work_delayable *k_work_delayable_from_work(struct k_work *w)
{
	return (struct k_work_delayable *)w;
}
static inline void k_work_init_delayable(struct k_work_delayable *d, k_work_handler_t h)
{
	d->work.handler = h;
	pthread_mutex_init(&d->m, NULL);
	d->gen = 0;
	d->running = 0;
}

struct sched { struct k_work_delayable *d; unsigned gen; int ms; };
static inline void *sched_thread(void *arg)
{
	struct sched s = *(struct sched *)arg;
	int fire;

	free(arg);
	usleep((useconds_t)s.ms * 1000);
	pthread_mutex_lock(&s.d->m);
	fire = s.d->gen == s.gen;
	if (fire) {
		s.d->running++;
	}
	pthread_mutex_unlock(&s.d->m);
	if (fire) {
		s.d->work.handler(&s.d->work);
		pthread_mutex_lock(&s.d->m);
		s.d->running--;
		pthread_mutex_unlock(&s.d->m);
	}
	return NULL;
}
static inline int k_work_reschedule(struct k_work_delayable *d, k_timeout_t t)
{
	struct sched *s = malloc(sizeof(*s));
	pthread_t th;

	pthread_mutex_lock(&d->m);
	s->d = d;
	s->gen = ++d->gen;
	s->ms = t.ms;
	pthread_mutex_unlock(&d->m);
	pthread_create(&th, NULL, sched_thread, s);
	pthread_detach(th);
	return 1;
}
static inline int k_work_cancel_delayable_sync(struct k_work_delayable *d, struct k_work_sync *sync)
{
	(void)sync;
	pthread_mutex_lock(&d->m);
	d->gen++;
	pthread_mutex_unlock(&d->m);
	for (;;) {
		int running;

		pthread_mutex_lock(&d->m);
		running = d->running;
		pthread_mutex_unlock(&d->m);
		if (!running) {
			return 0;
		}
		usleep(500);
	}
}
""",
}

HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include "runguard.c"

static struct runguard g;
static volatile int marks, mark_slow, mark_done;

static void mark(void)
{
	if (mark_slow) {
		usleep(mark_slow * 1000);
		mark_done = 1;
	}
	marks++;
}

#define CHECK(cond)                                                                                \
	do {                                                                                       \
		if (!(cond)) {                                                                     \
			printf("FAILED line %d: %s\n", __LINE__, #cond);                           \
			return 1;                                                                  \
		}                                                                                  \
	} while (0)

static void ms(int n) { usleep((useconds_t)n * 1000); }

/* An app that runs long enough is marked, once. */
static int scenario_fires(void)
{
	runguard_arm(&g, 40);
	ms(15);
	CHECK(marks == 0);
	ms(80);
	CHECK(marks == 1);
	ms(80);
	CHECK(marks == 1);
	return 0;
}

/* An app unloaded before its time is not marked. */
static int scenario_cancelled(void)
{
	runguard_arm(&g, 40);
	ms(10);
	runguard_cancel(&g);
	ms(100);
	CHECK(marks == 0);
	/* Cancelling what has fired, or nothing, is harmless. */
	runguard_cancel(&g);
	return 0;
}

/* Another app started since: its arming replaces the first, which is not marked on its own time. */
static int scenario_rearmed(void)
{
	runguard_arm(&g, 60);
	ms(30);
	runguard_arm(&g, 60);
	ms(45);
	/* The first deadline (60 ms) is past, the second (90 ms) is not. */
	CHECK(marks == 0);
	ms(80);
	CHECK(marks == 1);
	return 0;
}

/* Unload, then the next app: the cancelled one never fires, the new one does. */
static int scenario_next_app(void)
{
	runguard_arm(&g, 50);
	ms(10);
	runguard_cancel(&g);
	runguard_arm(&g, 50);
	ms(100);
	CHECK(marks == 1);
	return 0;
}

/* A mark that is running when the app unloads: the cancel waits for it, so it is over by then. */
static int scenario_cancel_waits(void)
{
	mark_slow = 100;
	runguard_arm(&g, 10);
	ms(40);
	runguard_cancel(&g);
	CHECK(mark_done == 1 && marks == 1);
	return 0;
}

int main(int argc, char **argv)
{
	const char *which = argc > 1 ? argv[1] : "";
	static const struct { const char *name; int (*fn)(void); } all[] = {
		{"fires", scenario_fires}, {"cancelled", scenario_cancelled},
		{"rearmed", scenario_rearmed}, {"next", scenario_next_app},
		{"cancel_waits", scenario_cancel_waits},
	};

	runguard_init(&g, mark);
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
    work = tmp_path_factory.mktemp("runguard")
    for rel, text in SHIM.items():
        path = work / "shim" / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    src = work / "harness.c"
    src.write_text(HARNESS)
    exe = work / "harness"
    subprocess.run([cc, "-O1", "-Wall", "-pthread", "-I", str(work / "shim"),
                    "-I", str(MULTIAPP / "src"), "-o", str(exe), str(src)], check=True)
    return exe


@pytest.mark.parametrize("scenario", ["fires", "cancelled", "rearmed", "next", "cancel_waits"])
def test_the_running_mark(harness, scenario):
    out = subprocess.run([harness, scenario], capture_output=True, text=True, timeout=30)
    assert out.returncode == 0, out.stdout + out.stderr
    assert out.stdout.strip() == "ok"


# ---- src/apps.c: where the guard is armed, cancelled and what the mark checks ----

APPS = (MULTIAPP / "src/apps.c").read_text()


def body(text, signature):
    m = re.search(signature, text, re.M)
    assert m, signature
    return text[m.start():text.index("\n}\n", m.start())]


def test_the_mark_is_only_made_for_the_app_that_started_and_still_runs():
    mark = body(APPS, r"^static void running_mark")
    assert "loaded != NULL" in mark and "loaded_info != NULL" in mark
    # Only from "in main": never back from refused or forward from anything else.
    assert "request.step == STEP_MAIN" in mark and "boot_step(STEP_RUNNING)" in mark


def test_the_guard_is_armed_when_main_starts_and_cancelled_before_an_unload():
    # A console app's main() starts when USB is up, the others' at once.
    start = body(APPS, r"^int multiapp_start")
    assert re.search(r"if \(!app->console\) \{\s*running_arm\(\);", start)
    assert start.index("k_thread_create(&app_thread") < start.index("running_arm()")
    usb = body(APPS, r"^void multiapp_usb_up")
    assert usb.index("k_thread_start(&app_thread)") < usb.index("running_arm()")
    unload = body(APPS, r"^static int app_unload")
    assert unload.index("runguard_cancel(&running_guard)") < unload.index("app_stop()")


def test_the_return_path_still_marks_and_the_delay_is_a_kconfig_option():
    main = body(APPS, r"^static void app_main")
    assert "boot_step(STEP_RUNNING)" in main
    kconfig = (MULTIAPP / "Kconfig").read_text()
    option = kconfig[kconfig.index("config MULTIAPP_RUNNING_AFTER_MS"):]
    assert re.search(r"default 3000", option[:option.index("\nconfig ", 5)])
    arm = body(APPS, r"^static void running_arm")
    assert "CONFIG_MULTIAPP_RUNNING_AFTER_MS > 0" in arm
    # A crash-loop guard: boot_pick() still skips an app that stayed below STEP_RUNNING.
    assert "tried_step < STEP_RUNNING" in APPS
