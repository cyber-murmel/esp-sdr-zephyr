/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * "multiapp stage": the shell side of hot-swapping a stage (doc/apps/multiapp-internals.md). A
 * kernel arrives as base64 lines (put, data, end: tools/kswap.py of apps/multiapp/scripts does
 * it) or as a file on the volume (load); each ends with a line "stage result: ok" or
 * "stage result: error <errno> <why>", which the tool reads.
 *
 * The shell's thread runs all of it, so its stack is internal RAM, which a flash read needs.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/base64.h>
#include <zephyr/sys/crc.h>

#include <esp_sdr/esp_sdr_place.h>

#include <multiapp/multiapp.h>
#include <multiapp/stage.h>

#include "apps.h"

LOG_MODULE_DECLARE(multiapp, LOG_LEVEL_INF);

#define KERNEL_BUF_SIZE KB(CONFIG_MULTIAPP_KERNEL_MAX_KB)

/* The kernel's file as it arrives or is read: a PSRAM buffer (place.h), llext copies from it. */
static ESP_SDR_DATA_F(BULK, kernel_buf, NOINIT) uint8_t kernel_buf[KERNEL_BUF_SIZE] __aligned(4);

/* The upload in progress. */
static struct {
	bool active;
	char stage[24];
	size_t size, off;
	uint32_t crc_want, crc;
	uint32_t timeout_ms;
} up;

/* One command at a time, and the result and view they print live here, not on the stack. */
static K_MUTEX_DEFINE(cmd_lock);
static ESP_SDR_DATA(BULK, res) struct multiapp_stage_result res;
static ESP_SDR_DATA(BULK, view) struct multiapp_stage_view view;

static const char *why(int err)
{
	switch (-err) {
	case ETIMEDOUT:
		return "the app never reached its sync point; the swap is undone and the kernel unloaded";
	case ENOENT:
		return "no such stage or file (multiapp stage list)";
	case ENOEXEC:
		return "not a usable kernel for this stage, this host build or this build of the app "
		       "(the log has why)";
	case EBADMSG:
		return "CRC mismatch";
	case ENODEV:
		return "no app is running";
	case EALREADY:
		return "the stage already runs the app's own function";
	case ENOMEM:
		return "no memory for the kernel (multiapp info shows the heaps)";
	case EFBIG:
		return "the file is larger than CONFIG_MULTIAPP_KERNEL_MAX_KB";
	case EBUSY:
		return "busy: another command, the PC's access to the volume did not end (a copy "
		       "still running), or two threads use the stage";
	case EINVAL:
		return "bad argument";
	default:
		return "failed";
	}
}

static int fail(const struct shell *sh, int err)
{
	shell_error(sh, "stage result: error %d %s", -err, why(err));
	return err;
}

static void print_kernel(const struct shell *sh, const struct multiapp_kernel *k)
{
	shell_print(sh, "  kernel %s (%s, %zu bytes, %s): code %zu at %p, fast code %zu at %p, data %zu, "
		    "bss %zu, rodata %zu, %zu bytes in heaps, runs at %p", k->name, k->source,
		    k->file_size, k->place == MULTIAPP_PLACE_INTERNAL ? "internal RAM" : "PSRAM",
		    k->text_size, k->text, k->fast_size, k->fast, k->data_size, k->bss_size,
		    k->rodata_size, k->alloc_size, k->fn);
}

static void print_swap(const struct shell *sh)
{
	shell_print(sh, "stage %s: %s in %u us (%s)", res.stage,
		    res.reverted ? "back on the app's own function" : "swapped to the kernel",
		    res.latency_us, res.immediate ? "the app was idle" : "at the app's sync point");
	print_kernel(sh, &res.kernel);
	if (res.replaced) {
		shell_print(sh, "stage %s: the previous kernel is %s", res.stage,
			    res.old_ret == 0 ? "unloaded" : "still loaded, unused (the log has why)");
	}
	if (res.reverted) {
		shell_print(sh, "stage %s: the kernel is %s", res.stage,
			    res.old_ret == 0 ? "unloaded" : "still loaded, unused (the log has why)");
	}
	shell_print(sh, "stage result: ok");
}

static uint32_t timeout_arg(size_t argc, char **argv, size_t i)
{
	return argc > i ? (uint32_t)strtoul(argv[i], NULL, 0) : CONFIG_MULTIAPP_STAGE_TIMEOUT_MS;
}

