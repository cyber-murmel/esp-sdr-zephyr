/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The console of an SSH session (doc/dev/network-services.md, phase 3). The server reaches this
 * through its channel callback:
 *  - a "shell" request (`ssh <host>`) gets the console that is registered (multiapp_console_register(),
 *    the MicroPython REPL of phase 4) or else the Zephyr shell;
 *  - an "exec" request of the command "shell" (`ssh <host> shell`) always gets the interactive Zephyr
 *    shell;
 *  - any other exec request (`ssh <host> 'multiapp list'`) runs the command in the Zephyr shell, sends its
 *    output and ends the channel with the command's exit status;
 *  - a subsystem request (sftp, phase 5) is refused.
 *
 * Zephyr's own SSH shell backend (SHELL_BACKEND_SSH) is not built: it takes every channel for itself and
 * refuses exec requests. This file is its transport (a second shell instance, here) and the channel logic
 * around it. One Zephyr shell instance: one shell or exec session at a time, the others get a line that
 * says so. The shell thread's stack is in the LP RAM (multiapp_lpram_s31.ld): the commands it runs touch
 * the flash. An exec command runs on the sshd_work thread (src/sshd.c: it cannot run on the server's
 * thread, which has to move its output).
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/ssh/common.h>
#include <zephyr/net/ssh/server.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>

#include <multiapp/console.h>

#include "console_mux.h"
#include "console_mux_core.h"

LOG_MODULE_REGISTER(sshmux, LOG_LEVEL_INF);

/*
 * The shell and exec paths reach the session from several threads (the server's, the shell's, sshd_work, the work
 * queue) with plain flags and no atomics around them, and the PSA exclusion of sshd.c is by priority: that holds on
 * one CPU only. This host is single core; an SMP build needs these locks and flags thought through first.
 */
BUILD_ASSERT(!IS_ENABLED(CONFIG_SMP), "the SSH host code is not SMP safe yet (doc/dev/network-services.md)");

#define MAX_SESSIONS (CONFIG_SSH_SERVER_MAX_CLIENTS * CONFIG_SSH_MAX_CHANNELS)
#define BUSY_TEXT    "The shell is in use by another SSH session.\r\n"
#define EXEC_MAX     256
#define EXEC_MAX_TEXT 255
#define EXEC_IDLE_MS  5000

struct session {
	bool used;
	bool shell;                           /* it owns the Zephyr shell */
	bool closing;                         /* the channel is gone */
	bool exec;                            /* the Zephyr shell runs one command for it */
	bool exec_queued;
	const char *exec_err;                 /* an exec request that cannot run: said on stderr, status 1 */
	struct ssh_transport *transport;      /* its connection */
	bool transport_closed;                /* the connection went; a channel that never opened has no CLOSED event */
	uint32_t gen;                         /* which use of this slot: a job of an earlier session must not touch the next */
	bool started;                         /* a shell or exec request was served */
	struct ssh_channel *ch;
	const struct multiapp_console *repl;  /* else NULL */
	void *repl_handle;
	struct multiapp_console_io io;
};

static struct session sessions[MAX_SESSIONS];
static uint32_t session_gen;
static K_MUTEX_DEFINE(mux_lock);

/*
 * Every call into a channel from a thread other than the server's goes through here. The server thread closes a
 * channel and, for the next client, resets the transport and its channels (ssh_transport_start() zeroes them) while
 * a writer of lower priority may be in the middle of a call: the writer then used a channel that was gone, or the
 * next client's. Under ch_lock a session is either open or closing, and a closing one is not touched.
 */
static K_MUTEX_DEFINE(ch_lock);
/* The session that has the Zephyr shell: where tp_write() and tp_read() go (under ch_lock) */
static struct session *shell_sess;

static int sess_write(struct session *s, const void *data, size_t len)
{
	int ret = -ECONNRESET;

	k_mutex_lock(&ch_lock, K_FOREVER);
	if (s != NULL && s->used && !s->closing) {
		ret = ssh_channel_write(s->ch, data, len);
	}
	k_mutex_unlock(&ch_lock);
	return ret;
}

