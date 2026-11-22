/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * MicroPython on the ESP32-S31 host: its thread, and the REPL's stream (doc/dev/network-services.md,
 * "The MicroPython REPL over SSH").
 *
 * The REPL reads and writes the local console (the CDC-ACM port the shell gave up, or UART0 in the UART
 * build: the port's src/zephyr_getchar.c and uart_core.c) until an SSH session asks for a shell. The
 * host's console mux (src/console_mux.c) then hands the session to this file's console, and the
 * port's stdio hook (modules/micropython ports/zephyr/stdio_hook.h) moves the REPL onto the channel.
 * When the session ends, the REPL is on the local console again. `ssh <host> shell` is never this
 * console: it is the Zephyr shell.
 *
 * Threads: the SSH server's thread calls open(), rx_ready(), tx_ready() and closed() (closed() under the
 * mux's registry lock: it waits for nobody but the REPL's thread inside one non-blocking channel call);
 * MicroPython's thread calls the hook. The channel is reached only under io_lock, so a session that went
 * is never written to (the mux reuses its slot for the next client).
 */

#include <errno.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/sys_heap.h>
#include <zephyr/sys/atomic.h>

#include "py/mphal.h"
#include "py/runtime.h"
#include "py/stackctrl.h"
#include "ports/zephyr/stdio_hook.h"
#include "shared/runtime/interrupt_char.h"

#include <multiapp/console.h>

#include "repl_core.h"

LOG_MODULE_REGISTER(mp_repl, LOG_LEVEL_INF);

int real_main(void);

/* The port's real_main() calls this at every soft reset: the host's main() has started USB. */
int mp_usbd_init(void)
{
	return 0;
}
void mp_hal_signal_event(void);

static K_MUTEX_DEFINE(io_lock);
static K_SEM_DEFINE(tx_sem, 0, 1);
static const struct multiapp_console_io *sess_io; /* under io_lock; NULL: no session */
static atomic_t sess_on;                          /* a session is the REPL's stream (hook active) */
static struct repl_ring ring;
static atomic_t pulled; /* the channel gave bytes since input_empty() last looked */

/* ---- the port's stdio hook, on MicroPython's thread ---- */

/* The channel's input into the ring; under io_lock. */
static void pull_locked(void)
{
	if (sess_io != NULL) {
		size_t got = 0;
		unsigned int ints = repl_feed(&ring, sess_io->read, sess_io->ctx, mp_interrupt_char, &got);

		if (got != 0U) {
			atomic_set(&pulled, 1);
		}
		if (ints != 0U) {
			mp_sched_keyboard_interrupt();
			mp_hal_signal_event();
		}
	}
}

static int hook_rx(void)
{
	int c;

	if (!atomic_get(&sess_on)) {
		return MP_HAL_STDIO_HOOK_INACTIVE;
	}
	c = repl_ring_get(&ring);
	if (c < 0) {
		/* Whatever the server's notification has not brought yet */
		k_mutex_lock(&io_lock, K_FOREVER);
		pull_locked();
		k_mutex_unlock(&io_lock);
		c = repl_ring_get(&ring);
	}
	return atomic_get(&sess_on) ? c : MP_HAL_STDIO_HOOK_INACTIVE;
}

static int hook_poll(void)
{
	if (!atomic_get(&sess_on)) {
		return MP_HAL_STDIO_HOOK_INACTIVE;
	}
	if (repl_ring_count(&ring) == 0U) {
		k_mutex_lock(&io_lock, K_FOREVER);
		pull_locked();
		k_mutex_unlock(&io_lock);
	}
	return repl_ring_count(&ring) != 0U;
}

static mp_uint_t hook_tx(const char *str, mp_uint_t len)
{
	mp_uint_t done = 0;

	while (done < len && atomic_get(&sess_on)) {
		const struct multiapp_console_io *io;
		int n = -ECONNRESET;

		k_mutex_lock(&io_lock, K_FOREVER);
		io = sess_io;
		if (io != NULL) {
			n = io->write(io->ctx, str + done, len - done);
		}
		k_mutex_unlock(&io_lock);
		if (io == NULL || n == -ECONNRESET) {
			break;
		}
		if (n > 0) {
			done += (mp_uint_t)n;
		} else if (n == 0 || n == -EAGAIN || n == -ENOBUFS || n == -EWOULDBLOCK) {
			/* The channel is full: tx_ready() comes; the timeout covers a missed one */
			(void)k_sem_take(&tx_sem, K_MSEC(20));
		} else {
			break;
		}
	}
	return done;
}

