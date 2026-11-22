/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The stages of the running app (include/multiapp/stage.h) and the swap protocol
 * (doc/apps/multiapp-internals.md, "Hot-swapping a stage"):
 *
 * - an app registers each stage as it loads, with its function type's spelling; the host hashes it
 *   and a kernel must carry the same;
 * - a swap sets the new function where the app's thread installs it at its next sync or idle
 *   call, or, while the app is idle, installs it at once. All of it is under one spinlock per stage,
 *   so no CPU reads the pointer half way and the old function is never in a call when the swap
 *   returns: the thread that runs the stage is the one that makes the stage quiescent. One thread
 *   is inside at a time (the owner, whose sync found the stage idle, until its idle call): another
 *   thread's idle changes nothing, and another thread's sync while the owner is inside makes the
 *   stage "shared", which refuses every swap (the host cannot know when the second thread leaves);
 * - the caller waits for the ack and, if the app never gets there, takes the request back
 *   (under the lock, so that an install that just happened is told from one that did not) and
 *   unloads the kernel;
 * - after the ack the previous kernel is unloaded; "revert" swaps to the app's own function and
 *   unloads; an app's unload drops every kernel first, as they hold its use count. A stage can be a
 *   module's (the 802.15.4 driver's tx_gen), whose threads outlive the app: the drop closes the stage
 *   (a sync returns the built-in function and takes no ownership, so nobody new enters a kernel that
 *   is going), waits a bounded time for the owner to leave and, if it does not, leaves the kernel
 *   loaded and fails (-EBUSY: the switch falls back to a reboot);
 * - a stage that asked for internal RAM (MULTIAPP_STAGE_DEFINE_INTERNAL()) takes no kernel whose
 *   function is not in the internal region it was loaded into.
 *
 * The shell side (one swap or load at a time) holds stage_lock; the app's calls only the stage's
 * spinlock. This file does not know llext: apps.c loads and unloads, tests/host compiles this
 * file against stubs of those.
 */

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <esp_sdr/esp_sdr_place.h>

#include <multiapp/stage.h>

#include "apps.h"

LOG_MODULE_DECLARE(multiapp, LOG_LEVEL_INF);

#define STAGES_MAX 4

/* How long a drop waits for a thread that is still inside a stage to leave it. */
#ifndef MULTIAPP_STAGE_QUIET_MS
#define MULTIAPP_STAGE_QUIET_MS CONFIG_MULTIAPP_STAGE_TIMEOUT_MS
#endif
/* A thread that ended inside the stage (the app's threads are aborted before the drop) is not inside. */
#ifndef MULTIAPP_STAGE_OWNER_DEAD
#define MULTIAPP_STAGE_OWNER_DEAD(t) (((t)->base.thread_state & _THREAD_DEAD) != 0U)
#endif

struct slot {
	struct multiapp_stage *st;
	/* FNV-1a of the type spelling, from the app's side. */
	uint32_t sig;
	struct k_spinlock lock;
	struct k_sem ack;
	/* Under lock: */
	void *pending;
	/* The thread inside the stage, NULL when it is idle. */
	k_tid_t owner;
	/* A second thread synced while the owner was inside: no swap is safe. */
	bool shared;
	/* A drop of the stage's kernel is under way: sync returns the built-in function. */
	bool closed;
	uint32_t applied_cyc;
	uint32_t swaps;
	/* Under stage_lock: a kernel runs this stage (kernels[], same index). */
	bool has_kernel;
};

/* Kernel objects: internal RAM, where a spinlock and a semaphore must be (it is scarce on the S31). */
static struct slot slots[STAGES_MAX];
/*
 * The kernels' bookkeeping, and the kernel being installed and the one it replaces, which the
 * shell's small stack has no room for: PSRAM, cold.
 */
static ESP_SDR_DATA(BULK, kernels) struct multiapp_kernel kernels[STAGES_MAX];
static ESP_SDR_DATA(BULK, newk) struct multiapp_kernel newk;
static ESP_SDR_DATA(BULK, oldk) struct multiapp_kernel oldk;

#define KERNEL(s) (&kernels[(s) - slots])
/* The shell's side: one load, swap or listing at a time; the registry changes under it too. */
static K_MUTEX_DEFINE(stage_lock);