static int sess_write_stderr_g(struct session *s, const char *text, uint32_t gen)
{
	int ret = -ECONNRESET;

	k_mutex_lock(&ch_lock, K_FOREVER);
	if (s != NULL && s->used && !s->closing && (gen == 0U || s->gen == gen)) {
		ret = ssh_channel_write_stderr(s->ch, text, strlen(text));
	}
	k_mutex_unlock(&ch_lock);
	return ret;
}

static int sess_read(struct session *s, void *data, size_t len)
{
	int ret = -ECONNRESET;

	k_mutex_lock(&ch_lock, K_FOREVER);
	if (s != NULL && s->used && !s->closing) {
		ret = ssh_channel_read(s->ch, data, len);
	}
	k_mutex_unlock(&ch_lock);
	return ret;
}

static int sess_exit_g(struct session *s, uint32_t status, uint32_t gen)
{
	int ret = -ECONNRESET;

	k_mutex_lock(&ch_lock, K_FOREVER);
	if (s != NULL && s->used && !s->closing && (gen == 0U || s->gen == gen)) {
		ret = ssh_channel_exit(s->ch, status);
	}
	k_mutex_unlock(&ch_lock);
	return ret;
}

static int sess_write_stderr(struct session *s, const char *text)
{
	return sess_write_stderr_g(s, text, 0U);
}

static int sess_exit(struct session *s, uint32_t status)
{
	return sess_exit_g(s, status, 0U);
}

/* The Zephyr shell's side: the session is looked up under the lock, so that a session that went is never used */
static int shell_chan_write(const void *data, size_t len)
{
	int ret = -ECONNRESET;

	k_mutex_lock(&ch_lock, K_FOREVER);
	if (shell_sess != NULL && shell_sess->used && !shell_sess->closing) {
		ret = ssh_channel_write(shell_sess->ch, data, len);
	}
	k_mutex_unlock(&ch_lock);
	return ret;
}

static int shell_chan_read(void *data, size_t len)
{
	int ret = -ECONNRESET;

	k_mutex_lock(&ch_lock, K_FOREVER);
	if (shell_sess != NULL && shell_sess->used && !shell_sess->closing) {
		ret = ssh_channel_read(shell_sess->ch, data, len);
	}
	k_mutex_unlock(&ch_lock);
	return ret;
}

static int sess_close(struct session *s)
{
	int ret = -ECONNRESET;

	k_mutex_lock(&ch_lock, K_FOREVER);
	if (s != NULL && s->used && !s->closing) {
		ret = ssh_channel_close(s->ch);
	}
	k_mutex_unlock(&ch_lock);
	return ret;
}

void cmux_lock(void)
{
	k_mutex_lock(&mux_lock, K_FOREVER);
}

void cmux_unlock(void)
{
	k_mutex_unlock(&mux_lock);
}

/* ---- the Zephyr shell over a channel ---- */

struct shell_tp {
	shell_transport_handler_t handler;
	void *context;
};

static struct shell_tp shell_tp;

/* The Zephyr shell serves an exec request: it reads no input, the command is run by exec_run() */
static bool shell_exec;
/* Bytes the shell wrote to the channel of this session: a command that ends without any is told to the client */
static uint32_t shell_out_bytes;
/*
 * The end of the client's input in an interactive session (ssh <host> shell < file): the shell is let finish what it
 * has been sent. eof_check() looks every 100 ms; the channel ends with status 0 once the shell has read all its input
 * and been quiet for 400 ms, or after EOF_MAX_MS whatever it does (a stuck command must not keep the connection).
 */
