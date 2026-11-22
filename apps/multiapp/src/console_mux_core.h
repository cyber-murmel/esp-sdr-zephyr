/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The console registry and the choice of a session's console (no Zephyr: tests/host/test_sshkeys.py
 * builds it for the PC). The SSH glue is src/console_mux.c.
 */

#ifndef MULTIAPP_CONSOLE_MUX_CORE_H_
#define MULTIAPP_CONSOLE_MUX_CORE_H_

#include <multiapp/console.h>

/* What the client asked for */
enum cmux_request {
	CMUX_SHELL, /* a "shell" request: an interactive login */
	CMUX_EXEC,  /* an "exec" request: ssh <host> <command> */
};

/*
 * The console for a session: a registered one for a shell request, NULL (the Zephyr shell) for an exec
 * request and when none is registered. The caller holds the lock (cmux_lock()) until it has opened it.
 */
const struct multiapp_console *cmux_pick(enum cmux_request req);

/* The registered console or NULL */
const struct multiapp_console *cmux_current(void);

/* The glue's (the PC test's own stand-ins): one lock around pick and open, register and unregister. */
void cmux_lock(void);
void cmux_unlock(void);
/* @p c was unregistered: close its sessions (under the lock). */
void cmux_removed(const struct multiapp_console *c);

#endif /* MULTIAPP_CONSOLE_MUX_CORE_H_ */
