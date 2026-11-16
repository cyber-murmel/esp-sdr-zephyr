/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The smallest multiapp app, a loader check: prints, calls a host function,
 * calls its own code directly and through a table of function pointers, has a
 * constructor and a shell command.
 */

#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/printk.h>

#include <esp_attr.h>

#include <esp_sdr/esp_sdr.h>

#include <esp_phy_init.h>

#if defined(CONFIG_MULTIAPP_MODULE_TESTS)
#include "hellomod.h"
#endif

static const char greeting[] = "hello from an extension";
static int constructed;

/* IRAM_ATTR code lands in the extension's text like the rest. */
static IRAM_ATTR int twice(int v)
{
	return 2 * v;
}

static int thrice(int v)
{
	return 3 * v;
}

/* Code addresses in data: relocated to where the code executes. */
static int (*const ops[])(int v) = {twice, thrice};

static void __attribute__((constructor)) hello_init(void)
{
	constructed++;
}

static int cmd_hello(const struct shell *sh, size_t argc, char **argv)
{
	int v = argc > 1 ? atoi(argv[1]) : 21;

	shell_print(sh, "hello: ops %d %d, direct %d", ops[0](v), ops[1](v), thrice(v));
	return 0;
}

#if defined(CONFIG_SMP)
/*
 * Atomic increments from both CPUs at once, on a counter in the extension's
 * data (PSRAM) and on one on the shell's stack (internal RAM).
 */
#define ATOMIC_ROUNDS 200000
static atomic_t psram_count;
static K_THREAD_STACK_DEFINE(atomic_stack, 1024);
static struct k_thread atomic_thread;
static K_SEM_DEFINE(atomic_done, 0, 1);

static void atomic_worker(void *p1, void *p2, void *p3)
{
	atomic_t *internal = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	for (int i = 0; i < ATOMIC_ROUNDS; i++) {
		atomic_inc(&psram_count);
		atomic_inc(internal);
	}
	k_sem_give(&atomic_done);
}

static int cmd_atomic(const struct shell *sh, size_t argc, char **argv)
{
	atomic_t internal = ATOMIC_INIT(0);

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	atomic_set(&psram_count, 0);
	k_thread_create(&atomic_thread, atomic_stack, K_THREAD_STACK_SIZEOF(atomic_stack),
			atomic_worker, &internal, NULL, NULL, 0, 0, K_FOREVER);
	k_thread_cpu_pin(&atomic_thread, arch_curr_cpu()->id ^ 1);
	k_thread_start(&atomic_thread);
	for (int i = 0; i < ATOMIC_ROUNDS; i++) {
		atomic_inc(&psram_count);
		atomic_inc(&internal);
	}
	k_sem_take(&atomic_done, K_FOREVER);
	shell_print(sh, "atomic: psram %p %ld, internal %p %ld (expect %d)", (void *)&psram_count,
		    atomic_get(&psram_count), (void *)&internal, atomic_get(&internal),
		    2 * ATOMIC_ROUNDS);
	return 0;
}
#endif

#if defined(CONFIG_MULTIAPP_MODULE_TESTS)
/* Calls into hellolib (PSRAM), which calls hellofast (internal RAM), and where each runs. */
static int cmd_mod(const struct shell *sh, size_t argc, char **argv)
{
	const uint32_t rounds = 100000;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "mod: fast code %p data %p value %d, lib code %p, chain %d",
		    (void *)hellofast_twice, (void *)&hellofast_value, hellofast_value,
		    (void *)hellolib_chain, hellolib_chain(5));
	shell_print(sh, "mod: loop cycles fast %u, lib %u", hellofast_loop(rounds),
		    hellolib_loop(rounds));
	shell_print(sh, "mod: device %s, %d inits",
		    device_get_binding("hellolib") != NULL ? "found" : "missing",
		    hellolib_dev_inits());
	return 0;
}
#endif

/* A call into the PHY's blob, a module. */
static int cmd_phy(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "phy: blob code %p, version %s", (void *)get_phy_version_str,
		    get_phy_version_str());
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(hello_cmds,
#if defined(CONFIG_SMP)
	SHELL_CMD(atomic, NULL, "atomic increments from both CPUs", cmd_atomic),
#endif
#if defined(CONFIG_MULTIAPP_MODULE_TESTS)
	SHELL_CMD(mod, NULL, "calls across modules, PSRAM and internal RAM", cmd_mod),
#endif
	SHELL_CMD(phy, NULL, "a call into the PHY's blob as a module", cmd_phy),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_ARG_REGISTER(hello, &hello_cmds, "[n]: the loader check", cmd_hello, 1, 1);

int main(void)
{
	printk("%s: constructed %d, code at %p, the radio is at %u MHz\n", greeting, constructed,
	       (void *)main, esp_sdr_get_freq());
	return twice(21);
}
