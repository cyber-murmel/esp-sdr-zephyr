/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The SSH server's callbacks (src/console_mux.c), which src/sshd.c passes to ssh_server_start().
 */

#ifndef MULTIAPP_CONSOLE_MUX_H_
#define MULTIAPP_CONSOLE_MUX_H_

#include <zephyr/net/ssh/server.h>

int cmux_server_event(struct ssh_server *sshd, const struct ssh_server_event *event, void *user_data);
int cmux_transport_event(struct ssh_transport *transport, const struct ssh_transport_event *event,
			 void *user_data);

/* Run @p fn on the sshd_work thread (src/sshd.c), one job at a time: 0 or -EBUSY. */
int sshd_run_job(void (*fn)(void *arg), void *arg);

/* Bytes of the server thread's stack never used, as of the last session's end (0: none yet). */
size_t cmux_server_stack_unused(void);

/* Milliseconds from a session's channel closing to its connection's slot being free, and from the stop of its shell
 * to the shell being free again, as of the last session (-1: none yet). */
void cmux_timing(int32_t *slot_ms, int32_t *shell_ms);

/* The server thread ended (src/sshd.c starts it again) */
void sshd_server_closed(void);

/* Sessions open now (shell and registered console), for `ssh status`; and whether the Zephyr shell is taken. */
int cmux_sessions(void);
bool cmux_shell_busy(void);

#endif /* MULTIAPP_CONSOLE_MUX_H_ */