static const struct mp_hal_stdio_hook repl_hook = {
	.rx = hook_rx,
	.poll = hook_poll,
	.tx = hook_tx,
};

/* ---- the mux's console ---- */

static int con_open(const struct multiapp_console_io *io, void **session)
{
	int ret = 0;

	k_mutex_lock(&io_lock, K_FOREVER);
	if (sess_io != NULL) {
		/* A REPL has one session: the next client gets the Zephyr shell */
		ret = -EBUSY;
	} else {
		sess_io = io;
		repl_ring_reset(&ring);
		/* An empty line: the REPL answers with a prompt, which is what the client waits for */
		(void)repl_ring_put(&ring, '\r');
		k_sem_reset(&tx_sem);
		atomic_set(&sess_on, 1);
		*session = &ring;
	}
	k_mutex_unlock(&io_lock);
	if (ret == 0) {
		mp_hal_signal_event();
	}
	return ret;
}

static void con_rx_ready(void *session)
{
	ARG_UNUSED(session);
	k_mutex_lock(&io_lock, K_FOREVER);
	pull_locked();
	k_mutex_unlock(&io_lock);
	mp_hal_signal_event();
}

static void con_tx_ready(void *session)
{
	ARG_UNUSED(session);
	k_sem_give(&tx_sem);
}

static void con_closed(void *session)
{
	ARG_UNUSED(session);
	atomic_set(&sess_on, 0);
	k_mutex_lock(&io_lock, K_FOREVER);
	sess_io = NULL;
	k_mutex_unlock(&io_lock);
	/* A writer waiting for room leaves; the REPL goes on at the local console */
	k_sem_give(&tx_sem);
	mp_hal_signal_event();
}

/* All the input of a session that ended is read (the ring is empty and the last pull found nothing): see console.h */
static bool con_input_empty(void *session)
{
	ARG_UNUSED(session);
	return repl_ring_count(&ring) == 0U && atomic_set(&pulled, 0) == 0;
}

static const struct multiapp_console repl_console = {
	.name = "micropython",
	.open = con_open,
	.rx_ready = con_rx_ready,
	.tx_ready = con_tx_ready,
	.closed = con_closed,
	.input_empty = con_input_empty,
};

/* ---- MicroPython's thread ---- */

/* The port's main() ends here only when the REPL is left for good (machine.reset() does not return). */
static void mp_entry(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	mp_hal_stdio_hook = &repl_hook;
	if (multiapp_console_register(&repl_console) != 0) {
		LOG_WRN("the REPL is not on SSH: another console is registered");
	}
	(void)real_main();
	(void)multiapp_console_unregister(&repl_console);
	mp_hal_stdio_hook = NULL;
}

/*
 * The thread's stack is in PSRAM (the extension's bss, CONFIG_MULTIAPP_MICROPYTHON_STACK): Python needs depth, internal RAM
 * has none to spare. A thread whose stack is in PSRAM must never touch the flash itself: the flash driver turns the
 * cache off and its own stack goes with it. So everything MicroPython calls that can reach the flash runs on a worker with an
 * internal stack (mp_flash_run(): esdr.shell()'s commands), and the host refuses a flash operation from any thread
 * whose stack is in PSRAM (src/flashguard.c: an error and a log line instead of a hang).
 */
static struct k_thread mp_thread;

/*
 * Below the SSH server (10), the net threads and the USB device, and above the shell (14): the REPL
 * must not hold up a stream or the network, and an app's own threads (esdr's workers) are lower still.
 */
#define MP_THREAD_PRIO 13

/* The port's stack check starts at the top of the thread it runs in. */
void multiapp_mp_cstack_init(void)
{
	const struct k_thread *self = k_current_get();

	mp_cstack_init_with_top((void *)(self->stack_info.start + self->stack_info.size),
				self->stack_info.size);
}

static uint8_t mp_stack_mem[CONFIG_MULTIAPP_MICROPYTHON_STACK] __aligned(16);

/* ---- the flash worker: an internal stack, taken from the libc arena when first needed ---- */

int malloc_runtime_stats_get(struct sys_memory_stats *stats);

