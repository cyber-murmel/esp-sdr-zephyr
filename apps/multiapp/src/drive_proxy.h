/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Who has the FAT volume, the PC over USB mass storage or the device (doc/apps/multiapp-internals.md,
 * "Writers and readers of the volume"). The drive's LUN goes through a proxy disk (src/drive.c) that
 * asks this state machine before every status and every disk access. The device has two ways to
 * take the volume, both after the PC's command in flight has ended (no PC disk access running and
 * DRIVE_QUIET_MS since the last):
 *
 * - A read (an app's file, a kernel file) does not hide the medium: the PC's disk accesses wait
 *   until the read is over and then go on, so a copy in progress is not cut, only delayed. The
 *   device mounts the volume afresh, so what the PC has written is what it reads; a file the PC is
 *   still copying fails the trailer's CRC and the reader tries again.
 * - A write hides the medium. Zephyr's MSC class has no unit attention and no media-change call:
 *   the one thing the device can tell the PC is "NOT READY, medium not present", after which the PC
 *   drops what it cached and reads the volume again when the medium is back (udisks polls every
 *   2 s). The medium stays away for at least DRIVE_HIDE_MIN_MS from when it went, and, while a ring
 *   runs, until it stops: the PC's rescan must not land in a stream.
 *
 * States: visible; draining (a claim waits for the command in flight: accesses are allowed; a write
 * hides the medium from here on); claimed (a write refuses the PC's accesses, a read makes them wait);
 * hold (a write is over and the medium stays away).
 *
 * Only the claim holder moves the state (the device's users take one lock first), and only the MSC
 * thread asks. No flash access and no thread of its own: the time is read when it matters. A device
 * user must not be the USB thread: its callbacks deliver the PC's data, and a claim that sleeps there
 * waits for a PC command that cannot go on (drive_take() refuses with -EDEADLK).
 */

#ifndef MULTIAPP_DRIVE_PROXY_H_
#define MULTIAPP_DRIVE_PROXY_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

/* A write keeps the medium away at least this long (udisks' media poll is every 2 s). */
#define DRIVE_HIDE_MIN_MS 3000
/*
 * A PC command is over when its last disk access ended this long ago and none is running. A PC that
 * stalls longer in the middle of a command (it waits for data) looks over: a read goes on and the
 * rest of the PC's command waits for it; a write cuts the command.
 */
#define DRIVE_QUIET_MS    100
/* How often a waiting claim, and a PC access waiting for a read, look again. */
#define DRIVE_POLL_MS     5
/* A PC access waits this long for a read that does not end before it fails. */
#define DRIVE_GATE_MAX_MS 10000

struct drive_proxy_hooks {
	/* Drop what the device cached of the volume (FatFs's window), after a claim: 0 or -errno. */
	int (*remount)(void);
	/* Write out what a writer wrote, before its claim ends. */
	void (*sync)(void);
	/* Whether the calling thread is the USB thread, which must not claim. */
	bool (*in_usb_thread)(void);
};

struct drive_proxy {
	struct k_spinlock lock;
	/* Under lock: */
	bool draining;
	bool claimed;
	/* The claim in progress is a write: the medium goes away. */
	bool hides;
	/* The PC's disk accesses running, and when the last one ended (ms of k_uptime_get()). */
	uint32_t io;
	int64_t last_io;
	/* The hidden period began then, and the medium stays away until then. */
	int64_t hidden_at;
	int64_t hold_until;
	/* After a write, until hold_until and the ring stops (read without the lock). */
	atomic_t hold;

	/* Set while the drive is on the bus (the class is registered): else nobody to hide from. */
	atomic_t on_bus;
	/* Whether a ring runs (the radio module's), counted in so that its unload can wait for calls. */
	atomic_ptr_t busy;
	atomic_t busy_calls;
	/* The device's users of the volume, one at a time; only its holder touches the next ones. */
	struct k_mutex vol;
	int depth;
	bool held;
	bool outer_write;
	struct drive_proxy_hooks hooks;
};

void drive_proxy_init(struct drive_proxy *p, const struct drive_proxy_hooks *hooks);

/* The proxy disk's status: whether the PC has the medium. */
bool drive_proxy_visible(struct drive_proxy *p);
/*
 * The proxy disk's accesses: true to go ahead, false to fail (the device has the volume and writes
 * it, the medium is away, or a read did not end in DRIVE_GATE_MAX_MS). Waits while the device reads.
 */
bool drive_proxy_io_begin(struct drive_proxy *p);
void drive_proxy_io_end(struct drive_proxy *p);

/*
 * The device takes the volume: wait for the PC's command in flight to end (a write hides the medium
 * from the start of the wait), then remount. take/give nest in one thread, and one thread at a time
 * holds it. 0, -EAGAIN (another thread kept it for @p timeout), -EBUSY (the PC's command did not
 * end in that time: the medium is back as it was), -EDEADLK (the USB thread, or a write inside a
 * read) or the remount's error. A no-op beyond the lock while the drive is not on the bus.
 */
int drive_take(struct drive_proxy *p, k_timeout_t timeout, bool write);
/* The outermost give of a writer syncs and starts the hold. */
void drive_give(struct drive_proxy *p);

void drive_proxy_on_bus(struct drive_proxy *p, bool on);
bool drive_proxy_is_on_bus(struct drive_proxy *p);

/*
 * The radio module says whether a ring runs, for as long as it is loaded: a hold is not over while
 * it returns true. NULL takes it back, after which no call of it is running.
 */
void drive_proxy_set_busy(struct drive_proxy *p, bool (*busy)(void));

#endif /* MULTIAPP_DRIVE_PROXY_H_ */
