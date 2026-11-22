/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The SSH server of the S31 multiapp host (doc/dev/network-services.md, phase 3): public key login
 * only (ssh-rsa, ssh-ed25519 and ecdsa-sha2-nistp256 keys), an ECDSA P-256 host key made on the first
 * boot with the hardware TRNG and kept in the settings (ZMS, the "calibration" partition), the users'
 * keys from /FLASH:/ssh/authorized_keys. The sessions' console is src/console_mux.c's.
 *
 * One thread, sshd_work, does everything that is slow or touches the flash, so that the shell threads
 * (a 3 KB stack) and the server thread never carry the volume: the host key at boot, the key file, the
 * requests of the shell commands, which wait for it, and the commands of `ssh <host> <command>`. It runs
 * at the lowest priority, the shell's: the keys and the file take turns with the shell and the log.
 *
 * Nothing here touches the flash while a stream runs (multiapp_net_stream_busy(), as MCUmgr): the key
 * file is neither read nor written, the new host key waits in RAM to be saved.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <zephyr/app_version.h>
#include <zephyr/fs/fs.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/ssh/keygen.h>
#include <zephyr/net/ssh/server.h>
#include <zephyr/settings/settings.h>
#include <mbedtls/platform_util.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include "apps.h"
#include "authkeys.h"
#include "console_mux.h"
#include "extsig.h"
#include "gitver.h"
#include "netpath.h"

LOG_MODULE_REGISTER(sshd, LOG_LEVEL_INF);

/*
 * Single core only: the PSA exclusion below is by thread priority and the flags and tables here have no locks
 * against another CPU (doc/dev/network-services.md, "SMP").
 */
BUILD_ASSERT(!IS_ENABLED(CONFIG_SMP), "the SSH host code is not SMP safe yet");
BUILD_ASSERT(!IS_ENABLED(CONFIG_TEST_RANDOM_GENERATOR), "the host key needs the hardware TRNG, not the test generator");
BUILD_ASSERT(IS_ENABLED(CONFIG_ENTROPY_HAS_DRIVER), "no entropy driver: the host key would not be random");

#define HOST_SLOT      0
#define FIRST_KEY_SLOT 1
#define KEY_SLOTS      (CONFIG_SSH_MAX_HOST_KEYS - 1)
#define HOSTKEY_BITS   256
#define HOSTKEY_NAME   "ssh/hostkey"
/* Key material: the host key's 32 byte ECDSA scalar (the stored value, read here) */
#define DER_MAX        96
#define KEYS_DIR       NETPATH_VOLUME "ssh"
#define KEYS_FILE      KEYS_DIR "/authorized_keys"
#define LINE_MAX_LEN   1024
#define LIST_MAX       8
/* The lowest application priority, the shell's and the log thread's: key generation and file IO take turns with them */
#define WORK_PRIO      K_LOWEST_APPLICATION_THREAD_PRIO
/* The SSH server's thread priority is CONFIG_SSH_SERVER_THREAD_PRIORITY (10, ssh.conf) */
#define PSA_PRIO       (CONFIG_SSH_SERVER_THREAD_PRIORITY - 1)
#define CLAIM_MS       2000
#define SAVE_RETRY_S   5
#define ECDSA_SCALAR_LEN 32

struct entry {
	char type[AK_TYPE_MAX];
	char comment[AK_COMMENT_MAX];
	char fp[AK_FP_LEN];
	unsigned int bits;
	int slot;          /* in the server, or -1 */
	const char *note;  /* why not */
};

/* The work thread's (and read by the shell commands once it is idle: the requests wait for it). */
static struct entry entries[LIST_MAX];
static int entry_count;
static int slots_used;
static int lines_bad;
static char host_fp[AK_FP_LEN];
static enum { HK_NONE, HK_GENERATING, HK_LOADED, HK_NEW_SAVED, HK_NEW_UNSAVED } hk_state;
static bool hk_ready;       /* the host key is there and the server was started once */
static bool keepalive_paused;

/*
 * TCP keep-alive probes of an idle session cost the stream a gap each on this core (one a minute here): paused
 * while a stream runs, so a client that vanished is found when it ends. The wish is looked at once a second on the
 * system work queue, not on sshd_work (a long command there must not hold it back), and only sets a flag: the
 * server thread, which owns the sockets, applies it.
 */
static void keepalive_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(keepalive_work, keepalive_fn);

static void keepalive_fn(struct k_work *work)
{
	bool stream = multiapp_net_stream_busy();

	ARG_UNUSED(work);
	if (stream != keepalive_paused) {
		keepalive_paused = stream;
		(void)ssh_server_set_tcp_keepalive(ssh_server_instance(0), !stream);
		LOG_INF("TCP keep-alive %s (%s)", stream ? "paused" : "on again", stream ? "a stream runs" : "no stream");
	}
	k_work_reschedule(&keepalive_work, K_SECONDS(1));
}
static bool server_running; /* the server thread runs (it ends on a fatal accept error: started again) */
static bool keys_loaded;    /* the file was read once */
static uint8_t der[DER_MAX];
static uint8_t line_buf[LINE_MAX_LEN];