#define EOF_MAX_MS   5000
#define EOF_QUIET_MS 400
static int64_t last_write_ms, eof_t0;
static bool eof_pending, eof_drained;
static struct session *eof_session;
static void eof_check(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(eof_work, eof_check);

static int tp_init(const struct shell_transport *t, const void *config, shell_transport_handler_t handler,
		   void *context)
{
	ARG_UNUSED(t);
	shell_tp.handler = handler;
	shell_tp.context = context;
	ARG_UNUSED(config);
	return 0;
}

static int tp_uninit(const struct shell_transport *t)
{
	ARG_UNUSED(t);
	return 0;
}

static int tp_enable(const struct shell_transport *t, bool blocking)
{
	ARG_UNUSED(t);
	ARG_UNUSED(blocking);
	return 0;
}

/* defined by SHELL_DEFINE() below */
static const struct shell ssh_sh;
static struct k_thread ssh_sh_thread;

static int tp_write(const struct shell_transport *t, const void *data, size_t length, size_t *cnt)
{
	int ret;

	ARG_UNUSED(t);
	ret = shell_chan_write(data, length);
	if (ret == -ECONNRESET) {
		/* The session is gone: the output goes nowhere, and the shell must not wait for room in a channel */
		*cnt = length;
		return 0;
	}
	if (ret < 0) {
		return ret;
	}
	if (k_current_get() != &ssh_sh_thread) {
		/* what the caller of shell_execute_cmd() wrote: the shell thread's own start-up lines do not count */
		shell_out_bytes += (uint32_t)ret;
	}
	last_write_ms = k_uptime_get();
	*cnt = (size_t)ret;
	return 0;
}

static int tp_read(const struct shell_transport *t, void *data, size_t length, size_t *cnt)
{
	int ret;

	ARG_UNUSED(t);
	if (shell_sess == NULL || shell_exec) {
		*cnt = 0;
		return 0;
	}
	if (ssh_sh.ctx->state != SHELL_STATE_ACTIVE) {
		/*
		 * shell_start() empties the transport's input before it is active (z_shell_backend_rx_buffer_flush()):
		 * what a client sent right after its request (a piped command) would be thrown away. It stays in the
		 * channel's ring, and the pending RX event makes the shell read it as soon as it is active.
		 */
		*cnt = 0;
		return 0;
	}
	ret = shell_chan_read(data, length);
	if (ret == -ECONNRESET) {
		*cnt = 0;
		return 0;
	}
	if (ret < 0) {
		return ret;
	}
	if (ret == 0 && eof_pending) {
		/* the shell asked for more after the client's input ended: all of it has been read */
		eof_drained = true;
	}
	*cnt = (size_t)ret;
	return 0;
}

static const struct shell_transport_api tp_api = {
	.init = tp_init,
	.uninit = tp_uninit,
	.enable = tp_enable,
	.write = tp_write,
	.read = tp_read,
};
static const struct shell_transport shell_transport_ssh = {.api = &tp_api, .ctx = &shell_tp};

/* The log queue is of no use: the session's shell takes no log (shell_init(..., false, ...)). */
SHELL_DEFINE(ssh_sh, "esdr:~$ ", &shell_transport_ssh, 64, 0, SHELL_FLAG_OLF_CRLF);

static bool shell_taken;
/* The shell thread's life: ssh_sh_thread is one object, made again by every shell_init() */
static bool shell_thread_made;
static bool shell_stopping;
static K_SEM_DEFINE(sh_gone_sem, 0, 1);
/* How long things take after a session: for `ssh status`, to tell whether a new client has to wait */
static int64_t t_chan_closed, t_sh_stop;
static int32_t slot_busy_ms = -1, shell_gone_ms = -1;
/* Last peak of the server thread's stack (bytes never used), taken when a session ends */
static size_t server_stack_unused;

static int ssh_sh_start(struct session *s, bool exec)
{
	static const struct shell_backend_config_flags flags = SHELL_DEFAULT_BACKEND_CONFIG_FLAGS;
	/* A command's output is for a program: no echo, no colours, no cursor movements */
	static const struct shell_backend_config_flags exec_flags = {
		.insert_mode = 0, .echo = 0, .obscure = 0, .mode_delete = 1, .use_colors = 0, .use_vt100 = 0,
	};
	int ret;

	if (shell_stopping && k_sem_take(&sh_gone_sem, K_SECONDS(3)) != 0) {
		return -EBUSY;
	}
	if (shell_taken) {
		return -EBUSY;
	}
	/*
	 * The uninit callback runs on the shell thread before the thread aborts itself (shell.c, kill_handler()):
	 * the server thread, which has the higher priority, got in between and made the thread again while it was
	 * still alive (a crash in z_swap, a load fault, with its saved context overwritten). Wait until it is dead.
	 */
	if (shell_thread_made && k_thread_join(&ssh_sh_thread, K_SECONDS(2)) != 0) {
		return -EBUSY;
	}
	shell_exec = exec;
	shell_sess = s;
	ret = shell_init(&ssh_sh, s->ch, exec ? exec_flags : flags, false, LOG_LEVEL_NONE);
	if (ret != 0) {
		LOG_ERR("shell_init: %d", ret);
		shell_sess = NULL;
		return ret;
	}
	if (exec) {
		/* The prompt the shell prints as it starts is no output of the command */
		(void)shell_prompt_change(&ssh_sh, "");
	}
	shell_thread_made = true;
	shell_taken = true;
	s->shell = true;
	return 0;
}

/* The shell thread has torn the instance down: the next session may have it */
static void ssh_sh_gone(const struct shell *sh, int res)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(res);
	shell_exec = false;
	shell_sess = NULL;
	shell_taken = false;
	shell_stopping = false;
	shell_gone_ms = (int32_t)(k_uptime_get() - t_sh_stop);
	k_sem_give(&sh_gone_sem);
}