static size_t arena_free(void)
{
	struct sys_memory_stats st = {0};

	return malloc_runtime_stats_get(&st) == 0 ? st.free_bytes : 0U;
}

/* Priority of the worker: not above MicroPython's own thread */
#define FW_PRIO MP_THREAD_PRIO

static struct k_work_q fw_q;
static k_thread_stack_t *fw_stack;
static size_t fw_size;

struct fw_job {
	struct k_work work;
	int (*fn)(void *arg);
	void *arg;
	int ret;
	struct k_sem done;
};

static void fw_handler(struct k_work *work)
{
	struct fw_job *j = CONTAINER_OF(work, struct fw_job, work);

	j->ret = j->fn(j->arg);
	k_sem_give(&j->done);
}

/* Run fn(arg) on the worker and wait for it (MicroPython's thread may not touch the flash itself). */
static int mp_flash_run(int (*fn)(void *arg), void *arg)
{
	struct fw_job j = {.fn = fn, .arg = arg};
	static K_MUTEX_DEFINE(fw_lock);

	k_mutex_lock(&fw_lock, K_FOREVER);
	if (fw_stack == NULL) {
		const struct k_work_queue_config cfg = {.name = "mp_flash"};

		fw_size = CONFIG_MULTIAPP_MICROPYTHON_WORKER_STACK;
		fw_stack = aligned_alloc(16, fw_size);
		if (fw_stack == NULL) {
			k_mutex_unlock(&fw_lock);
			LOG_ERR("no %zu bytes of internal RAM for the flash worker (arena free %zu)", fw_size,
				arena_free());
			return -ENOMEM;
		}
		k_work_queue_init(&fw_q);
		k_work_queue_start(&fw_q, fw_stack, fw_size, FW_PRIO, &cfg);
	}
	k_sem_init(&j.done, 0, 1);
	k_work_init(&j.work, fw_handler);
	(void)k_work_submit_to_queue(&fw_q, &j.work);
	k_sem_take(&j.done, K_FOREVER);
	/* The worker's stack comes from the arena with no guard: say once when a job has used 90 % of it (the sizing rule) */
	{
		static bool warned;
		size_t unused;

		if (!warned && k_thread_stack_space_get(k_work_queue_thread_get(&fw_q), &unused) == 0 &&
		    (fw_size - unused) * 10U > fw_size * 9U) {
			warned = true;
			LOG_WRN("the flash worker's stack peaked at %zu of %zu bytes (over 90 %%): raise "
				"CONFIG_MULTIAPP_MICROPYTHON_WORKER_STACK", fw_size - unused, fw_size);
		}
	}
	k_mutex_unlock(&fw_lock);
	return j.ret;
}

/* What the port's callers in the extension (esdr's calibration requests, esdr_local.c) run on the worker */
int multiapp_mp_flash_call(int (*fn)(void *arg), void *arg)
{
	return mp_flash_run(fn, arg);
}

/* What esdr.shell() collects: the host's output of the command, CR LF as LF, up to the buffer */
struct shell_job {
	const char *cmd;
	char *buf;
	size_t cap, len;
};

static void shell_sink(const void *data, size_t len, void *ctx)
{
	struct shell_job *sj = ctx;
	const char *p = data;

	for (size_t i = 0; i < len && sj->len < sj->cap; i++) {
		if (p[i] != '\r') {
			sj->buf[sj->len++] = p[i];
		}
	}
}

static int shell_job_fn(void *arg)
{
	struct shell_job *sj = arg;

	return multiapp_shell_exec(sj->cmd, shell_sink, sj);
}

/*
 * esdr.shell(): a host command, which may read the volume or write settings, on the worker, in the host's own
 * shell instance (the same as `ssh <host> '<command>'` uses). Like every other writer of the volume it does not run
 * while a stream runs (src/netsvc.c, the same rule as SSH's key file).
 */
#if defined(CONFIG_MCUMGR)
bool multiapp_net_stream_busy(void);
#endif

int multiapp_mp_shell_exec(const char *cmd, char *buf, size_t cap, size_t *len)
{
	struct shell_job sj = {.cmd = cmd, .buf = buf, .cap = cap};
	int ret;

#if defined(CONFIG_MCUMGR)
	if (multiapp_net_stream_busy()) {
		return -EBUSY;
	}
#endif
	ret = mp_flash_run(shell_job_fn, &sj);
	*len = sj.len;
	return ret;
}