/* Take the command lock, or say the shell is busy: nothing of the upload changes then. */
static bool lock(const struct shell *sh)
{
	if (k_mutex_lock(&cmd_lock, K_NO_WAIT) != 0) {
		fail(sh, -EBUSY);
		return false;
	}
	return true;
}

static int cmd_list(const struct shell *sh, size_t argc, char **argv)
{
	size_t n;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (!lock(sh)) {
		return -EBUSY;
	}
	n = multiapp_stage_count();
	if (n == 0U) {
		shell_print(sh, "no stages: no app is running, or it has none");
	}
	for (size_t i = 0; i < n && multiapp_stage_view(i, &view); i++) {
		const char *origin = view.cur == view.dflt ? "the app's own function"
				    : view.has_kernel && view.cur == view.kernel.fn ? "a kernel"
										      : "unknown";

		shell_print(sh, "stage %s: type \"%s\" (%08x), ABI %08x, runs %s at %p, default %p, "
			    "%s%s%s, %u swaps", view.name, view.sig_text, view.sig, view.abi, origin,
			    view.cur, view.dflt, view.idle ? "idle" : "busy",
			    view.pending ? ", swap pending" : "",
			    view.shared ? ", two threads were inside: swaps refused" : "", view.swaps);
		if (view.has_kernel) {
			print_kernel(sh, &view.kernel);
		}
	}
	if (up.active) {
		shell_print(sh, "upload for %s: %zu of %zu bytes", up.stage, up.off, up.size);
	}
	shell_print(sh, "kernel buffer %zu bytes, swap timeout %d ms", (size_t)KERNEL_BUF_SIZE,
		    CONFIG_MULTIAPP_STAGE_TIMEOUT_MS);
	k_mutex_unlock(&cmd_lock);
	return 0;
}

/* put <stage> <size> <crc32> [timeout_ms]: start an upload; the data follows (data, end). */
static int cmd_put(const struct shell *sh, size_t argc, char **argv)
{
	size_t n;
	size_t size = (size_t)strtoul(argv[2], NULL, 0);
	int ret = 0;

	if (!lock(sh)) {
		return -EBUSY;
	}
	up.active = false;
	n = multiapp_stage_count();
	ret = -ENOENT;
	for (size_t i = 0; i < n && multiapp_stage_view(i, &view); i++) {
		if (strcmp(view.name, argv[1]) == 0) {
			ret = 0;
		}
	}
	if (ret == 0 && (size > KERNEL_BUF_SIZE || size < sizeof(struct multiapp_trailer))) {
		ret = size < sizeof(struct multiapp_trailer) ? -EINVAL : -EFBIG;
	}
	if (ret != 0) {
		k_mutex_unlock(&cmd_lock);
		return fail(sh, ret);
	}
	(void)strncpy(up.stage, argv[1], sizeof(up.stage) - 1);
	up.stage[sizeof(up.stage) - 1] = '\0';
	up.size = size;
	up.off = 0;
	up.crc_want = (uint32_t)strtoul(argv[3], NULL, 0);
	up.crc = 0;
	up.timeout_ms = timeout_arg(argc, argv, 4);
	up.active = true;
	shell_print(sh, "put %s: %zu bytes expected", up.stage, up.size);
	k_mutex_unlock(&cmd_lock);
	return 0;
}

/* data <base64>: the next bytes of the upload; answers "ok <bytes so far>". */
static int cmd_data(const struct shell *sh, size_t argc, char **argv)
{
	size_t olen = 0;
	int ret;

	ARG_UNUSED(argc);
	if (!lock(sh)) {
		return -EBUSY;
	}
	if (!up.active) {
		k_mutex_unlock(&cmd_lock);
		return fail(sh, -EINVAL);
	}
	ret = base64_decode(&kernel_buf[up.off], up.size - up.off, &olen, (const uint8_t *)argv[1],
			    strlen(argv[1]));
	if (ret != 0) {
		/* Too much data, or not base64: the upload is dead, the PC starts again. */
		up.active = false;
		k_mutex_unlock(&cmd_lock);
		return fail(sh, ret == -ENOMEM ? -EFBIG : -EINVAL);
	}
	up.crc = crc32_ieee_update(up.crc, &kernel_buf[up.off], olen);
	up.off += olen;
	shell_print(sh, "ok %zu", up.off);
	k_mutex_unlock(&cmd_lock);
	return 0;
}

static int cmd_abort(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (!lock(sh)) {
		return -EBUSY;
	}
	up.active = false;
	shell_print(sh, "upload dropped");
	k_mutex_unlock(&cmd_lock);
	return 0;
}