/* A shell writer may wait for room in the channel (shell_pend_on_txdone(), for ever): its session is gone now */
static void wake_shell_writers(void)
{
	if (shell_tp.handler != NULL) {
		shell_tp.handler(SHELL_TRANSPORT_EVT_TX_RDY, shell_tp.context);
	}
}

static void ssh_sh_kill(void)
{
	wake_shell_writers();
	shell_stopping = true;
	t_sh_stop = k_uptime_get();
	k_sem_reset(&sh_gone_sem);
	shell_uninit(&ssh_sh, ssh_sh_gone);
}

static void ssh_sh_stop(struct session *s)
{
	if (s->shell) {
		s->shell = false;
		ssh_sh_kill();
	}
}

/* ---- a registered console's view of the channel ---- */

static int io_read(void *ctx, void *data, size_t len)
{
	struct session *s = ctx;

	return sess_read(s, data, len);
}

static int io_write(void *ctx, const void *data, size_t len)
{
	struct session *s = ctx;

	return sess_write(s, data, len);
}

static void io_close(void *ctx)
{
	struct session *s = ctx;

	(void)sess_close(s);
}

/* A console left (unregistered, under the lock): its sessions end. */
void cmux_removed(const struct multiapp_console *c)
{
	for (size_t i = 0; i < ARRAY_SIZE(sessions); i++) {
		struct session *s = &sessions[i];

		if (s->used && s->repl == c) {
			void *handle = s->repl_handle;

			s->repl = NULL;
			c->closed(handle);
			(void)sess_close(s);
		}
	}
}

int cmux_sessions(void)
{
	int n = 0;

	for (size_t i = 0; i < ARRAY_SIZE(sessions); i++) {
		n += sessions[i].used ? 1 : 0;
	}
	return n;
}

bool cmux_shell_busy(void)
{
	return shell_taken;
}

/* ---- the server's callbacks ---- */

static void say(struct ssh_channel *ch, const char *text)
{
	(void)ssh_channel_write(ch, text, strlen(text));
}

/* A shell or exec request: the console of the session. 0 or -errno (the request fails). */
static int start_console(struct session *s, enum cmux_request req)
{
	const struct multiapp_console *c;
	int ret = -ENOENT;

	cmux_lock();
	c = cmux_pick(req);
	if (c != NULL) {
		s->io = (struct multiapp_console_io){
			.read = io_read, .write = io_write, .close = io_close, .ctx = s,
		};
		ret = c->open(&s->io, &s->repl_handle);
		if (ret == 0) {
			s->repl = c;
			LOG_INF("session on console %s", c->name);
		} else {
			LOG_WRN("console %s declined the session (%d): Zephyr shell", c->name, ret);
		}
	}
	cmux_unlock();
	if (ret == 0) {
		return 0;
	}
	ret = ssh_sh_start(s, false);
	if (ret == -EBUSY) {
		say(s->ch, BUSY_TEXT);
	}
	return ret;
}

