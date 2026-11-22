/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The network services' gate (doc/dev/network-services.md, phase 2): MCUmgr over UDP refuses every
 * command until `mcumgr arm <minutes>` on a shell, and then refuses what would hurt:
 *  - every command while disarmed (and after the minutes ran out);
 *  - every command of the image and file groups, reads included, while a stream runs (flash access
 *    turns the cache off), with its own error code MGMT_ERR_STREAMING: a ring runs
 *    (multiapp_ring_running(), the radio module's diagnostic bus or sample ring), or an app holds
 *    multiapp_stream_hold() (no app does yet);
 *  - file access outside /FLASH:/, and *.llext writes in a build that cannot verify signatures;
 *  - file commands take the volume from the PC (drive.c, multiapp_drive_fs_begin()): a transfer for its
 *    whole length and with the drive hidden, a stat or hash for the command; see "The volume across a
 *    transfer" below. A PC that keeps its disk access busy gets "busy" (rc 10).
 * While disarmed the board answers every command, with the error "access denied" (rc 11), and does
 * nothing else. A reboot disarms: the state is RAM only.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/grp/fs_mgmt/fs_mgmt.h>
#include <zephyr/mgmt/mcumgr/grp/fs_mgmt/fs_mgmt.h>
#include <zephyr/mgmt/mcumgr/grp/fs_mgmt/fs_mgmt_callbacks.h>
#include <zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt_callbacks.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>

#include <multiapp/multiapp.h>

#include "apps.h"
#include "extsig.h"
#include "netpath.h"
#include "streamgate.h"

LOG_MODULE_REGISTER(mcumgr, LOG_LEVEL_INF);

#define VOLUME NETPATH_VOLUME
/* The first per-user code: "streaming: flash access refused" (mcumgrlib.py knows it) */
#define MGMT_ERR_STREAMING MGMT_ERR_EPERUSER
#define MAX_MINUTES 60
/* One log line a second for refusals, the rest counted. */
#define LOG_GAP_MS 1000

/* Uptime second (+1) up to which the gate is open; 0 is closed. */
static atomic_t armed_until;
static atomic_t stream_holds;
static atomic_t refused_quiet;
static int64_t last_refusal_log = -LOG_GAP_MS;

static void expire_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(expire_work, expire_fn);

static uint32_t now_s(void)
{
	return (uint32_t)(k_uptime_get() / MSEC_PER_SEC) + 1U;
}

static bool armed(void)
{
	uint32_t until = (uint32_t)atomic_get(&armed_until);

	return until != 0U && (int32_t)(until - now_s()) > 0;
}

static void expire_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	if (armed()) {
		/* The seconds are rounded: once more, a second later */
		k_work_reschedule(&expire_work, K_SECONDS(1));
	} else if (atomic_set(&armed_until, 0) != 0) {
		LOG_INF("disarmed (time ran out)");
	}
}

static void hold_release_soon(void);

void multiapp_stream_hold(void)
{
	atomic_inc(&stream_holds);
	/* A stream is starting: the volume goes back at once (the file commands are refused from now) */
	hold_release_soon();
}

void multiapp_stream_release(void)
{
	if (atomic_get(&stream_holds) > 0) {
		atomic_dec(&stream_holds);
	}
}

/* The SDR's USB class (src/osmosdr.c, apps/osmosdr/src/usb_sdr.c): true when the PC has it configured */
extern bool usb_sdr_enabled(void);

/*
 * A ring that nobody reads is no stream to protect: its samples are lost anyway. The PC may also pause its reads
 * for a while (a program that stalls, a USB hub): 5 s without a finished IN transfer is taken for a consumer that
 * went, shorter would let a flash write, whose cache stall costs the stream samples, into a stream that is only slow.
 */
#define CONSUMER_MS 5000

/*
 * A stream is a ring that runs and, when its consumer is the PC over the SDR's USB class, whose IN transfers the
 * PC keeps finishing (apps.c, multiapp_osmosdr_last_in_ms()): see stream_consumer_live(). A PC program that was
 * killed, or crashed, leaves the device's ring running with no one reading: CONSUMER_MS after its last transfer the
 * flash is free again for updates and key changes. Any other ring (an app without that USB class) is a stream as
 * long as it runs.
 */
