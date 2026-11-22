/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The "canary" shell command, for the transmit memory layout (CONFIG_ESP_SDR_S31_TXMEM):
 *   canary fill [seed]   write a pattern to TCM groups 1 and 2 (0x2F020000 to 0x2F060000)
 *   canary check         compare: a word that differs was written by something since the fill
 * Nothing of the image lives there, so after a run of rxb (DMA into group 3, both cores) every word
 * must still match. Both groups are 64 Ki words: a fill or check is a few ms.
 */

#include <stdint.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#define GRP1 0x2F020000UL
#define GRP2 0x2F040000UL
#define GRP_END 0x2F060000UL
#define MAX_SHOWN 8U

static uint32_t filled_seed;
static bool filled;

static uint32_t pat(uintptr_t a, uint32_t seed)
{
	return (uint32_t)a * 0x9E3779B1U ^ seed;
}

static int cmd_fill(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t seed = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 0xC0DEC0DEU;

	for (uintptr_t a = GRP1; a < GRP_END; a += 4U) {
		*(volatile uint32_t *)a = pat(a, seed);
	}
	filled_seed = seed;
	filled = true;
	shell_print(sh, "canary: 0x%08lx..0x%08lx filled, seed 0x%08x", GRP1, GRP_END, seed);
	return 0;
}

static int cmd_check(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t bad[2] = {0, 0}, shown = 0;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (!filled) {
		shell_error(sh, "canary: not filled");
		return -EAGAIN;
	}
	for (uintptr_t a = GRP1; a < GRP_END; a += 4U) {
		uint32_t got = *(volatile uint32_t *)a, want = pat(a, filled_seed);

		if (got != want) {
			bad[a >= GRP2]++;
			if (shown++ < MAX_SHOWN) {
				shell_print(sh, "  0x%08lx: 0x%08x, filled 0x%08x", a, got, want);
			}
		}
	}
	shell_print(sh, "canary: group 1 %u words changed, group 2 %u words changed: %s", bad[0],
		    bad[1], bad[0] + bad[1] == 0U ? "PASS" : "FAIL");
	return bad[0] + bad[1] == 0U ? 0 : -EIO;
}

SHELL_STATIC_SUBCMD_SET_CREATE(canary_cmds,
	SHELL_CMD_ARG(fill, NULL, "[seed]: write the pattern to groups 1 and 2", cmd_fill, 1, 1),
	SHELL_CMD(check, NULL, "count the words that changed since the fill", cmd_check),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(canary, &canary_cmds, "TCM groups 1 and 2 untouched: fill | check", NULL);