static uint32_t sig_hash(const char *text)
{
	uint32_t h = 2166136261U;

	for (; *text != '\0'; text++) {
		h = (h ^ (uint8_t)*text) * 16777619U;
	}
	return h;
}

static struct slot *slot_find(const char *name)
{
	for (size_t i = 0; i < STAGES_MAX; i++) {
		if (slots[i].st != NULL && strcmp(slots[i].st->name, name) == 0) {
			return &slots[i];
		}
	}
	return NULL;
}

void multiapp_stage_register(struct multiapp_stage *st)
{
	struct slot *s = NULL;

	k_mutex_lock(&stage_lock, K_FOREVER);
	st->host = NULL;
	if (slot_find(st->name) != NULL) {
		LOG_ERR("stage %s: the name is taken, it stays on its default", st->name);
	} else {
		for (size_t i = 0; s == NULL && i < STAGES_MAX; i++) {
			s = slots[i].st == NULL ? &slots[i] : NULL;
		}
		if (s == NULL) {
			LOG_ERR("stage %s: more than %d stages, it stays on its default", st->name,
				STAGES_MAX);
		}
	}
	if (s != NULL) {
		memset(s, 0, sizeof(*s));
		s->st = st;
		s->sig = sig_hash(st->sig_text);
		/* Nothing is inside a stage until the app's first sync (owner NULL). */
		k_sem_init(&s->ack, 0, 1);
		st->host = s;
	}
	k_mutex_unlock(&stage_lock);
}

void multiapp_stage_unregister(struct multiapp_stage *st)
{
	struct slot *s = st->host;

	if (s == NULL) {
		return;
	}
	k_mutex_lock(&stage_lock, K_FOREVER);
	st->cur = st->dflt;
	if (s->has_kernel) {
		/* Not reached when apps.c drops the kernels first (a kernel that imports from the app
		 * keeps its use count): the app is going, nothing runs the stage, so it goes too. */
		int ret = multiapp_kernel_unload(KERNEL(s));

		LOG_WRN("stage %s: unregistered with kernel %s loaded, %s", st->name,
			KERNEL(s)->name, ret == 0 ? "unloaded" : "it stays loaded, unused");
	}
	st->host = NULL;
	memset(s, 0, sizeof(*s));
	k_mutex_unlock(&stage_lock);
}

/* With the lock held: install @p fn and note when. */
static void apply(struct slot *s, void *fn)
{
	s->st->cur = fn;
	s->applied_cyc = k_cycle_get_32();
	s->swaps++;
}

void *multiapp_stage_sync(struct multiapp_stage *st)
{
	struct slot *s = st->host;
	k_tid_t me = k_current_get();
	k_spinlock_key_t key;
	bool ack = false, breach = false;
	void *fn;

	if (s == NULL) {
		return st->cur;
	}
	key = k_spin_lock(&s->lock);
	if (s->closed) {
		/*
		 * The kernel is being dropped: the built-in function, which never unloads, and no
		 * ownership, so that the drop is not kept waiting for a call that is not in the kernel.
		 */
		fn = st->dflt;
		k_spin_unlock(&s->lock, key);
		return fn;
	}
	if (s->owner != NULL && s->owner != me) {
		/* Two threads inside: whatever the host swaps, one of them may still run the old one. */
		breach = !s->shared;
		s->shared = true;
	} else {
		if (s->pending != NULL) {
			apply(s, s->pending);
			s->pending = NULL;
			ack = true;
		}
		s->owner = me;
	}
	fn = st->cur;
	k_spin_unlock(&s->lock, key);
	if (ack) {
		k_sem_give(&s->ack);
	}
	if (breach) {
		LOG_ERR("stage %s: a second thread synced while another is inside, swaps are refused",
			st->name);
	}
	return fn;
}

void multiapp_stage_idle(struct multiapp_stage *st)
{
	struct slot *s = st->host;
	k_tid_t me = k_current_get();
	k_spinlock_key_t key;
	bool ack = false, ignored = false;

	if (s == NULL) {
		return;
	}
	key = k_spin_lock(&s->lock);
	if (s->owner == me) {
		s->owner = NULL;
		/* Nobody is inside now: a swap asked for in the meantime can happen here. */
		if (s->pending != NULL) {
			apply(s, s->pending);
			s->pending = NULL;
			ack = true;
		}
	} else if (s->owner != NULL) {
		/* Not the thread that is inside: it does not make the stage quiescent (a sync that a
		 * closed stage answered with the built-in function leaves no owner, and no warning). */
		ignored = !s->closed;
	}
	k_spin_unlock(&s->lock, key);
	if (ack) {
		k_sem_give(&s->ack);
	}
	if (ignored) {
		LOG_WRN("stage %s: idle from a thread that is not inside, ignored", st->name);
	}
}