/* In the LP RAM (multiapp_lpram_s31.ld): it touches the flash, so not PSRAM, and internal RAM is scarce */
static K_THREAD_STACK_DEFINE(sshd_work_stack, 6144);
static struct k_thread work_thread;

/*
 * mbedTLS has no locking in this build (Zephyr's port has no threading layer): the server thread's PSA calls (key
 * exchange, every packet) and the key table of ssh_host_key.c must not interleave with ours. The server runs at
 * CONFIG_SSH_SERVER_THREAD_PRIORITY and PSA never blocks, so while this thread runs the server is blocked outside any PSA call; at
 * PSA_PRIO, above the server, it cannot be preempted by it either. Only for the calls into the key table: keep
 * them short (an import is milliseconds), file IO and the host key's generation stay at WORK_PRIO (the server is
 * not running yet when the host key is made).
 */
static int psa_enter(void)
{
	int old = k_thread_priority_get(k_current_get());

	k_thread_priority_set(k_current_get(), PSA_PRIO);
	return old;
}

static void psa_leave(int old)
{
	k_thread_priority_set(k_current_get(), old);
}

/* A user's public key into a slot of the server's table: the raw key data (ecdsa-sha2-nistp256, ssh-ed25519) */
static int key_import(int slot, const struct ak_key *k)
{
	const uint8_t *raw;
	int old, ret, n;

	n = ak_raw_pubkey(k, &raw);
	if (n <= 0) {
		return -EINVAL;
	}
	old = psa_enter();
	ret = ssh_keygen_import(slot, false, SSH_HOST_KEY_FORMAT_RAW, raw, (size_t)n);
	psa_leave(old);
	return ret;
}

#ifdef CONFIG_SSH_USER_KEY_ED25519
/* The server asks the application to check an ssh-ed25519 login (zephyr/net/ssh/keygen.h) */
int ssh_ed25519_verify(const uint8_t pub_key[32], const uint8_t *msg, size_t msg_len,
		       const uint8_t signature[64])
{
	return multiapp_ed25519_verify(pub_key, msg, msg_len, signature) ? 0 : -1;
}
#endif

/* ---- the host key ---- */

static int hk_load_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg, void *param)
{
	int *n = param;

	ARG_UNUSED(key);
	*n = (int)len;
	if (len <= sizeof(der) && read_cb(cb_arg, der, sizeof(der)) != (ssize_t)len) {
		*n = -EIO;
	}
	return 0;
}

static void hk_fingerprint(void)
{
	struct ak_key k;
	uint8_t point[AK_ECDSA_POINT_LEN];
	int n = ssh_keygen_export(HOST_SLOT, false, SSH_HOST_KEY_FORMAT_RAW, point, sizeof(point));

	host_fp[0] = '\0';
	if (n == AK_ECDSA_POINT_LEN && ak_from_ecdsa_point(point, (size_t)n, &k) == 0) {
		ak_fingerprint(k.blob, k.blob_len, host_fp);
	}
}

/* Load the stored host key, or make one and save it. false: no host key (no server). */
static bool hk_setup(void)
{
	int stored = 0, ret;

	ret = settings_subsys_init();
	if (ret != 0) {
		LOG_ERR("settings: %d (the host key cannot be kept)", ret);
	} else {
		(void)settings_load_subtree_direct("ssh", hk_load_cb, &stored);
	}
	if (stored != ECDSA_SCALAR_LEN) {
		/* whatever the callback copied (a stored length up to the buffer's) is not a key to keep around */
		mbedtls_platform_zeroize(der, sizeof(der));
	}
	if (stored == ECDSA_SCALAR_LEN) {
		ret = ssh_keygen_import(HOST_SLOT, true, SSH_HOST_KEY_FORMAT_RAW, der, (size_t)stored);
		/* the key is in the server's table now; this copy of the private scalar goes */
		mbedtls_platform_zeroize(der, sizeof(der));
		if (ret == 0) {
			hk_state = HK_LOADED;
			hk_fingerprint();
			LOG_INF("host key loaded: ecdsa-sha2-nistp256 %s", host_fp);
			return true;
		}
		LOG_ERR("the stored host key is unusable (%d): making a new one", ret);
	} else if (stored != 0) {
		/* An RSA key of an earlier build, or damage: the key is replaced and its fingerprint changes */
		LOG_WRN("the stored host key is not an ECDSA P-256 key (%d bytes): making a new one", stored);
	}
	hk_state = HK_GENERATING;
	LOG_INF("making the SSH host key (ECDSA P-256, hardware TRNG)");
	ret = ssh_keygen(HOST_SLOT, SSH_HOST_KEY_TYPE_ECDSA_P256, HOSTKEY_BITS);
	mbedtls_platform_zeroize(der, sizeof(der));
	if (ret != 0) {
		LOG_ERR("host key generation failed: %d", ret);
		hk_state = HK_NONE;
		return false;
	}
	hk_fingerprint();
	hk_state = HK_NEW_UNSAVED;
	LOG_INF("new host key: ecdsa-sha2-nistp256 %s", host_fp);
	printk("ssh: new host key ecdsa-sha2-nistp256 %s\n", host_fp);
	return true;
}

