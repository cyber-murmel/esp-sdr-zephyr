# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/multiapp/src/osmosdr.c: the vendor class forwarders and multiapp_osmosdr_drop(), run on the
host with threads standing for the USB thread and the shell's.

The unload must be sure that no call into the app is running or can start once the drop returns
(the app's memory goes next). The file is compiled as it is, against stubs of the few kernel calls
it makes; multiapp_app_fn() stands for the host's lookup, which returns nothing once the unload
has begun. Skipped without a host C compiler."""

import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
MULTIAPP = REPO / "apps/multiapp"

# What osmosdr.c includes of Zephyr. atomic_get() runs test_after_get once, after it has read 0: the
# instant after the drop saw no call inside, in which another thread may run (the race to rule out).
SHIM = {
    "zephyr/kernel.h": """
#pragma once
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/sys/atomic.h>
void k_msleep(int ms);
""",
    "zephyr/device.h": "#pragma once\nstruct device;\n",
    "zephyr/sys/atomic.h": """
#pragma once
typedef long atomic_t;
extern void (*test_after_get)(void);
static inline long atomic_get(const atomic_t *t)
{
	long v = __atomic_load_n(t, __ATOMIC_SEQ_CST);

	if (v == 0 && test_after_get != NULL) {
		void (*hook)(void) = test_after_get;

		test_after_get = NULL;
		hook();
	}
	return v;
}
static inline long atomic_inc(atomic_t *t) { return __atomic_fetch_add(t, 1, __ATOMIC_SEQ_CST); }
static inline long atomic_dec(atomic_t *t) { return __atomic_fetch_sub(t, 1, __ATOMIC_SEQ_CST); }
""",
}

HARNESS = r"""
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void (*test_after_get)(void);
#include "osmosdr.c"

void k_msleep(int ms) { usleep((useconds_t)ms * 1000); }

enum { IN_DONE, OUT_DONE, LINK, CONTROL_OUT, CONTROL_IN, HANDLERS };
static const char *const names[HANDLERS] = {"usb_sdr_in_done", "usb_sdr_out_done", "usb_sdr_link",
					    "usb_sdr_control_out", "usb_sdr_control_in"};

/* Two apps, one after the other: what a handler of an app that is gone is called with. */
static int live, cur;
static int freed[2], gate[2], entered[2], release_gate[2], bad;
static int hits[2][HANDLERS];

static void enter(int g, int which)
{
	__atomic_fetch_add(&hits[g][which], 1, __ATOMIC_SEQ_CST);
	if (__atomic_load_n(&freed[g], __ATOMIC_SEQ_CST)) {
		__atomic_fetch_add(&bad, 1, __ATOMIC_SEQ_CST);
	}
}

#define APP(g)                                                                                     \
	static void in_done_##g(uint8_t *d, size_t l, int e) { (void)d; (void)l; (void)e; enter(g, IN_DONE); } \
	static void out_done_##g(uint8_t *d, size_t l, int e) { (void)d; (void)l; (void)e; enter(g, OUT_DONE); } \
	static void link_##g(bool up) { (void)up; enter(g, LINK); }                               \
	static int control_out_##g(uint8_t r, uint16_t v, const uint8_t *d, size_t l)              \
	{ (void)r; (void)v; (void)d; (void)l; enter(g, CONTROL_OUT); return 0; }                   \
	static int control_in_##g(uint8_t r, uint16_t v, uint8_t *b, size_t m)                     \
	{                                                                                          \
		(void)r; (void)v; (void)b; (void)m;                                                \
		enter(g, CONTROL_IN);                                                              \
		if (gate[g]) {                                                                     \
			__atomic_store_n(&entered[g], 1, __ATOMIC_SEQ_CST);                        \
			while (!__atomic_load_n(&release_gate[g], __ATOMIC_SEQ_CST)) {             \
				usleep(100);                                                       \
			}                                                                          \
		}                                                                                  \
		/* Still inside when the app's memory goes: the bug. */                            \
		if (__atomic_load_n(&freed[g], __ATOMIC_SEQ_CST)) {                                \
			__atomic_fetch_add(&bad, 1, __ATOMIC_SEQ_CST);                             \
		}                                                                                  \
		return 7;                                                                          \
	}
APP(0)
APP(1)

static void *const table[2][HANDLERS] = {
	{in_done_0, out_done_0, link_0, control_out_0, control_in_0},
	{in_done_1, out_done_1, link_1, control_out_1, control_in_1},
};

/* The host's lookup: nothing once the app is cut off (loaded_info cleared). */
void *multiapp_app_fn(const char *name)
{
	if (!__atomic_load_n(&live, __ATOMIC_SEQ_CST)) {
		return NULL;
	}
	for (int i = 0; i < HANDLERS; i++) {
		if (strcmp(name, names[i]) == 0) {
			return table[__atomic_load_n(&cur, __ATOMIC_SEQ_CST)][i];
		}
	}
	return NULL;
}

static uint8_t buf[8];

static void call_all(void)
{
	usb_sdr_in_done(buf, 1, 0);
	usb_sdr_out_done(buf, 1, 0);
	usb_sdr_link(true);
	(void)usb_sdr_control_out(1, 2, buf, 1);
	(void)usb_sdr_control_in(1, 2, buf, 1);
}

#define CHECK(cond)                                                                                \
	do {                                                                                       \
		if (!(cond)) {                                                                     \
			printf("FAILED line %d: %s\n", __LINE__, #cond);                           \
			return 1;                                                                  \
		}                                                                                  \
	} while (0)

static int ran_in_the_gap;

/* The USB thread runs right after the drop saw nothing inside: it must find no handler. */
static void usb_thread_in_the_gap(void)
{
	int before = hits[0][CONTROL_IN];

	(void)usb_sdr_control_in(1, 2, buf, 1);
	ran_in_the_gap = hits[0][CONTROL_IN] != before;
}

static int scenario_gap(void)
{
	live = 1;
	cur = 0;
	call_all();
	for (int i = 0; i < HANDLERS; i++) {
		CHECK(hits[0][i] == 1);
	}
	CHECK(calls == 0);
	/* The unload: cut off first (live = 0 stands for loaded_info), then drop. */
	__atomic_store_n(&live, 0, __ATOMIC_SEQ_CST);
	test_after_get = usb_thread_in_the_gap;
	CHECK(multiapp_osmosdr_drop() == 0);
	CHECK(!ran_in_the_gap);
	__atomic_store_n(&freed[0], 1, __ATOMIC_SEQ_CST);
	/* After it: nothing of the app is called, whatever the host asks. */
	call_all();
	CHECK(usb_sdr_control_in(1, 2, buf, 1) == -ENOTSUP);
	CHECK(usb_sdr_control_out(1, 2, buf, 1) == -ENOTSUP);
	CHECK(bad == 0 && calls == 0);
	/* The next app's handlers are found, not the first one's. */
	cur = 1;
	__atomic_store_n(&live, 1, __ATOMIC_SEQ_CST);
	call_all();
	for (int i = 0; i < HANDLERS; i++) {
		CHECK(hits[1][i] == 1);
		CHECK(hits[0][i] == 1);
	}
	CHECK(usb_sdr_control_in(1, 2, buf, 1) == 7);
	CHECK(bad == 0 && calls == 0);
	return 0;
}

static void *usb_thread(void *arg)
{
	(void)arg;
	(void)usb_sdr_control_in(1, 2, buf, 1);
	return NULL;
}

static int drop_ret = 99, drop_done;

static void *shell_thread(void *arg)
{
	(void)arg;
	drop_ret = multiapp_osmosdr_drop();
	__atomic_store_n(&drop_done, 1, __ATOMIC_SEQ_CST);
	return NULL;
}

/* A handler is inside: the drop waits for it (or gives up with -EBUSY, the memory then stays). */
static int scenario_wait(int give_up)
{
	pthread_t usb, shell;

	live = 1;
	cur = 0;
	gate[0] = 1;
	pthread_create(&usb, NULL, usb_thread, NULL);
	while (!__atomic_load_n(&entered[0], __ATOMIC_SEQ_CST)) {
		usleep(100);
	}
	CHECK(calls == 1);
	__atomic_store_n(&live, 0, __ATOMIC_SEQ_CST);
	pthread_create(&shell, NULL, shell_thread, NULL);
	if (give_up) {
		pthread_join(shell, NULL);
		CHECK(drop_ret == -EBUSY);
	} else {
		usleep(100 * 1000);
		CHECK(!__atomic_load_n(&drop_done, __ATOMIC_SEQ_CST));
		__atomic_store_n(&release_gate[0], 1, __ATOMIC_SEQ_CST);
		pthread_join(shell, NULL);
		CHECK(drop_ret == 0);
		/* The handler had returned by then: the memory may go. */
		CHECK(calls == 0);
	}
	__atomic_store_n(&release_gate[0], 1, __ATOMIC_SEQ_CST);
	pthread_join(usb, NULL);
	CHECK(bad == 0);
	CHECK(calls == 0);
	return 0;
}

int main(int argc, char **argv)
{
	const char *which = argc > 1 ? argv[1] : "";
	int ret;

	if (strcmp(which, "gap") == 0) {
		ret = scenario_gap();
	} else if (strcmp(which, "wait") == 0) {
		ret = scenario_wait(0);
	} else if (strcmp(which, "giveup") == 0) {
		ret = scenario_wait(1);
	} else {
		printf("no scenario %s\n", which);
		return 2;
	}
	if (ret == 0) {
		printf("ok\n");
	}
	return ret;
}
"""


@pytest.fixture(scope="module")
def harness(tmp_path_factory):
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        pytest.skip("no host C compiler")
    work = tmp_path_factory.mktemp("forwarders")
    for rel, text in SHIM.items():
        path = work / "shim" / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    src = work / "harness.c"
    src.write_text(HARNESS)
    exe = work / "harness"
    subprocess.run([cc, "-O1", "-Wall", "-pthread", "-I", str(work / "shim"),
                    "-I", str(MULTIAPP / "src"), "-I", str(REPO / "apps/osmosdr/src"),
                    "-I", str(MULTIAPP / "src"), "-o", str(exe), str(src)], check=True)
    return exe


def run(harness, scenario):
    out = subprocess.run([harness, scenario], capture_output=True, text=True, timeout=30)
    assert out.returncode == 0, out.stdout + out.stderr
    assert out.stdout.strip() == "ok"


def test_a_caller_after_the_wait_finds_no_handler(harness):
    # The race the order rules out: the drop has seen no call inside and the USB thread runs
    # before it forgets the handlers (it counts itself in, then reads the cache).
    run(harness, "gap")


def test_the_drop_waits_for_a_handler_that_is_inside(harness):
    run(harness, "wait")


def test_the_drop_gives_up_on_a_handler_that_does_not_return(harness):
    run(harness, "giveup")