/* ---- an exec request: a command of the Zephyr shell ---- */

static char exec_cmd[EXEC_MAX];
static atomic_t exec_running;
/* The session the queued command belongs to (its gen): a slot used again is another session */
static uint32_t exec_gen;
/* From the queueing of an exec job to the end of exec_run(): exec_cmd and the flags below are its */
static atomic_t exec_pending;
static bool exec_cancel;   /* the session ended before or while the command ran */
static bool exec_orphan;   /* ... and its shell could not be taken down yet */
static K_SEM_DEFINE(exec_done, 0, 1);

/*
 * Wait until the session shell is started and idle. shell_execute_cmd() takes the shell's lock for itself and gives
 * up with -ENOEXEC, printing nothing, when it does not get it within CONFIG_SHELL_TX_TIMEOUT_MS (500 ms). The shell
 * thread holds the lock while it starts (shell_start() prints its first lines) and runs at the lowest priority, 14:
 * the server thread (10) takes the CPU for the key exchange of the next queued client (about 0.5 s each), and the
 * thread can wait that long with the lock in its hand, so the command found the lock taken. Board: one command of 240
 * in a run of fast sessions ended with 8 and a bare "\r\n" (the shell thread's own output). The lock is free again once
 * the thread is done with its start, and nothing takes it after that in an exec session (no input is read).
 */
static bool exec_shell_idle(void)
{
	int64_t t0 = k_uptime_get();
	char st[48];

	while (!exec_cancel && k_uptime_get() - t0 < EXEC_IDLE_MS) {
		if (ssh_sh.ctx->state == SHELL_STATE_ACTIVE && k_sem_count_get(&ssh_sh.ctx->lock_sem) > 0) {
			if (k_uptime_get() - t0 >= 100) {
				LOG_INF("exec: the shell was idle after %d ms", (int)(k_uptime_get() - t0));
			}
			return true;
		}
		k_sleep(K_MSEC(10));
	}
	(void)k_thread_state_str(&ssh_sh_thread, st, sizeof(st));
	LOG_WRN("exec: the shell is not idle after %d ms: state %d, lock %u, thread %s", EXEC_IDLE_MS,
		(int)ssh_sh.ctx->state, k_sem_count_get(&ssh_sh.ctx->lock_sem), st);
	return false;
}

/*
 * On the sshd_work thread: run the command, then end the channel with its status once the output is sent. A command
 * that still ends with -ENOEXEC without a byte of output from this thread (the lock was lost to the shell thread
 * after all) is run again a few times.
 */
static void exec_run(void *arg)
{
	struct session *s = arg;
	int rc = -ECANCELED;
	int tries = 0;

	atomic_set(&exec_running, 1);
	LOG_INF("exec: \"%s\"", exec_cmd);
	(void)exec_shell_idle();
	while (!exec_cancel) {
		shell_out_bytes = 0;
		rc = shell_execute_cmd(&ssh_sh, exec_cmd);
		if (rc != -ENOEXEC || shell_out_bytes != 0 || ++tries > 20) {
			break;
		}
		LOG_WRN("exec: no lock for the shell, again (%d): state %d, lock %u", tries, (int)ssh_sh.ctx->state,
			k_sem_count_get(&ssh_sh.ctx->lock_sem));
		k_sleep(K_MSEC(50));
	}
	if (!exec_cancel && s->used && !s->closing && s->gen == exec_gen) {
		/* the shell's errors are negative errnos */
		uint32_t status = rc == 0 ? 0U : (uint32_t)MIN(rc < 0 ? -rc : rc, 255);

		LOG_INF("exec: \"%s\" ended with %d after %d tries", exec_cmd, rc, tries + 1);
		if (rc != 0 && shell_out_bytes == 0) {
			/* a failure always says something */
			static const char none[] = "ssh: the command failed without a message\n";

			(void)sess_write_stderr_g(s, none, exec_gen);
		}
		(void)sess_exit_g(s, status, exec_gen);
	}
	atomic_set(&exec_running, 0);
	if (exec_orphan) {
		/* The session went while the command ran and gave up waiting for it */
		exec_orphan = false;
		ssh_sh_kill();
	}
	atomic_set(&exec_pending, 0);
	k_sem_give(&exec_done);
}