/* The new key to the settings, when no stream runs (else later: the work loop calls this again). */
static void hk_save(void)
{
	int n, ret;

	if (hk_state != HK_NEW_UNSAVED || multiapp_net_stream_busy()) {
		/* nothing to save, or tried again by the work loop: the private key is not even copied out now */
		return;
	}
	{
		int old = psa_enter();

		n = ssh_keygen_export(HOST_SLOT, true, SSH_HOST_KEY_FORMAT_RAW, der, sizeof(der));
		psa_leave(old);
	}
	if (n <= 0) {
		LOG_ERR("host key export: %d", n);
		mbedtls_platform_zeroize(der, sizeof(der));
		return;
	}
	if (multiapp_net_stream_busy()) {
		/* tried again by the work loop every few seconds */
		mbedtls_platform_zeroize(der, sizeof(der));
		return;
	}
	ret = settings_save_one(HOSTKEY_NAME, der, (size_t)n);
	mbedtls_platform_zeroize(der, sizeof(der));
	if (ret != 0) {
		LOG_ERR("saving the host key: %d (it is lost at the next reset)", ret);
		return;
	}
	hk_state = HK_NEW_SAVED;
	LOG_INF("host key saved");
}

/* ---- the users' keys ---- */

static void slots_clear(void)
{
	int old = psa_enter();

	for (int i = FIRST_KEY_SLOT; i < CONFIG_SSH_MAX_HOST_KEYS; i++) {
		(void)ssh_keygen_free(i);
	}
	psa_leave(old);
	slots_used = 0;
	entry_count = 0;
	lines_bad = 0;
}

static void add_line(const uint8_t *line, size_t len)
{
	struct ak_key k;
	int r = ak_parse_line((const char *)line, len, &k);
	struct entry *e;

	if (r == AK_SKIP) {
		return;
	}
	if (r < 0) {
		lines_bad++;
		LOG_WRN("authorized_keys: %s", ak_strerror(r));
		return;
	}
	if (entry_count >= LIST_MAX) {
		lines_bad++;
		return;
	}
	e = &entries[entry_count];
	memset(e, 0, sizeof(*e));
	strncpy(e->type, k.type, sizeof(e->type) - 1);
	strncpy(e->comment, k.comment, sizeof(e->comment) - 1);
	ak_fingerprint(k.blob, k.blob_len, e->fp);
	e->bits = k.bits;
	e->slot = -1;
	if (!k.supported) {
		e->note = k.why;
	} else {
		for (int i = 0; i < entry_count; i++) {
			if (strcmp(entries[i].fp, e->fp) == 0) {
				e->note = "same key as an earlier line";
				break;
			}
		}
		if (e->note == NULL && slots_used >= KEY_SLOTS) {
			e->note = "no free key slot (CONFIG_SSH_MAX_HOST_KEYS)";
		}
		if (e->note == NULL) {
			int slot = FIRST_KEY_SLOT + slots_used;

			if (key_import(slot, &k) == 0) {
				e->slot = slot;
				slots_used++;
			} else {
				e->note = "the server refused the key";
			}
		}
	}
	entry_count++;
}

static bool dir_has(const char *dir, const char *name);

/* The key file is there: asking by open would log an error when it is not */
static bool keys_file_there(void)
{
	return dir_has(NETPATH_VOLUME, "ssh") && dir_has(KEYS_DIR, "authorized_keys");
}

/*
 * The lines of the open file, one call of @p cb each (the line without its newline, @p toolong: it was cut at
 * LINE_MAX_LEN). A negative return of @p cb stops it. 0, or the read error.
 */
typedef int (*line_cb)(const uint8_t *line, size_t len, bool toolong, void *arg);

static int file_lines(struct fs_file_t *f, line_cb cb, void *arg)
{
	uint8_t chunk[128];
	size_t fill = 0;
	bool cut = false;
	ssize_t n;
	int ret;

	while ((n = fs_read(f, chunk, sizeof(chunk))) > 0) {
		for (ssize_t i = 0; i < n; i++) {
			uint8_t c = chunk[i];

			if (c == '\n') {
				ret = cb(line_buf, fill, cut, arg);
				if (ret < 0) {
					return ret;
				}
				fill = 0;
				cut = false;
			} else if (fill < sizeof(line_buf)) {
				line_buf[fill++] = c;
			} else {
				cut = true;
			}
		}
	}
	if (n < 0) {
		return (int)n;
	}
	return fill > 0 ? cb(line_buf, fill, cut, arg) : 0;
}