/*
 * Make the stage run @p fn. Idle: installed here. Busy: left for the owner's next sync or idle
 * call, and waited for. -ETIMEDOUT with the request taken back if the app does not get there in
 * time; -EBUSY for a stage that two threads were inside at once.
 */
static int stage_request(struct slot *s, void *fn, uint32_t timeout_ms,
			 struct multiapp_stage_result *res)
{
	uint32_t t0 = k_cycle_get_32();
	k_spinlock_key_t key;
	bool immediate = false;

	/* Nothing else gives it: the caller has stage_lock, and a taken-back request leaves none. */
	k_sem_reset(&s->ack);
	key = k_spin_lock(&s->lock);
	if (s->shared) {
		k_spin_unlock(&s->lock, key);
		LOG_ERR("stage %s: two threads were inside it at once, swaps are refused",
			s->st->name);
		return -EBUSY;
	}
	if (s->owner == NULL) {
		apply(s, fn);
		immediate = true;
	} else {
		s->pending = fn;
	}
	k_spin_unlock(&s->lock, key);
	if (!immediate && k_sem_take(&s->ack, K_MSEC(timeout_ms)) != 0) {
		bool undone = false;

		key = k_spin_lock(&s->lock);
		if (s->pending == fn) {
			s->pending = NULL;
			undone = true;
		}
		k_spin_unlock(&s->lock, key);
		if (undone) {
			return -ETIMEDOUT;
		}
		/* The app installed it as the wait ended; its ack is on its way. */
		(void)k_sem_take(&s->ack, K_MSEC(100));
	}
	key = k_spin_lock(&s->lock);
	res->latency_us = k_cyc_to_us_floor32(s->applied_cyc - t0);
	k_spin_unlock(&s->lock, key);
	res->immediate = immediate;
	return 0;
}

static bool stage_shared(struct slot *s)
{
	k_spinlock_key_t key = k_spin_lock(&s->lock);
	bool shared = s->shared;

	k_spin_unlock(&s->lock, key);
	return shared;
}

/*
 * Whether the kernel's function runs from internal RAM: the loader put the kernel's fast code
 * (.module_fast) in the internal region heap when its trailer said INTERNAL, and the function lies
 * in it. Code left in .text is in PSRAM whatever the trailer says.
 */
static bool kernel_in_internal_ram(const struct multiapp_kernel *k)
{
	const uintptr_t fn = (uintptr_t)k->fn, lo = (uintptr_t)k->fast;

	return k->place != MULTIAPP_PLACE_PSRAM && k->fast_size != 0U && fn >= lo &&
	       fn - lo < k->fast_size;
}