/* The command of an exec request, trimmed; "shell" asks for the interactive Zephyr shell. */
static int start_exec(struct session *s, const uint8_t *data, uint32_t len)
{
	int ret;

	while (len > 0 && (data[0] == ' ' || data[0] == '\t')) {
		data++;
		len--;
	}
	while (len > 0 && (data[len - 1] == ' ' || data[len - 1] == '\t' || data[len - 1] == '\n')) {
		len--;
	}
	if (len == 0 || (len == 5 && memcmp(data, "shell", 5) == 0)) {
		/* never a registered console: this is how to reach the Zephyr shell when a REPL runs */
		return start_console(s, CMUX_EXEC);
	}
	if (atomic_get(&exec_pending) != 0) {
		/* the last command is still ending (exec_cmd is its): answered, not run */
		s->exec_err = "ssh: the previous command is still ending\n";
		s->exec = true;
		return 0;
	}
	if (len >= sizeof(exec_cmd)) {
		/* Accepted and answered on stderr with a status: a refusal would only show as a dropped connection */
		s->exec_err = "ssh: the command is longer than " STRINGIFY(EXEC_MAX_TEXT) " characters\n";
		s->exec = true;
		return 0;
	}
	ret = ssh_sh_start(s, true);
	if (ret != 0) {
		s->exec_err = ret == -EBUSY ? "ssh: the shell is in use by another session\n"
					    : "ssh: the shell could not be started\n";
		s->exec = true;
		return 0;
	}
	memcpy(exec_cmd, data, len);
	exec_cmd[len] = '\0';
	exec_cancel = false;
	exec_gen = s->gen;
	k_sem_reset(&exec_done);
	s->exec = true;
	return 0;
}

static void eof_check(struct k_work *work)
{
	struct session *s = eof_session;
	int64_t now = k_uptime_get();
	int64_t quiet = now - MAX(eof_t0, last_write_ms);

	ARG_UNUSED(work);
	if (s == NULL || !s->used || s->closing || !eof_pending) {
		return;
	}
	if ((eof_drained && quiet >= EOF_QUIET_MS) || now - eof_t0 >= EOF_MAX_MS) {
		eof_pending = false;
		LOG_INF("session: input ended, the shell is done (%d ms)", (int)(now - eof_t0));
		/* sends what the shell wrote, then the status, EOF and close */
		(void)sess_exit(s, 0);
		return;
	}
	if (!eof_drained && shell_tp.handler != NULL) {
		/* wake the shell to read what is left (it stops at the end of the input) */
		shell_tp.handler(SHELL_TRANSPORT_EVT_RX_RDY, shell_tp.context);
	}
	k_work_reschedule(&eof_work, K_MSEC(100));
}