static int load_line(const uint8_t *line, size_t len, bool toolong, void *arg)
{
	ARG_UNUSED(arg);
	if (toolong) {
		lines_bad++;
		LOG_WRN("authorized_keys: a line is longer than %d", LINE_MAX_LEN);
		return 0;
	}
	add_line(line, len);
	return 0;
}

/* The file, under a claim of the volume taken by the caller. 0, -ENOENT (no file: no keys) or -errno. */
static int keys_read_locked(void)
{
	struct fs_file_t f;
	int ret;

	slots_clear();
	if (!keys_file_there()) {
		return -ENOENT;
	}
	fs_file_t_init(&f);
	ret = fs_open(&f, KEYS_FILE, FS_O_READ);
	if (ret != 0) {
		return ret;
	}
	ret = file_lines(&f, load_line, NULL);
	(void)fs_close(&f);
	return ret;
}

static int keys_report(int ret)
{
	if (ret == 0 || ret == -ENOENT) {
		keys_loaded = true;
		LOG_INF("authorized keys: %d loaded, %d listed, %d bad lines%s", slots_used, entry_count, lines_bad,
			ret == -ENOENT ? " (no " KEYS_FILE ")" : "");
		return 0;
	}
	LOG_ERR("authorized keys: %d", ret);
	return ret;
}

/* Reload: a read claim of the volume, refused while a stream runs. */
static int keys_reload(void)
{
	int ret;

	if (multiapp_net_stream_busy()) {
		return -EBUSY;
	}
	ret = multiapp_drive_fs_begin(false, false, CLAIM_MS);
	if (ret != 0) {
		return ret == -EAGAIN ? -EBUSY : ret;
	}
	ret = keys_read_locked();
	multiapp_drive_fs_end(false, false);
	return keys_report(ret);
}

static int file_last_byte_is_newline(struct fs_file_t *f, bool *nl, bool *empty)
{
	struct fs_dirent st;
	uint8_t c = '\n';
	int ret = fs_stat(KEYS_FILE, &st);

	*empty = false;
	*nl = true;
	if (ret == -ENOENT) {
		*empty = true;
		return 0;
	}
	if (ret != 0) {
		return ret;
	}
	if (st.size == 0) {
		*empty = true;
		return 0;
	}
	ret = fs_seek(f, -1, FS_SEEK_END);
	if (ret == 0 && fs_read(f, &c, 1) == 1) {
		*nl = c == '\n';
	}
	return fs_seek(f, 0, FS_SEEK_END);
}

/*
 * The file system logs an error for a mkdir of a directory, an unlink of a file and even an fs_stat of a path that
 * is not there (so that an error line means an error): look in the directory instead.
 */
static bool dir_has(const char *dir, const char *name)
{
	struct fs_dir_t d;
	struct fs_dirent e;
	bool found = false;

	fs_dir_t_init(&d);
	if (fs_opendir(&d, dir) != 0) {
		return false;
	}
	while (fs_readdir(&d, &e) == 0 && e.name[0] != '\0') {
		if (strcasecmp(e.name, name) == 0) {
			found = true;
			break;
		}
	}
	(void)fs_closedir(&d);
	return found;
}

/* Append a line (already parsed and found good), then read the file again. */
static int keys_add(const char *line, size_t len, char *msg, size_t msg_len)
{
	struct fs_file_t f;
	bool nl, empty;
	int ret, written;

	if (multiapp_net_stream_busy()) {
		return -EBUSY;
	}
	ret = multiapp_drive_fs_begin(true, true, CLAIM_MS);
	if (ret != 0) {
		return ret == -EAGAIN ? -EBUSY : ret;
	}
	ret = dir_has(NETPATH_VOLUME, "ssh") ? 0 : fs_mkdir(KEYS_DIR);
	if (ret != 0) {
		snprintf(msg, msg_len, "mkdir " KEYS_DIR ": %d", ret);
		goto out;
	}
	fs_file_t_init(&f);
	ret = fs_open(&f, KEYS_FILE, FS_O_CREATE | FS_O_RDWR);
	if (ret != 0) {
		snprintf(msg, msg_len, "open " KEYS_FILE ": %d", ret);
		goto out;
	}
	ret = file_last_byte_is_newline(&f, &nl, &empty);
	if (ret == 0) {
		if (!nl) {
			ret = fs_write(&f, "\n", 1) == 1 ? 0 : -EIO;
		}
		if (ret == 0) {
			written = fs_write(&f, line, len);
			ret = written == (int)len ? 0 : (written < 0 ? written : -EIO);
		}
		if (ret == 0) {
			ret = fs_write(&f, "\n", 1) == 1 ? 0 : -EIO;
		}
	}
	(void)fs_close(&f);
	if (ret != 0) {
		snprintf(msg, msg_len, "write " KEYS_FILE ": %d", ret);
		goto out;
	}
	ret = keys_read_locked();
out:
	multiapp_drive_fs_end(true, true);
	return ret == 0 ? keys_report(0) : ret;
}