static bool stream_busy(void)
{
	static struct k_spinlock lock;
	static bool ring_seen;
	static uint32_t ring_since;
	k_spinlock_key_t key;
	uint32_t now, last, since;

	if (atomic_get(&stream_holds) > 0) {
		return true;
	}
	if (!multiapp_ring_running()) {
		key = k_spin_lock(&lock);
		ring_seen = false;
		k_spin_unlock(&lock, key);
		return false;
	}
	if (!usb_sdr_enabled()) {
		return true;
	}
	/*
	 * Callers run on several threads: the first look and the reads of "last" and "now" are one step, so that
	 * ring_since is never later than the "now" of another caller (an unsigned age would wrap). "last" is read
	 * first: a transfer that finishes between the two reads is then not after "now".
	 */
	key = k_spin_lock(&lock);
	last = multiapp_osmosdr_last_in_ms();
	now = k_uptime_get_32();
	if (!ring_seen) {
		ring_since = now;
		ring_seen = true;
	}
	since = ring_since;
	k_spin_unlock(&lock, key);
	return stream_consumer_live(now, last, since, CONSUMER_MS);
}

/* The same rule for the other network services (SSH, src/sshd.c): no flash access while a stream runs */
bool multiapp_net_stream_busy(void)
{
	return stream_busy();
}

/* A refusal: logged once a second, the others counted. */
static void refused(const char *what, const char *why)
{
	int64_t now = k_uptime_get();

	if (now - last_refusal_log < LOG_GAP_MS) {
		atomic_inc(&refused_quiet);
		return;
	}
	last_refusal_log = now;
	LOG_WRN("refused %s: %s (%ld more since the last line)", what, why,
		(long)atomic_set(&refused_quiet, 0));
}

static enum mgmt_cb_return fs_lock(int32_t *rc, uint8_t id, uint8_t op);
static enum mgmt_cb_return fs_fresh(int32_t *rc, enum fs_mgmt_file_access_types access);


static enum mgmt_cb_return deny(int32_t *rc, int code)
{
	*rc = code;
	return MGMT_CB_ERROR_RC;
}

/* Every command: a closed gate answers nothing but "access denied". */
static enum mgmt_cb_return cmd_recv(uint32_t event, enum mgmt_cb_return prev, int32_t *rc,
				    uint16_t *group, bool *abort_more, void *data, size_t size)
{
	ARG_UNUSED(event);
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);
	ARG_UNUSED(size);
	if (prev != MGMT_CB_OK) {
		return prev;
	}
	if (armed()) {
		const struct mgmt_evt_op_cmd_arg *arg = data;

		/*
		 * Reads stall the flash cache as writes do (a stream gaps twice per 3 KB file read): the whole
		 * image group (slot headers, state, upload, erase) and the file group go, every op. The os group
		 * (echo, parameters, info) runs from RAM.
		 */
		if (arg != NULL && (arg->group == MGMT_GROUP_ID_IMAGE || arg->group == MGMT_GROUP_ID_FS) &&
		    stream_busy()) {
			refused("flash access", "streaming");
			hold_release_soon();
			return deny(rc, MGMT_ERR_STREAMING);
		}
		if (arg != NULL && arg->group == MGMT_GROUP_ID_FS) {
			return fs_lock(rc, arg->id, arg->op);
		}
		return prev;
	}
	if (data != NULL) {
		const struct mgmt_evt_op_cmd_arg *arg = data;
		char what[24];

		snprintf(what, sizeof(what), "group %u id %u", arg->group, arg->id);
		refused(what, "disarmed");
	}
	return deny(rc, MGMT_ERR_EACCESSDENIED);
}

/* Each upload chunk: not while a stream runs. */
static enum mgmt_cb_return img_chunk(uint32_t event, enum mgmt_cb_return prev, int32_t *rc,
				     uint16_t *group, bool *abort_more, void *data, size_t size)
{
	ARG_UNUSED(event);
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);
	ARG_UNUSED(data);
	ARG_UNUSED(size);
	if (prev != MGMT_CB_OK || !stream_busy()) {
		return prev;
	}
	refused("image upload", "streaming");
	return deny(rc, MGMT_ERR_STREAMING);
}