static int channel_event(struct ssh_channel *ch, const struct ssh_channel_event *event, void *user_data)
{
	struct session *s = user_data;

	switch (event->type) {
	case SSH_CHANNEL_EVENT_REQUEST: {
		bool ok = false;
		bool refuse = false;

		switch (event->channel_request.type) {
		case SSH_CHANNEL_REQUEST_SHELL:
			if (!s->started) {
				ok = start_console(s, CMUX_SHELL) == 0;
				s->started = ok;
			}
			refuse = !ok;
			break;
		case SSH_CHANNEL_REQUEST_EXEC:
			if (!s->started) {
				ok = start_exec(s, event->channel_request.data, event->channel_request.data_len) == 0;
				s->started = ok;
			}
			refuse = !ok;
			break;
		case SSH_CHANNEL_REQUEST_SUBSYSTEM:
			/* sftp is phase 5 */
			LOG_WRN("subsystem \"%.*s\" refused", (int)MIN(event->channel_request.data_len, 32U),
				(const char *)event->channel_request.data);
			refuse = true;
			break;
		case SSH_CHANNEL_REQUEST_PSEUDO_TERMINAL:
		case SSH_CHANNEL_REQUEST_ENV_VAR:
		case SSH_CHANNEL_REQUEST_WINDOW_CHANGE:
			/* No terminal to set up: the client's does the editing */
			ok = true;
			break;
		default:
			/* x11, signals */
			break;
		}
		if (event->channel_request.want_reply) {
			(void)ssh_channel_request_result(ch, ok);
		}
		if (refuse) {
			(void)ssh_channel_close(ch);
		} else if (ok && s->exec_err != NULL) {
			(void)sess_write_stderr(s, s->exec_err);
			(void)sess_exit(s, 1);
		} else if (ok && s->exec && !s->exec_queued) {
			/* after the reply: its output must not overtake it */
			s->exec_queued = true;
			atomic_set(&exec_pending, 1);
			if (sshd_run_job(exec_run, s) != 0) {
				atomic_set(&exec_pending, 0);
				say(ch, "The SSH server is busy.\r\n");
				s->exec_queued = false;
				(void)sess_exit(s, 1);
			}
		}
		return 0;
	}
	case SSH_CHANNEL_EVENT_RX_DATA_READY:
		if (s->shell && !s->exec && shell_tp.handler != NULL) {
			shell_tp.handler(SHELL_TRANSPORT_EVT_RX_RDY, shell_tp.context);
		} else {
			/* a registered console may be unregistered by another thread: look under the lock */
			cmux_lock();
			if (s->repl != NULL) {
				s->repl->rx_ready(s->repl_handle);
			}
			cmux_unlock();
		}
		return 0;
	case SSH_CHANNEL_EVENT_TX_DATA_READY:
		if (s->shell && shell_tp.handler != NULL) {
			shell_tp.handler(SHELL_TRANSPORT_EVT_TX_RDY, shell_tp.context);
		} else {
			cmux_lock();
			if (s->repl != NULL && s->repl->tx_ready != NULL) {
				s->repl->tx_ready(s->repl_handle);
			}
			cmux_unlock();
		}
		return 0;
	case SSH_CHANNEL_EVENT_RX_STDERR_DATA_READY: {
		uint8_t sink[32];

		/* A client has nothing to say on stderr: drop it so that the window does not fill */
		while (ssh_channel_read_stderr(ch, sink, sizeof(sink)) > 0) {
		}
		return 0;
	}
	case SSH_CHANNEL_EVENT_EOF:
		if (!s->exec && s->shell) {
			/* The client's input ended (echo ... | ssh <host> shell): the shell finishes what it was sent */
			LOG_INF("session: end of input");
			eof_session = s;
			eof_t0 = k_uptime_get();
			eof_pending = true;
			eof_drained = false;
			k_work_reschedule(&eof_work, K_MSEC(100));
		} else if (!s->exec) {
			(void)ssh_channel_close(ch);
		}
		/* An exec command goes on to its end whatever the input does (ssh -n) */
		return 0;
	case SSH_CHANNEL_EVENT_CLOSED: {
		size_t unused;

		LOG_INF("session closed");
		t_chan_closed = k_uptime_get();
		/* This runs on the server thread, which has done the key exchange and the login of this connection */
		if (k_thread_stack_space_get(k_current_get(), &unused) == 0) {
			server_stack_unused = unused;
		}
		k_mutex_lock(&ch_lock, K_FOREVER);
		s->closing = true;
		if (shell_sess == s) {
			/* The slot goes to the next client: what is still written for this one must not follow it */
			shell_sess = NULL;
		}
		k_mutex_unlock(&ch_lock);
		/* A shell writer waiting for room in the channel would wait for ever: it drops its output now */
		wake_shell_writers();
		eof_pending = false;
		(void)k_work_cancel_delayable(&eof_work);
		cmux_lock();
		if (s->repl != NULL) {
			const struct multiapp_console *c = s->repl;

			s->repl = NULL;
			c->closed(s->repl_handle);
		}
		cmux_unlock();
		if (s->exec && s->shell) {
			/* the command may still write: its output goes nowhere from now, and it gets a while to end */
			exec_cancel = true;
			if (s->exec_queued && k_sem_take(&exec_done, K_SECONDS(5)) != 0 &&
			    atomic_get(&exec_running) != 0) {
				LOG_WRN("exec: the command still runs, its shell stays until it ends");
				exec_orphan = true;
				s->shell = false;
			}
		}
		ssh_sh_stop(s);
		s->used = false;
		return 0;
	}
	default:
		return -1;
	}
}

