/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The device's claim on the FAT volume against the PC's USB drive: see drive_proxy.h. Compiled on
 * the host by tests/host/test_multiapp_drive.py as it is.
 */

#include <errno.h>

#include "drive_proxy.h"

void drive_proxy_init(struct drive_proxy *p, const struct drive_proxy_hooks *hooks)
{
	p->draining = false;
	p->claimed = false;
	p->hides = false;
	p->io = 0;
	/* No access has ended yet: a claim need not wait for a quiet period. */
	p->last_io = -(int64_t)DRIVE_QUIET_MS;
	p->hidden_at = 0;
	p->hold_until = 0;
	atomic_set(&p->hold, 0);
	atomic_set(&p->on_bus, 0);
	atomic_ptr_set(&p->busy, NULL);
	atomic_set(&p->busy_calls, 0);
	k_mutex_init(&p->vol);
	p->depth = 0;
	p->held = false;
	p->outer_write = false;
	p->hooks = *hooks;
}

void drive_proxy_on_bus(struct drive_proxy *p, bool on)
{
	atomic_set(&p->on_bus, on ? 1 : 0);
}

bool drive_proxy_is_on_bus(struct drive_proxy *p)
{
	return atomic_get(&p->on_bus) != 0;
}

void drive_proxy_set_busy(struct drive_proxy *p, bool (*busy)(void))
{
	atomic_ptr_set(&p->busy, (void *)busy);
	/* Taken back: the module's code may go as soon as no call of it is left. */
	for (int i = 0; busy == NULL && atomic_get(&p->busy_calls) != 0 && i < 1000; i++) {
		k_msleep(1);
	}
}

/* A call of the radio module's, counted in before the pointer is read. */
bool drive_proxy_ring_busy(struct drive_proxy *p)
{
	bool (*busy)(void);
	bool ret = false;

	atomic_inc(&p->busy_calls);
	busy = (bool (*)(void))atomic_ptr_get(&p->busy);
	if (busy != NULL) {
		ret = busy();
	}
	atomic_dec(&p->busy_calls);
	return ret;
}

/*
 * Whether the hold of a write is over, ending it when it is due: the 3 s have passed and no ring
 * runs. The ring is asked outside the lock (module code).
 */
static bool hold_over(struct drive_proxy *p)
{
	bool busy, over;
	k_spinlock_key_t key;

	if (atomic_get(&p->hold) == 0) {
		return true;
	}
	busy = drive_proxy_ring_busy(p);
	key = k_spin_lock(&p->lock);
	if (k_uptime_get() >= p->hold_until && !busy) {
		atomic_set(&p->hold, 0);
	}
	over = atomic_get(&p->hold) == 0;
	k_spin_unlock(&p->lock, key);
	return over;
}

bool drive_proxy_visible(struct drive_proxy *p)
{
	bool over = hold_over(p);
	k_spinlock_key_t key = k_spin_lock(&p->lock);
	bool visible = over && !(p->hides && (p->draining || p->claimed));

	k_spin_unlock(&p->lock, key);
	return visible;
}

bool drive_proxy_io_begin(struct drive_proxy *p)
{
	int64_t start = k_uptime_get();

	for (;;) {
		bool over = hold_over(p);
		k_spinlock_key_t key = k_spin_lock(&p->lock);
		bool wait = false, ok = false;

		if (p->claimed) {
			/* A write has the volume: fail. A read has it: wait for its end. */
			wait = !p->hides;
		} else if (p->draining || over) {
			/* The command in flight may finish while a claim waits for its end. */
			ok = true;
			p->io++;
		}
		k_spin_unlock(&p->lock, key);
		if (ok) {
			return true;
		}
		if (!wait || k_uptime_get() - start >= DRIVE_GATE_MAX_MS) {
			return false;
		}
		k_msleep(DRIVE_POLL_MS);
	}
}

void drive_proxy_io_end(struct drive_proxy *p)
{
	k_spinlock_key_t key = k_spin_lock(&p->lock);

	p->io--;
	p->last_io = k_uptime_get();
	k_spin_unlock(&p->lock, key);
}

/* Wait for the PC's command to end (a write hides the medium meanwhile): 0, or -EBUSY with the state as it was. */
static int claim(struct drive_proxy *p, k_timepoint_t end, bool write)
{
	k_spinlock_key_t key = k_spin_lock(&p->lock);
	int64_t now = k_uptime_get();

	/* In a hold the period goes on, so that it counts from when the medium first went. */
	if (write && atomic_get(&p->hold) == 0) {
		p->hidden_at = now;
	}
	p->hides = write;
	p->draining = true;
	k_spin_unlock(&p->lock, key);

	for (;;) {
		key = k_spin_lock(&p->lock);
		now = k_uptime_get();
		if (p->io == 0U && now - p->last_io >= DRIVE_QUIET_MS) {
			p->draining = false;
			p->claimed = true;
			k_spin_unlock(&p->lock, key);
			return 0;
		}
		if (sys_timepoint_expired(end)) {
			p->draining = false;
			p->hides = false;
			k_spin_unlock(&p->lock, key);
			return -EBUSY;
		}
		k_spin_unlock(&p->lock, key);
		k_msleep(DRIVE_POLL_MS);
	}
}

/* The device is done: after a write the medium stays away until the period has lasted long enough. */
static void release(struct drive_proxy *p, bool write)
{
	k_spinlock_key_t key = k_spin_lock(&p->lock);
	int64_t until = p->hidden_at + DRIVE_HIDE_MIN_MS;

	p->claimed = false;
	p->hides = false;
	if (write) {
		if (until > p->hold_until) {
			p->hold_until = until;
		}
		atomic_set(&p->hold, 1);
	}
	k_spin_unlock(&p->lock, key);
}

int drive_take(struct drive_proxy *p, k_timeout_t timeout, bool write)
{
	k_timepoint_t end = sys_timepoint_calc(timeout);
	int ret = 0;

	if (p->hooks.in_usb_thread != NULL && p->hooks.in_usb_thread()) {
		return -EDEADLK;
	}
	if (k_mutex_lock(&p->vol, timeout) != 0) {
		return -EAGAIN;
	}
	if (p->depth > 0 && write && !p->outer_write) {
		/* A read cannot become a write: it did not hide the medium. */
		k_mutex_unlock(&p->vol);
		return -EDEADLK;
	}
	if (p->depth == 0) {
		p->outer_write = write;
		if (drive_proxy_is_on_bus(p)) {
			ret = claim(p, end, write);
			if (ret == 0 && p->hooks.remount != NULL) {
				ret = p->hooks.remount();
				if (ret != 0) {
					/* The volume may be in any state: a writer's PC reads it again. */
					release(p, write);
				}
			}
			if (ret != 0) {
				k_mutex_unlock(&p->vol);
				return ret;
			}
			p->held = true;
		}
	}
	p->depth++;
	return 0;
}

void drive_give(struct drive_proxy *p)
{
	if (--p->depth == 0 && p->held) {
		bool write = p->outer_write;

		p->held = false;
		if (write && p->hooks.sync != NULL) {
			p->hooks.sync();
		}
		release(p, write);
	}
	k_mutex_unlock(&p->vol);
}