int multiapp_stage_install(const char *stage, const char *name, const uint8_t *file, size_t len,
			   const char *source, uint32_t timeout_ms, struct multiapp_stage_result *res)
{
	struct multiapp_kernel *k = &newk;
	struct slot *s;
	int ret;

	memset(res, 0, sizeof(*res));
	k_mutex_lock(&stage_lock, K_FOREVER);
	s = slot_find(stage);
	if (s == NULL) {
		ret = -ENOENT;
		goto out;
	}
	(void)strncpy(res->stage, s->st->name, sizeof(res->stage) - 1);
	if (stage_shared(s)) {
		LOG_ERR("stage %s: two threads were inside it at once, swaps are refused", s->st->name);
		ret = -EBUSY;
		goto out;
	}
	ret = multiapp_kernel_load(name, file, len, source, k);
	if (ret != 0) {
		goto out;
	}
	/* The checks the build cannot make for a file that comes from anywhere. */
	if (strcmp(k->desc->stage, s->st->name) != 0) {
		LOG_ERR("%s is a kernel for stage %s, not %s", k->name, k->desc->stage,
			s->st->name);
		ret = -ENOEXEC;
	} else if (k->desc->abi != s->st->abi) {
		LOG_ERR("%s: built against ABI %08x of stage %s, the app has %08x: the headers the "
			"stage shares changed since one of them was built, rebuild the kernel",
			k->name, k->desc->abi, s->st->name, s->st->abi);
		ret = -ENOEXEC;
	} else if (sig_hash(k->desc->sig_text) != s->sig) {
		LOG_ERR("%s: built for \"%s\" (%08x), stage %s has \"%s\" (%08x)", k->name,
			k->desc->sig_text, sig_hash(k->desc->sig_text), s->st->name,
			s->st->sig_text, s->sig);
		ret = -ENOEXEC;
	} else if (s->st->internal && !kernel_in_internal_ram(k)) {
		LOG_ERR("%s: stage %s runs from internal RAM (interrupts off, or at a ring's rate) and "
			"this kernel's function is not: build it INTERNAL, with its code placed by "
			"ESP_SDR_CODE() (multiapp_kernel(... INTERNAL))", k->name, s->st->name);
		ret = -ENOEXEC;
	} else {
		ret = stage_request(s, k->fn, timeout_ms, res);
		if (ret == -ETIMEDOUT) {
			LOG_ERR("stage %s: the app did not reach its sync point in %u ms, "
				"the kernel is unloaded", s->st->name, timeout_ms);
		}
	}
	if (ret != 0) {
		(void)multiapp_kernel_unload(k);
		goto out;
	}
	oldk = *KERNEL(s);
	res->replaced = s->has_kernel;
	*KERNEL(s) = *k;
	s->has_kernel = true;
	res->kernel = *k;
	/* Nobody is inside the old function: the app made the stage quiescent before the ack. */
	if (res->replaced) {
		res->old_ret = multiapp_kernel_unload(&oldk);
		if (res->old_ret != 0) {
			LOG_ERR("%s did not unload (%d): it stays loaded, unused", oldk.name,
				res->old_ret);
		}
	}
out:
	k_mutex_unlock(&stage_lock);
	return ret;
}

int multiapp_stage_revert(const char *stage, uint32_t timeout_ms, struct multiapp_stage_result *res)
{
	struct slot *s;
	int ret;

	memset(res, 0, sizeof(*res));
	k_mutex_lock(&stage_lock, K_FOREVER);
	s = slot_find(stage);
	if (s == NULL) {
		ret = -ENOENT;
		goto out;
	}
	(void)strncpy(res->stage, s->st->name, sizeof(res->stage) - 1);
	if (!s->has_kernel) {
		ret = -EALREADY;
		goto out;
	}
	ret = stage_request(s, s->st->dflt, timeout_ms, res);
	if (ret != 0) {
		goto out;
	}
	res->reverted = true;
	res->kernel = *KERNEL(s);
	res->old_ret = multiapp_kernel_unload(KERNEL(s));
	if (res->old_ret == 0) {
		s->has_kernel = false;
	} else {
		LOG_ERR("%s did not unload (%d): it stays loaded, unused", KERNEL(s)->name,
			res->old_ret);
	}
out:
	k_mutex_unlock(&stage_lock);
	return ret;
}

/*
 * With the stage closed (sync answers with the built-in function and takes no ownership): wait for
 * the thread that is inside to leave. 0 when nobody is, -EBUSY after MULTIAPP_STAGE_QUIET_MS, and for
 * a stage that two threads were inside at once, whose second thread nobody can follow.
 */
static int stage_wait_quiet(struct slot *s)
{
	for (uint32_t ms = 0;; ms++) {
		k_spinlock_key_t key = k_spin_lock(&s->lock);
		k_tid_t owner = s->owner;
		bool shared = s->shared;

		/* A thread that ended inside (the app's are aborted before the drop) is out of it. */
		if (owner != NULL && MULTIAPP_STAGE_OWNER_DEAD(owner)) {
			s->owner = NULL;
			owner = NULL;
		}
		k_spin_unlock(&s->lock, key);
		if (shared) {
			return -EBUSY;
		}
		if (owner == NULL) {
			return 0;
		}
		if (ms >= MULTIAPP_STAGE_QUIET_MS) {
			return -EBUSY;
		}
		k_msleep(1);
	}
}