/* ---- removing keys: the file is copied without the lines that match ---- */

#define KEYS_NEW KEYS_DIR "/authorized_keys.new"

struct drop {
	struct fs_file_t *out;
	const char *match;   /* "SHA256:..." or a comment */
	int dropped;
	int err;
};

static int copy_line(const uint8_t *line, size_t len, bool toolong, void *arg)
{
	struct drop *d = arg;
	struct ak_key k;
	int r;

	if (toolong) {
		return -EFBIG;
	}
	r = ak_parse_line((const char *)line, len, &k);
	if (r == 0) {
		char fp[AK_FP_LEN];

		ak_fingerprint(k.blob, k.blob_len, fp);
		if (strcmp(d->match, fp) == 0 || strcmp(d->match, k.comment) == 0) {
			d->dropped++;
			return 0;
		}
	}
	if (fs_write(d->out, line, len) != (ssize_t)len || fs_write(d->out, "\n", 1) != 1) {
		return -EIO;
	}
	return 0;
}

static int keys_remove(const char *match, char *msg, size_t msg_len)
{
	struct fs_file_t in, out;
	struct drop d = {.out = &out, .match = match};
	int ret;

	if (multiapp_net_stream_busy()) {
		return -EBUSY;
	}
	ret = multiapp_drive_fs_begin(true, true, CLAIM_MS);
	if (ret != 0) {
		return ret == -EAGAIN ? -EBUSY : ret;
	}
	fs_file_t_init(&in);
	fs_file_t_init(&out);
	ret = keys_file_there() ? fs_open(&in, KEYS_FILE, FS_O_READ) : -ENOENT;
	if (ret != 0) {
		snprintf(msg, msg_len, "no key file");
		ret = -ENOENT;
		goto out;
	}
	if (dir_has(KEYS_DIR, "authorized_keys.new")) {
		(void)fs_unlink(KEYS_NEW);
	}
	ret = fs_open(&out, KEYS_NEW, FS_O_CREATE | FS_O_WRITE);
	if (ret != 0) {
		(void)fs_close(&in);
		snprintf(msg, msg_len, "open " KEYS_NEW ": %d", ret);
		goto out;
	}
	ret = file_lines(&in, copy_line, &d);
	(void)fs_close(&in);
	(void)fs_close(&out);
	if (ret == -EFBIG) {
		snprintf(msg, msg_len, "a line of the file is longer than %d: edit it on the PC", LINE_MAX_LEN);
	} else if (ret != 0) {
		snprintf(msg, msg_len, "copying the file: %d", ret);
	} else if (d.dropped == 0) {
		snprintf(msg, msg_len, "no key matches (a SHA256: fingerprint or a comment)");
		ret = -ENOENT;
	} else {
		/* The copy replaces the file: FAT has no atomic rename over a file, so the old one goes first */
		ret = fs_unlink(KEYS_FILE);
		if (ret == 0) {
			ret = fs_rename(KEYS_NEW, KEYS_FILE);
		}
		if (ret != 0) {
			snprintf(msg, msg_len, "replacing the file: %d (the copy is " KEYS_NEW ")", ret);
		}
	}
	if (ret != 0) {
		(void)fs_unlink(KEYS_NEW);   /* made above */
	} else {
		snprintf(msg, msg_len, "%d key%s removed", d.dropped, d.dropped == 1 ? "" : "s");
		ret = keys_read_locked();
		ret = ret == 0 || ret == -ENOENT ? keys_report(0) : ret;
	}
out:
	multiapp_drive_fs_end(true, true);
	return ret;
}

/* ---- requests of the shell commands ---- */

enum req_op { REQ_NONE, REQ_RELOAD, REQ_ADD, REQ_REMOVE };

static struct {
	enum req_op op;
	char line[LINE_MAX_LEN];
	int result;
	char msg[96];
} req;
static K_MUTEX_DEFINE(req_lock);
/* A request was handed to the work thread and it has not ended it: the submitter may have given up waiting */
static atomic_t req_busy;
static K_SEM_DEFINE(req_go, 0, 1);
static K_SEM_DEFINE(req_done, 0, 1);