/* end: check the upload's size and CRC, load the kernel, swap it in. */
static int cmd_end(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (!lock(sh)) {
		return -EBUSY;
	}
	if (!up.active) {
		k_mutex_unlock(&cmd_lock);
		return fail(sh, -EINVAL);
	}
	up.active = false;
	if (up.off != up.size) {
		shell_error(sh, "stage: %zu of %zu bytes arrived", up.off, up.size);
		k_mutex_unlock(&cmd_lock);
		return fail(sh, -EINVAL);
	}
	if (up.crc != up.crc_want) {
		shell_error(sh, "stage: CRC %08x, announced %08x", up.crc, up.crc_want);
		k_mutex_unlock(&cmd_lock);
		return fail(sh, -EBADMSG);
	}
	ret = multiapp_stage_install(up.stage, kernel_buf, up.size, "upload", up.timeout_ms, &res);
	if (ret != 0) {
		k_mutex_unlock(&cmd_lock);
		return fail(sh, ret);
	}
	print_swap(sh);
	k_mutex_unlock(&cmd_lock);
	return 0;
}

/* load <stage> <file> [timeout_ms]: a kernel file of the volume (".llext" is added to a bare name). */
static int cmd_load(const struct shell *sh, size_t argc, char **argv)
{
	char file[48];
	ssize_t n;
	int ret;

	if (!lock(sh)) {
		return -EBUSY;
	}
	(void)snprintf(file, sizeof(file), "%s%s", argv[2],
		       strchr(argv[2], '.') == NULL ? ".llext" : "");
	up.active = false;
	/*
	 * The PC may still be copying the file: the volume is read afresh each time, and a file that
	 * is half there fails the trailer's size or CRC check, or the FAT reads badly. Once more
	 * after a pause then.
	 */
	for (int attempt = 0;; attempt++) {
		n = multiapp_drive_read_file(file, kernel_buf, KERNEL_BUF_SIZE);
		if (n < 0) {
			ret = (int)n;
		} else {
			ret = multiapp_stage_install(argv[1], kernel_buf, (size_t)n, "file",
						     timeout_arg(argc, argv, 3), &res);
		}
		if (attempt > 0 || !multiapp_drive_on_bus() ||
		    (ret != -EBADMSG && ret != -ENOEXEC && ret != -EIO && ret != -EINVAL)) {
			break;
		}
		k_msleep(DRIVE_RETRY_MS);
	}
	if (n < 0) {
		shell_error(sh, "stage: %s: %d", file, (int)n);
	}
	if (ret != 0) {
		k_mutex_unlock(&cmd_lock);
		return fail(sh, ret);
	}
	print_swap(sh);
	k_mutex_unlock(&cmd_lock);
	return 0;
}

/* revert <stage> [timeout_ms]: the app's own function again, the kernel unloaded. */
static int cmd_revert(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	if (!lock(sh)) {
		return -EBUSY;
	}
	ret = multiapp_stage_revert(argv[1], timeout_arg(argc, argv, 2), &res);
	if (ret != 0) {
		k_mutex_unlock(&cmd_lock);
		return fail(sh, ret);
	}
	print_swap(sh);
	k_mutex_unlock(&cmd_lock);
	return 0;
}

static const struct shell_static_entry stage_subs[] = {
	SHELL_CMD(list, NULL, "The stages of the running app and their kernels", cmd_list),
	SHELL_CMD_ARG(put, NULL, "<stage> <size> <crc32> [timeout ms]: start uploading a kernel file",
		      cmd_put, 4, 1),
	SHELL_CMD_ARG(data, NULL, "<base64>: the next bytes of the upload", cmd_data, 2, 0),
	SHELL_CMD(end, NULL, "Load the uploaded kernel and swap it in", cmd_end),
	SHELL_CMD(abort, NULL, "Drop the upload", cmd_abort),
	SHELL_CMD_ARG(load, NULL, "<stage> <file> [timeout ms]: a kernel file of the volume", cmd_load,
		      3, 1),
	SHELL_CMD_ARG(revert, NULL, "<stage> [timeout ms]: the app's own function, unload the kernel",
		      cmd_revert, 2, 1),
	SHELL_SUBCMD_SET_END,
};

/* The root's subcommand "stage" (src/apps.c). */
const union shell_cmd_entry multiapp_stage_cmds = {.entry = stage_subs};