static enum mgmt_cb_return file_access(uint32_t event, enum mgmt_cb_return prev, int32_t *rc,
				       uint16_t *group, bool *abort_more, void *data, size_t size)
{
	const struct fs_mgmt_file_access *fa = data;

	ARG_UNUSED(event);
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);
	ARG_UNUSED(size);
	if (prev != MGMT_CB_OK) {
		return prev;
	}
	if (!netpath_ok(fa->filename)) {
		refused("file access", "path outside " VOLUME);
		return deny(rc, MGMT_ERR_EACCESSDENIED);
	}
	/* Written extensions are loaded only after their signature checks (src/extsig.c) */
	if (fa->access == FS_MGMT_FILE_ACCESS_WRITE && !multiapp_sig_verifies() &&
	    netpath_is_extension(fa->filename)) {
		refused("file write", "this build does not verify extension signatures");
		return deny(rc, MGMT_ERR_EACCESSDENIED);
	}
	/* A new transfer (a new path): a mount of its own, see fs_fresh() */
	return fs_fresh(rc, fa->access);
}

/*
 * The volume across a transfer. fs_mgmt keeps its file open between the requests of a transfer
 * (chunk after chunk, closed when the transfer ends or after its idle time), so a claim and a mount
 * that end with each command would leave it a dead handle. A transfer (upload or download) therefore
 * claims the volume with its first command and gives it back when fs_mgmt says it is done
 * (MGMT_EVT_OP_FS_MGMT_FILE_ACCESS_DONE), when a stream starts, or HOLD_IDLE_MS after its last
 * command. fs_mgmt closes an abandoned file after FS_IDLE_CLOSE (mcumgr.conf), earlier than that,
 * still under the claim. The other file commands (stat, hash, crc) are short: they take the volume
 * for themselves and give it back at their end. So the loader (`multiapp run`, `stage load`) that
 * follows a transfer finds the volume free.
 *
 * A transfer HIDES the drive from the PC (the write claim of multiapp.h, with a read-only mount for
 * a download): a read claim would make the PC's disk accesses wait, which they do for 10 s at most
 * (drive_proxy.h), and a long download outlasts that with I/O errors for the PC. The short commands
 * take a read claim.
 *
 * A claim is a k_mutex owner's: the thread that took it must give it, so a thread of its own
 * (hold_q, an internal stack: the flash driver turns the cache off) takes and gives it, while the
 * MCUmgr thread does the file IO under it. A new transfer, once the last one has ended, gets a mount
 * of its own (drop, then claim afresh: a held mount reused by a new transfer failed on the board).
 * Never while fs_mgmt may still have a file open (xfer_open(): its last transfer command is less
 * than XFER_OPEN_MS old): the mount stays then, and a command that needs more than it has is
 * answered busy.
 */
#define HOLD_IDLE_MS 4000
#define HOLD_WAIT_MS 2000
#define XFER_OPEN_MS 3500

static K_THREAD_STACK_DEFINE(hold_stack, 3072);
static struct k_work_q hold_q;
static const struct k_work_queue_config hold_cfg = {.name = "mcumgr_fs_hold"};
static K_MUTEX_DEFINE(cmd_mtx); /* the MCUmgr thread's, from CMD_RECV to CMD_DONE of a file command */
static K_SEM_DEFINE(acq_done, 0, 1);
static K_SEM_DEFINE(drop_done, 0, 1);
static struct k_work acquire_work;
static struct k_work drop_work;
static struct k_work_delayable release_work;
static bool want_write, want_hide; /* request to the acquire work */
static int acq_ret;
static bool held, held_write, held_hide; /* written by hold_q, read under cmd_mtx */
static atomic_t want_release;
static bool cmd_locked;
/* Under cmd_mtx: the command in hand, and the transfer fs_mgmt may have open */
static bool cmd_transfer, cmd_close, xfer_ended, xfer_is_open;
static int64_t xfer_ms;

static bool xfer_open(void)
{
	return xfer_is_open && k_uptime_get() - xfer_ms < XFER_OPEN_MS;
}

static void acquire_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	acq_ret = multiapp_drive_fs_begin(want_write, want_hide, HOLD_WAIT_MS);
	if (acq_ret != 0) {
		LOG_ERR("volume (%s%s): %d", want_write ? "write" : "read", want_hide ? ", hidden" : "",
			acq_ret);
	} else {
		held = true;
		held_write = want_write;
		held_hide = want_hide;
	}
	k_sem_give(&acq_done);
}

/* The volume goes back now (the MCUmgr thread waits for this, holding cmd_mtx, so the release work
 * cannot run beside it). */
static void drop_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	if (held) {
		multiapp_drive_fs_end(held_write, held_hide);
		held = false;
	}
	k_sem_give(&drop_done);
}