static bool key_known(const struct ak_key *k)
{
	char fp[AK_FP_LEN];

	ak_fingerprint(k->blob, k->blob_len, fp);
	for (int i = 0; i < entry_count; i++) {
		if (strcmp(entries[i].fp, fp) == 0) {
			return true;
		}
	}
	return false;
}

static void do_request(void)
{
	struct ak_key k;
	int r;

	req.msg[0] = '\0';
	switch (req.op) {
	case REQ_RELOAD:
		req.result = keys_reload();
		break;
	case REQ_ADD:
		r = ak_parse_line(req.line, strlen(req.line), &k);
		if (r == AK_SKIP) {
			snprintf(req.msg, sizeof(req.msg), "nothing to add");
			req.result = -EINVAL;
		} else if (r < 0) {
			snprintf(req.msg, sizeof(req.msg), "%s", ak_strerror(r));
			req.result = -EINVAL;
		} else if (!k.supported) {
			snprintf(req.msg, sizeof(req.msg), "%s", k.why);
			req.result = -ENOTSUP;
		} else if (key_known(&k)) {
			snprintf(req.msg, sizeof(req.msg), "the key is in the file already");
			req.result = -EEXIST;
		} else if (slots_used >= KEY_SLOTS) {
			snprintf(req.msg, sizeof(req.msg), "all %d key slots are in use", KEY_SLOTS);
			req.result = -ENOSPC;
		} else {
			req.result = keys_add(req.line, strlen(req.line), req.msg, sizeof(req.msg));
		}
		break;
	case REQ_REMOVE:
		req.result = keys_remove(req.line, req.msg, sizeof(req.msg));
		break;
	default:
		req.result = -EINVAL;
		break;
	}
	k_sem_give(&req_done);
	atomic_set(&req_busy, 0);
}

/*
 * From a shell thread: hand the request to the work thread and wait for it. @p msg (96 bytes) gets the answer's
 * text. A request that outlasts the wait stays the work thread's until it ends (req_busy): no new one is taken
 * meanwhile (-EBUSY), so that it never reads a request or an answer that is not its own.
 */
static int submit(enum req_op op, const char *line, char msg[96])
{
	int ret;

	msg[0] = '\0';
	if (!hk_ready) {
		return -EAGAIN;
	}
	/* A command on the work thread must not wait for a submitter that waits for the work thread */
	if (k_mutex_lock(&req_lock, k_current_get() == &work_thread ? K_NO_WAIT : K_SECONDS(25)) != 0) {
		return -EBUSY;
	}
	if (atomic_get(&req_busy) != 0) {
		k_mutex_unlock(&req_lock);
		return -EBUSY;
	}
	atomic_set(&req_busy, 1);
	req.op = op;
	if (line != NULL) {
		strncpy(req.line, line, sizeof(req.line) - 1);
		req.line[sizeof(req.line) - 1] = '\0';
	}
	k_sem_reset(&req_done);
	if (k_current_get() == &work_thread) {
		/* A command run by `ssh <host> <command>` runs on the work thread: no one else to wait for */
		do_request();
	} else {
		k_sem_give(&req_go);
	}
	if (k_sem_take(&req_done, K_SECONDS(20)) == 0) {
		ret = req.result;
		memcpy(msg, req.msg, sizeof(req.msg));
	} else {
		ret = -ETIMEDOUT;
	}
	k_mutex_unlock(&req_lock);
	return ret;
}

/* ---- a job for the work thread: the command of an exec request (src/console_mux.c) ---- */

static struct {
	void (*fn)(void *arg);
	void *arg;
} job;
static atomic_t job_busy;
static K_SEM_DEFINE(job_go, 0, 1);

int sshd_run_job(void (*fn)(void *arg), void *arg)
{
	if (!atomic_cas(&job_busy, 0, 1)) {
		return -EBUSY;
	}
	job.fn = fn;
	job.arg = arg;
	k_sem_give(&job_go);
	return 0;
}

/* ---- the thread ---- */

static char banner[64];

static void server_up(void)
{
	static int slot_ids[KEY_SLOTS];

	static const struct net_sockaddr_in6 any = {
		.sin6_family = NET_AF_INET6,
		.sin6_port = net_htons(CONFIG_SSH_PORT),
	};
	/* What the user sees before the login: this build, not "Welcome to Zephyr OS" and a hint for a terminal */
	{
		char ver[40];

		gitver_text(ver, sizeof(ver));
		snprintf(banner, sizeof(banner), "esp-sdr multiapp %s\n", ver);
		ssh_server_set_banner(banner);
	}
	/* publickey only: no password; any user name (the key is the identity) */
	for (int i = 0; i < KEY_SLOTS; i++) {
		slot_ids[i] = FIRST_KEY_SLOT + i;
	}
	int ret = ssh_server_start(ssh_server_instance(0), (const struct net_sockaddr *)&any, HOST_SLOT, NULL,
				   NULL, slot_ids, KEY_SLOTS, cmux_server_event,
				   cmux_transport_event, NULL);

	if (ret != 0) {
		LOG_ERR("ssh_server_start: %d", ret);
		return;
	}
	hk_ready = true;
	server_running = true;
	k_work_reschedule(&keepalive_work, K_SECONDS(1));
	LOG_INF("SSH server on port %d (public key login only)", CONFIG_SSH_PORT);
}

