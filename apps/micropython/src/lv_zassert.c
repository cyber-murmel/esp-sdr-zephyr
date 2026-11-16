/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The LVGL binding (lv_mp.c) wraps every function the headers it parses declare. Zephyr's
 * sys/zassert.h declares zassert_post_action() whatever CONFIG_ASSERT says, but
 * subsys/debug/zassert defines it only with assertions on; this is it for builds without them,
 * doing what a failed assertion does there.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/__assert.h>

FUNC_NORETURN void zassert_post_action(const char *file, unsigned int line)
{
	ARG_UNUSED(file);
	ARG_UNUSED(line);
	k_panic();
	CODE_UNREACHABLE;
}
