/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See runguard.h.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "runguard.h"

static void handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct runguard *g = CONTAINER_OF(dwork, struct runguard, work);
	k_spinlock_key_t key = k_spin_lock(&g->lock);
	bool fire = g->armed;

	g->armed = false;
	k_spin_unlock(&g->lock, key);
	if (fire) {
		g->mark();
	}
}

void runguard_init(struct runguard *g, void (*mark)(void))
{
	g->mark = mark;
	g->armed = false;
	k_work_init_delayable(&g->work, handler);
}

void runguard_arm(struct runguard *g, uint32_t delay_ms)
{
	k_spinlock_key_t key = k_spin_lock(&g->lock);

	g->armed = true;
	k_spin_unlock(&g->lock, key);
	(void)k_work_reschedule(&g->work, K_MSEC(delay_ms));
}

void runguard_cancel(struct runguard *g)
{
	struct k_work_sync sync;
	k_spinlock_key_t key = k_spin_lock(&g->lock);

	g->armed = false;
	k_spin_unlock(&g->lock, key);
	(void)k_work_cancel_delayable_sync(&g->work, &sync);
}
