/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Shell commands for bench work: "ubt status".
 */

#include <zephyr/shell/shell.h>

#include "ubt_plat.h"
#include "ubt_usb.h"

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "mode %u (requested %u) channel %u hop mode %u modulation %u", ubt.mode,
		    ubt.requested_mode, ubt.channel, ubt.hop_mode, ubt.modulation);
	shell_print(sh, "usb %s, queue %u packets, %u dropped", ubt_usb_enabled() ? "up" : "down",
		    (unsigned int)ubt_queue_count(&ubt.queue), ubt.queue.dropped);
	shell_print(sh, "clkn %u", ubt_clock_clkn(&ubt.clock, ubt.plat->cycles()));
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(ubt_cmds,
	SHELL_CMD(status, NULL, "Device state", cmd_status),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(ubt, &ubt_cmds, "Ubertooth firmware", NULL);