/*
 * What the port calls that reaches the flash and is not routed: refused with an exception (ext link --wrap): Python's
 * thread has its stack in PSRAM, and the volume is the host's (the PC, SSH and MCUmgr share it by claims). The VFS
 * is not mounted here either (_boot.py).
 */
#include <zephyr/storage/flash_map.h>
#include <zephyr/storage/disk_access.h>

static void flash_refused(void)
{
	mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("the host owns the flash and the volume (use esdr.shell())"));
}

int __wrap_flash_area_read(const struct flash_area *fa, off_t off, void *dst, size_t len)
{
	ARG_UNUSED(fa);
	ARG_UNUSED(off);
	ARG_UNUSED(dst);
	ARG_UNUSED(len);
	flash_refused();
	return -EACCES;
}

int __wrap_flash_area_write(const struct flash_area *fa, off_t off, const void *src, size_t len)
{
	ARG_UNUSED(fa);
	ARG_UNUSED(off);
	ARG_UNUSED(src);
	ARG_UNUSED(len);
	flash_refused();
	return -EACCES;
}

int __wrap_flash_area_erase(const struct flash_area *fa, off_t off, size_t len)
{
	ARG_UNUSED(fa);
	ARG_UNUSED(off);
	ARG_UNUSED(len);
	flash_refused();
	return -EACCES;
}

int __wrap_disk_access_read(const char *pdrv, uint8_t *data_buf, uint32_t start_sector, uint32_t num_sector)
{
	ARG_UNUSED(pdrv);
	ARG_UNUSED(data_buf);
	ARG_UNUSED(start_sector);
	ARG_UNUSED(num_sector);
	flash_refused();
	return -EACCES;
}

int __wrap_disk_access_write(const char *pdrv, const uint8_t *data_buf, uint32_t start_sector, uint32_t num_sector)
{
	ARG_UNUSED(pdrv);
	ARG_UNUSED(data_buf);
	ARG_UNUSED(start_sector);
	ARG_UNUSED(num_sector);
	flash_refused();
	return -EACCES;
}

/* machine.reset(): the reboot may stop the cache and write the flash's state, so it runs on the worker too */
void __real_sys_reboot(int type);

static int reboot_job(void *arg)
{
	__real_sys_reboot((int)(intptr_t)arg);
	return 0;
}

void __wrap_sys_reboot(int type)
{
	if (mp_flash_run(reboot_job, (void *)(intptr_t)type) != 0) {
		/* No worker (no memory for its stack): reboot from here, as the port always did */
		__real_sys_reboot(type);
	}
	for (;;) {
		k_sleep(K_FOREVER);
	}
}

/*
 * esdr.stacks(): the sizes and peaks (CONFIG_INIT_STACKS) the stack sizes are set from, and the libc arena's state: bytes
 * free and allocated now (the worker's stack is in the allocated, from its first use), to measure what the rest of the
 * host takes of it (CONFIG_MULTIAPP_MICROPYTHON_ARENA_USE).
 */
void multiapp_mp_stacks(size_t v[6])
{
	size_t unused = 0;
	struct sys_memory_stats st = {0};

	v[0] = sizeof(mp_stack_mem);
	if (k_thread_stack_space_get(&mp_thread, &unused) == 0) {
		v[1] = sizeof(mp_stack_mem) - unused;
	}
	v[2] = fw_size;
	if (fw_stack != NULL && k_thread_stack_space_get(k_work_queue_thread_get(&fw_q), &unused) == 0) {
		v[3] = fw_size - unused;
	}
	if (malloc_runtime_stats_get(&st) == 0) {
		v[4] = st.free_bytes;
		v[5] = st.allocated_bytes;
	}
}

int main(void)
{
	LOG_INF("MicroPython stack: %zu bytes in PSRAM, thread priority %d; libc arena free %zu B", sizeof(mp_stack_mem),
		MP_THREAD_PRIO, arena_free());
	k_thread_create(&mp_thread, (k_thread_stack_t *)mp_stack_mem, sizeof(mp_stack_mem), mp_entry, NULL, NULL, NULL,
			MP_THREAD_PRIO, K_FP_REGS, K_FOREVER);
	k_thread_name_set(&mp_thread, "micropython");
	k_thread_start(&mp_thread);
	return 0;
}