static void release_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	if (k_mutex_lock(&cmd_mtx, K_NO_WAIT) != 0) {
		/* A command is on: after it (soon if it was the transfer's last) */
		k_work_reschedule_for_queue(&hold_q, &release_work,
					    atomic_get(&want_release) != 0 ? K_MSEC(20)
									   : K_MSEC(HOLD_IDLE_MS));
		return;
	}
	if (held) {
		multiapp_drive_fs_end(held_write, held_hide);
		held = false;
	}
	atomic_set(&want_release, 0);
	k_mutex_unlock(&cmd_mtx);
}

static void hold_release_soon(void)
{
	atomic_set(&want_release, 1);
	k_work_reschedule_for_queue(&hold_q, &release_work, K_NO_WAIT);
}

/*
 * Hold the volume for a command that writes (@p write, a read-write mount) and/or hides the drive
 * (@p hide). Held already and enough: kept. Not enough, or @p fresh (the start of a transfer) and no
 * transfer open: dropped and claimed afresh, the MCUmgr thread (the caller) waiting for each step, so
 * that fs_mgmt opens its file on a mount that is there and stays.
 */
static enum mgmt_cb_return fs_claim(int32_t *rc, bool write, bool hide, bool fresh)
{
	bool covers = held && (held_write || !write) && (held_hide || !hide);

	if (held && (!covers || fresh)) {
		if (xfer_open()) {
			if (!covers) {
				refused("file command", "a transfer is open: try again in a few seconds");
				return deny(rc, MGMT_ERR_EBUSY);
			}
			/* Enough as it is: fs_mgmt closes its old file under this mount */
		} else {
			k_sem_reset(&drop_done);
			k_work_submit_to_queue(&hold_q, &drop_work);
			k_sem_take(&drop_done, K_FOREVER);
		}
	}
	if (!held) {
		int ret;

		want_write = write;
		want_hide = hide;
		k_sem_reset(&acq_done);
		k_work_submit_to_queue(&hold_q, &acquire_work);
		k_sem_take(&acq_done, K_FOREVER);
		ret = acq_ret;
		if (ret != 0) {
			/* -EAGAIN another user kept the lock, -EBUSY the PC's disk access did not end,
			 * or the volume would not mount */
			refused("file command", ret == -EBUSY ? "the PC is copying to the drive"
					       : ret == -EAGAIN ? "volume busy" : "volume not usable");
			return deny(rc, ret == -EBUSY || ret == -EAGAIN ? MGMT_ERR_EBUSY
									: MGMT_ERR_EUNKNOWN);
		}
	}
	atomic_set(&want_release, 0);
	k_work_reschedule_for_queue(&hold_q, &release_work, K_MSEC(HOLD_IDLE_MS));
	return MGMT_CB_OK;
}

/* Every file command (CMD_RECV): @p id is its command, @p op its direction */
static enum mgmt_cb_return fs_lock(int32_t *rc, uint8_t id, uint8_t op)
{
	if (cmd_locked) {
		/* The end of the last command did not come */
		k_mutex_unlock(&cmd_mtx);
		cmd_locked = false;
	}
	k_mutex_lock(&cmd_mtx, K_FOREVER);
	cmd_locked = true;
	cmd_transfer = id == FS_MGMT_ID_FILE;
	cmd_close = id == FS_MGMT_ID_OPENED_FILE;
	xfer_ended = false;
	return fs_claim(rc, cmd_transfer && op == MGMT_OP_WRITE, cmd_transfer, false);
}

/*
 * The start of a transfer (fs_mgmt's FILE_ACCESS hook, before it closes its old file and opens the
 * new): a fresh claim and mount when no transfer can be open; a stat or a hash needs nothing more
 * than fs_lock() gave it.
 */
static enum mgmt_cb_return fs_fresh(int32_t *rc, enum fs_mgmt_file_access_types access)
{
	if (!cmd_locked || (access != FS_MGMT_FILE_ACCESS_READ && access != FS_MGMT_FILE_ACCESS_WRITE)) {
		return MGMT_CB_OK;
	}
	return fs_claim(rc, access == FS_MGMT_FILE_ACCESS_WRITE, true, true);
}