void cmux_timing(int32_t *slot_ms, int32_t *shell_ms)
{
	*slot_ms = slot_busy_ms;
	*shell_ms = shell_gone_ms;
}

size_t cmux_server_stack_unused(void)
{
	return server_stack_unused;
}

static void sessions_sweep(void);

int cmux_server_event(struct ssh_server *sshd, const struct ssh_server_event *event, void *user_data)
{
	ARG_UNUSED(sshd);
	ARG_UNUSED(user_data);
	switch (event->type) {
	case SSH_SERVER_EVENT_CLOSED:
		LOG_INF("server closed");
		sshd_server_closed();
		break;
	case SSH_SERVER_EVENT_CLIENT_CONNECTED:
		sessions_sweep();
		LOG_INF("client connected");
		break;
	case SSH_SERVER_EVENT_CLIENT_DISCONNECTED:
		if (t_chan_closed != 0) {
			/* The slot is free right after this: the time since the session's channel was closed */
			slot_busy_ms = (int32_t)(k_uptime_get() - t_chan_closed);
			t_chan_closed = 0;
		}
		LOG_INF("client disconnected (slot busy %d ms after the channel closed, shell free %d ms after stop)",
			slot_busy_ms, shell_gone_ms);
		break;
	default:
		return -1;
	}
	return 0;
}

/*
 * A session whose connection went before its channel opened (the confirmation could not be sent, an RST after the
 * open request, the open result queued during a key exchange) never gets a CLOSED event: free it here, at the next
 * event of the server. The sessions that did open are freed by their CLOSED event, which follows the transport's
 * at once.
 */
static void sessions_sweep(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(sessions); i++) {
		if (sessions[i].used && sessions[i].transport_closed) {
			LOG_WRN("a session whose channel never opened is freed");
			sessions[i].closing = true;
			sessions[i].used = false;
		}
	}
}

int cmux_transport_event(struct ssh_transport *transport, const struct ssh_transport_event *event,
			 void *user_data)
{
	ARG_UNUSED(user_data);
	if (event->type == SSH_TRANSPORT_EVENT_CLOSED) {
		for (size_t i = 0; i < ARRAY_SIZE(sessions); i++) {
			if (sessions[i].used && sessions[i].transport == transport) {
				sessions[i].transport_closed = true;
			}
		}
		return 0;
	}
	if (event->type == SSH_TRANSPORT_EVENT_CHANNEL_OPEN) {
		struct ssh_channel *ch = event->channel_open.channel;
		struct session *s = NULL;

		sessions_sweep();
		/* The server thread is the only one that opens channels: no race between the free slots */
		for (size_t i = 0; i < ARRAY_SIZE(sessions); i++) {
			if (!sessions[i].used) {
				s = &sessions[i];
				break;
			}
		}
		if (s == NULL) {
			LOG_WRN("no session left");
			return ssh_channel_open_result(ch, false, NULL, NULL);
		}
		memset(s, 0, sizeof(*s));
		s->used = true;
		s->ch = ch;
		s->transport = transport;
		s->gen = ++session_gen;
		if (s->gen == 0U) {
			s->gen = ++session_gen;
		}
		return ssh_channel_open_result(ch, true, channel_event, s);
	}
	return 0;
}
