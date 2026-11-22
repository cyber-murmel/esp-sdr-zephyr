/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The "it is running" mark of the crash-loop guard (src/apps.c, doc/apps/multiapp-internals.md,
 * "Crash loops") for an app whose main() does not return: a delayed work that calls @a mark once the
 * app has been running for a while, unless it was cancelled or armed again since (another app
 * started, the app was unloaded).
 */

#ifndef MULTIAPP_RUNGUARD_H_
#define MULTIAPP_RUNGUARD_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

struct runguard {
	struct k_work_delayable work;
	struct k_spinlock lock;
	/* Called in the system work queue's thread, once per arming that was not cancelled. */
	void (*mark)(void);
	bool armed;
};

void runguard_init(struct runguard *g, void (*mark)(void));
/* Call @a mark in @a delay_ms; arming again restarts the wait (a new app started). */
void runguard_arm(struct runguard *g, uint32_t delay_ms);
/* Take the mark back, and wait for one that is running now; not from the work queue's thread. */
void runguard_cancel(struct runguard *g);

#endif /* MULTIAPP_RUNGUARD_H_ */