int multiapp_stage_drop_kernels(void)
{
	int worst = 0;

	k_mutex_lock(&stage_lock, K_FOREVER);
	for (size_t i = 0; i < STAGES_MAX; i++) {
		struct slot *s = &slots[i];
		k_spinlock_key_t key;
		int ret;

		if (s->st == NULL || !s->has_kernel) {
			continue;
		}
		/*
		 * The built-in function from now on, and no new caller of the kernel: a thread of a
		 * module's stage (the driver's transmit path) may sync at any time, and the app's
		 * unload is no quiescent point for it.
		 */
		key = k_spin_lock(&s->lock);
		s->closed = true;
		s->st->cur = s->st->dflt;
		s->pending = NULL;
		k_spin_unlock(&s->lock, key);
		ret = stage_wait_quiet(s);
		if (ret == 0) {
			ret = multiapp_kernel_unload(KERNEL(s));
			if (ret == 0) {
				s->has_kernel = false;
			} else {
				LOG_ERR("stage %s: kernel %s did not unload (%d)", s->st->name,
					KERNEL(s)->name, ret);
			}
		} else {
			LOG_ERR("stage %s: a thread is still inside kernel %s after %u ms (or two were "
				"inside at once): it stays loaded, unused", s->st->name,
				KERNEL(s)->name, (unsigned int)MULTIAPP_STAGE_QUIET_MS);
		}
		if (ret != 0) {
			worst = ret;
		}
		/* Open again: with the built-in function, whatever became of the kernel. */
		key = k_spin_lock(&s->lock);
		s->closed = false;
		k_spin_unlock(&s->lock, key);
	}
	k_mutex_unlock(&stage_lock);
	return worst != 0 ? -EBUSY : 0;
}

bool multiapp_stage_has_kernels(void)
{
	bool any = false;

	k_mutex_lock(&stage_lock, K_FOREVER);
	for (size_t i = 0; i < STAGES_MAX; i++) {
		any = any || (slots[i].st != NULL && slots[i].has_kernel);
	}
	k_mutex_unlock(&stage_lock);
	return any;
}

void multiapp_stage_forget(bool (*in)(const void *obj, void *ctx), void *ctx)
{
	k_mutex_lock(&stage_lock, K_FOREVER);
	for (size_t i = 0; i < STAGES_MAX; i++) {
		if (slots[i].st != NULL && in(slots[i].st, ctx)) {
			LOG_WRN("stage %s: still registered as its extension unloads, forgotten",
				slots[i].st->name);
			memset(&slots[i], 0, sizeof(slots[i]));
		}
	}
	k_mutex_unlock(&stage_lock);
}

bool multiapp_stage_owned_by(const char *name, const struct llext *ext,
			     bool (*holds)(const struct llext *ext, uintptr_t addr))
{
	struct slot *s;
	bool owned;

	/* The mutex is recursive: the shell's thread holds it while it loads a kernel. */
	k_mutex_lock(&stage_lock, K_FOREVER);
	s = slot_find(name);
	owned = s != NULL && holds(ext, (uintptr_t)s->st);
	k_mutex_unlock(&stage_lock);
	return owned;
}

size_t multiapp_stage_count(void)
{
	size_t n = 0;

	for (size_t i = 0; i < STAGES_MAX; i++) {
		n += slots[i].st != NULL;
	}
	return n;
}

bool multiapp_stage_view(size_t i, struct multiapp_stage_view *v)
{
	bool found = false;

	k_mutex_lock(&stage_lock, K_FOREVER);
	for (size_t n = 0; n < STAGES_MAX; n++) {
		struct slot *s = &slots[n];
		k_spinlock_key_t key;

		if (s->st == NULL || i-- != 0) {
			continue;
		}
		memset(v, 0, sizeof(*v));
		(void)strncpy(v->name, s->st->name, sizeof(v->name) - 1);
		v->sig_text = s->st->sig_text;
		v->sig = s->sig;
		v->dflt = s->st->dflt;
		key = k_spin_lock(&s->lock);
		v->cur = s->st->cur;
		v->idle = s->owner == NULL;
		v->shared = s->shared;
		v->abi = s->st->abi;
		v->pending = s->pending != NULL;
		v->swaps = s->swaps;
		k_spin_unlock(&s->lock, key);
		v->has_kernel = s->has_kernel;
		v->kernel = *KERNEL(s);
		found = true;
		break;
	}
	k_mutex_unlock(&stage_lock);
	return found;
}