static enum mgmt_cb_return cmd_done(uint32_t event, enum mgmt_cb_return prev, int32_t *rc,
				    uint16_t *group, bool *abort_more, void *data, size_t size)
{
	ARG_UNUSED(event);
	ARG_UNUSED(rc);
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);
	ARG_UNUSED(data);
	ARG_UNUSED(size);
	if (cmd_locked) {
		bool early;

		if (cmd_transfer && !xfer_ended) {
			/* fs_mgmt has this transfer's file open until it ends or idles out */
			xfer_is_open = true;
			xfer_ms = k_uptime_get();
		} else if (xfer_ended || cmd_close) {
			xfer_is_open = false;
		}
		/* A transfer's end, or a single-shot command and no transfer open: the volume goes back */
		early = xfer_ended || (!cmd_transfer && !xfer_open());
		cmd_locked = false;
		k_mutex_unlock(&cmd_mtx);
		if (early) {
			hold_release_soon();
		}
	}
	return prev;
}

/* fs_mgmt closed the file (a transfer's last chunk): cmd_done() frees the volume */
static enum mgmt_cb_return file_done(uint32_t event, enum mgmt_cb_return prev, int32_t *rc,
				     uint16_t *group, bool *abort_more, void *data, size_t size)
{
	ARG_UNUSED(event);
	ARG_UNUSED(rc);
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);
	ARG_UNUSED(data);
	ARG_UNUSED(size);
	xfer_ended = true;
	return prev;
}

static struct mgmt_callback cb_filedone = {.callback = file_done,
					   .event_id = MGMT_EVT_OP_FS_MGMT_FILE_ACCESS_DONE};
static struct mgmt_callback cb_done = {.callback = cmd_done, .event_id = MGMT_EVT_OP_CMD_DONE};
static struct mgmt_callback cb_cmd = {.callback = cmd_recv, .event_id = MGMT_EVT_OP_CMD_RECV};
static struct mgmt_callback cb_img = {.callback = img_chunk,
				      .event_id = MGMT_EVT_OP_IMG_MGMT_DFU_CHUNK};
static struct mgmt_callback cb_fs = {.callback = file_access,
				     .event_id = MGMT_EVT_OP_FS_MGMT_FILE_ACCESS};

static int netsvc_init(void)
{
	mgmt_callback_register(&cb_cmd);
	mgmt_callback_register(&cb_done);
	mgmt_callback_register(&cb_filedone);
	k_work_queue_init(&hold_q);
	k_work_queue_start(&hold_q, hold_stack, K_THREAD_STACK_SIZEOF(hold_stack), 11, &hold_cfg);
	k_work_init(&acquire_work, acquire_fn);
	k_work_init(&drop_work, drop_fn);
	k_work_init_delayable(&release_work, release_fn);
	mgmt_callback_register(&cb_img);
	mgmt_callback_register(&cb_fs);
	return 0;
}
SYS_INIT(netsvc_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static int cmd_arm(const struct shell *sh, size_t argc, char **argv)
{
	char *end;
	long minutes = strtol(argv[1], &end, 10);

	if (*end != '\0' || minutes < 1 || minutes > MAX_MINUTES) {
		shell_error(sh, "minutes: 1 to %d", MAX_MINUTES);
		return -EINVAL;
	}
	atomic_set(&armed_until, (atomic_val_t)(now_s() + (uint32_t)minutes * 60U));
	k_work_reschedule(&expire_work, K_MINUTES(minutes));
	LOG_INF("armed for %ld min", minutes);
	shell_print(sh, "MCUmgr armed for %ld min (UDP port %d)", minutes,
		    CONFIG_MCUMGR_TRANSPORT_UDP_PORT);
	return 0;
}

static int cmd_disarm(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	(void)k_work_cancel_delayable(&expire_work);
	atomic_set(&armed_until, 0);
	LOG_INF("disarmed");
	shell_print(sh, "MCUmgr disarmed");
	return 0;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t until = (uint32_t)atomic_get(&armed_until);

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (armed()) {
		shell_print(sh, "armed, %u s left", (unsigned int)(until - now_s()));
	} else {
		shell_print(sh, "disarmed");
	}
	shell_print(sh, "image uploads: %s", stream_busy() ? "refused (a stream runs)" : "allowed");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(mcumgr_cmds,
	SHELL_CMD_ARG(arm, NULL, "<minutes 1.." STRINGIFY(MAX_MINUTES) ">: answer MCUmgr commands",
		      cmd_arm, 2, 0),
	SHELL_CMD(disarm, NULL, "Refuse every MCUmgr command", cmd_disarm),
	SHELL_CMD(status, NULL, "The gate and the upload rule", cmd_status),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(mcumgr, &mcumgr_cmds, "MCUmgr (Simple Management Protocol over UDP) arm gate", NULL);