/* The server thread ended (a fatal error of accept()): the work loop starts it again */
void sshd_server_closed(void)
{
	server_running = false;
	LOG_WRN("the SSH server thread ended: it is started again");
}

static void work_fn(void *a, void *b, void *c)
{
	int tries;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	if (!hk_setup()) {
		return;
	}
	/*
	 * The key file first, then the server: one that is up but has no keys yet answers a login that comes early
	 * with a refusal. Not while a stream runs, and the PC may be using the drive: a few tries, then the server
	 * starts anyway and the loop below goes on trying.
	 */
	for (tries = 0; tries < 5 && !keys_loaded; tries++) {
		int ret = keys_reload();

		if (ret == 0 || ret == -ENOENT) {
			break;
		}
		k_sleep(K_SECONDS(2));
	}
	server_up();
	hk_save();
	{
		size_t unused;

		if (k_thread_stack_space_get(&work_thread, &unused) == 0) {
			LOG_INF("work thread: %u of %u stack bytes unused", (unsigned int)unused,
				(unsigned int)K_THREAD_STACK_SIZEOF(sshd_work_stack));
		}
	}
	for (;;) {
		struct k_poll_event ev[2];
		bool wait = hk_state == HK_NEW_UNSAVED || !keys_loaded || !server_running;

		k_poll_event_init(&ev[0], K_POLL_TYPE_SEM_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY, &req_go);
		k_poll_event_init(&ev[1], K_POLL_TYPE_SEM_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY, &job_go);
		/* The host key may still wait for the end of a stream, the key file too: look again every few seconds */
		(void)k_poll(ev, ARRAY_SIZE(ev), wait ? K_SECONDS(SAVE_RETRY_S) : K_FOREVER);
		if (k_sem_take(&req_go, K_NO_WAIT) == 0) {
			do_request();
		}
		if (k_sem_take(&job_go, K_NO_WAIT) == 0) {
			job.fn(job.arg);
			atomic_set(&job_busy, 0);
		}
		hk_save();
		if (!keys_loaded) {
			(void)keys_reload();
		}
		if (!server_running) {
			server_up();
		}

	}
}

static int sshd_init(void)
{
	k_thread_create(&work_thread, sshd_work_stack, K_THREAD_STACK_SIZEOF(sshd_work_stack), work_fn, NULL, NULL, NULL,
			WORK_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&work_thread, "sshd_work");
	return 0;
}
SYS_INIT(sshd_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* ---- shell ---- */

static const char *why(int err)
{
	switch (err) {
	case -EAGAIN:
		return "the SSH server is not ready yet (the host key is being made)";
	case -EBUSY:
		return "refused: a stream runs, or the PC is writing to the drive";
	case -ETIMEDOUT:
		return "timed out";
	default:
		return NULL;
	}
}

static int cmd_hostkey(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	switch (hk_state) {
	case HK_GENERATING:
		shell_print(sh, "the host key is being made");
		return 0;
	case HK_NONE:
		shell_error(sh, "no host key: generation failed");
		return -EIO;
	default:
		break;
	}
	shell_print(sh, "ecdsa-sha2-nistp256 %d %s", HOSTKEY_BITS, host_fp);
	shell_print(sh, "%s", hk_state == HK_LOADED ? "stored in the settings"
			      : hk_state == HK_NEW_SAVED ? "made at this boot, saved in the settings"
							  : "made at this boot, NOT saved yet (a stream runs): "
							    "lost at a reset");
	return 0;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "server: %s, port %d, public key login only", server_running ? "running" : "NOT RUNNING",
		    CONFIG_SSH_PORT);
	shell_print(sh, "sessions: %d, the Zephyr shell is %s", cmux_sessions(),
		    cmux_shell_busy() ? "taken" : "free");
	if (cmux_server_stack_unused() != 0) {
		shell_print(sh, "server thread stack: %u of %u bytes never used (after the last session)",
			    (unsigned int)cmux_server_stack_unused(), CONFIG_SSH_SERVER_STACK_SIZE);
	}
	{
		int32_t slot_ms, shell_ms;

		cmux_timing(&slot_ms, &shell_ms);
		if (slot_ms >= 0) {
			shell_print(sh, "after the last session: connection slot free %d ms after the channel closed, shell free "
					"%d ms after its stop", (int)slot_ms, (int)shell_ms);
		}
	}
	{
		size_t unused;

		if (k_thread_stack_space_get(&work_thread, &unused) == 0) {
			shell_print(sh, "sshd_work stack: %u of %u bytes never used (the peak of its commands so far)",
				    (unsigned int)unused, (unsigned int)K_THREAD_STACK_SIZEOF(sshd_work_stack));
		}
	}
	shell_print(sh, "keys: %d loaded of %d slots, %d listed%s", slots_used, KEY_SLOTS, entry_count,
		    keys_loaded ? "" : " (the file was not read yet)");
	shell_print(sh, "key file changes: %s", multiapp_net_stream_busy() ? "refused (a stream runs)" : "allowed");
	return 0;
}

