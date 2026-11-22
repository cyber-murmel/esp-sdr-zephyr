/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See console_mux_core.h.
 */

#include <errno.h>
#include <stddef.h>

#include "console_mux_core.h"

static const struct multiapp_console *repl;

int multiapp_console_register(const struct multiapp_console *c)
{
	int ret = 0;

	if (c == NULL || c->name == NULL || c->open == NULL || c->rx_ready == NULL || c->closed == NULL) {
		return -EINVAL;
	}
	cmux_lock();
	if (repl != NULL) {
		ret = -EBUSY;
	} else {
		repl = c;
	}
	cmux_unlock();
	return ret;
}

int multiapp_console_unregister(const struct multiapp_console *c)
{
	int ret = 0;

	cmux_lock();
	if (c == NULL || repl != c) {
		ret = -ENOENT;
	} else {
		repl = NULL;
		cmux_removed(c);
	}
	cmux_unlock();
	return ret;
}

const struct multiapp_console *cmux_current(void)
{
	return repl;
}

const struct multiapp_console *cmux_pick(enum cmux_request req)
{
	return req == CMUX_SHELL ? repl : NULL;
}