static int cmd_key_list(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (entry_count == 0) {
		shell_print(sh, "no keys%s: ssh key add <line>, or put " KEYS_FILE " on the drive and ssh keys reload",
			    keys_loaded ? "" : " read yet");
		return 0;
	}
	for (int i = 0; i < entry_count; i++) {
		const struct entry *e = &entries[i];

		shell_print(sh, "%d %s %u %s %s%s%s", i, e->type, e->bits, e->fp, e->comment,
			    e->slot >= 0 ? "" : "  NOT USED: ", e->slot >= 0 ? "" : e->note);
	}
	shell_print(sh, "%d bad lines at the last read of the file%s", lines_bad,
		    multiapp_net_stream_busy() ? " (a stream runs: the file is not read now, this is the table in RAM; "
						 "`ssh keys reload` reads it again when the stream has ended)" : "");
	return 0;
}

static int report(const struct shell *sh, int ret, const char *ok, const char *msg)
{
	const char *w = why(ret);

	if (ret == 0) {
		shell_print(sh, "%s", ok);
		return 0;
	}
	if (w != NULL) {
		shell_error(sh, "%s", w);
	} else if (msg[0] != '\0') {
		shell_error(sh, "%s", msg);
	} else {
		shell_error(sh, "failed: %d", ret);
	}
	return ret;
}

static int cmd_keys_reload(const struct shell *sh, size_t argc, char **argv)
{
	char msg[96];
	int ret = submit(REQ_RELOAD, NULL, msg);

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (ret == 0) {
		shell_print(sh, "%d keys loaded, %d listed, %d bad lines", slots_used, entry_count, lines_bad);
		return 0;
	}
	return report(sh, ret, "", msg);
}

static int cmd_key_add(const struct shell *sh, size_t argc, char **argv)
{
	char line[LINE_MAX_LEN];
	char msg[96];
	size_t n = 0;
	int ret;

	/* The line as one quoted argument, or unquoted: the words joined */
	for (size_t i = 1; i < argc; i++) {
		int w = snprintf(line + n, sizeof(line) - n, "%s%s", i > 1 ? " " : "", argv[i]);

		if (w < 0 || (size_t)w >= sizeof(line) - n) {
			shell_error(sh, "the line is longer than %d", LINE_MAX_LEN - 1);
			return -EINVAL;
		}
		n += (size_t)w;
	}
	ret = submit(REQ_ADD, line, msg);
	return report(sh, ret, "key added", msg);
}

static int cmd_key_remove(const struct shell *sh, size_t argc, char **argv)
{
	char msg[96];
	int ret;

	ARG_UNUSED(argc);
	ret = submit(REQ_REMOVE, argv[1], msg);
	return report(sh, ret, msg, msg);
}

SHELL_STATIC_SUBCMD_SET_CREATE(ssh_key_cmds,
	SHELL_CMD_ARG(add, NULL, "<authorized_keys line, quoted or not>: append to " KEYS_FILE " and load",
		      cmd_key_add, 2, 16),
	SHELL_CMD(list, NULL, "The keys of the file: fingerprint, comment, used or not", cmd_key_list),
	SHELL_CMD_ARG(remove, NULL, "<SHA256:fingerprint or comment>: drop the matching lines of " KEYS_FILE,
		      cmd_key_remove, 2, 0),
	SHELL_SUBCMD_SET_END);
SHELL_STATIC_SUBCMD_SET_CREATE(ssh_keys_cmds,
	SHELL_CMD(reload, NULL, "Read " KEYS_FILE " again", cmd_keys_reload),
	SHELL_SUBCMD_SET_END);
SHELL_STATIC_SUBCMD_SET_CREATE(ssh_cmds,
	SHELL_CMD(hostkey, NULL, "The host key's fingerprint (SHA-256, as OpenSSH shows it)", cmd_hostkey),
	SHELL_CMD(key, &ssh_key_cmds, "Users' public keys: add, list, remove", NULL),
	SHELL_CMD(keys, &ssh_keys_cmds, "reload", NULL),
	SHELL_CMD(status, NULL, "The server, its sessions and keys", cmd_status),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(ssh, &ssh_cmds, "SSH server (public key login)", NULL);
