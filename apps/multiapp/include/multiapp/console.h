/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The console of an SSH session (doc/dev/network-services.md, phase 3, "The console mux"). A session
 * gets the Zephyr shell, unless an app has registered a console of its own (the MicroPython REPL,
 * phase 4): new sessions that ask for a shell then get that one. A session that asks for a command
 * (`ssh <host> shell`: an exec request) always gets the Zephyr shell.
 *
 * The user is the MicroPython app of the ESP32-S31 (ext/mp/mp_repl_s31.c, doc/dev/network-services.md, "Phase 4");
 * src/console_mux_core.c is tested on the PC (tests/host/test_sshkeys.py), the app's input ring in
 * tests/host/test_mp_repl.py.
 */

#ifndef MULTIAPP_CONSOLE_H_
#define MULTIAPP_CONSOLE_H_

#include <stdbool.h>
#include <stddef.h>

/*
 * One session's byte stream, which the console reads and writes as a terminal. Valid from open() until
 * the console's closed() has returned. Non-blocking; any thread.
 */
struct multiapp_console_io {
	/* Up to @p len bytes of input: how many (0 when there is none), or a negative errno. */
	int (*read)(void *ctx, void *data, size_t len);
	/* Output: how many bytes were taken (fewer than @p len when the channel is full: wait for tx_ready()). */
	int (*write)(void *ctx, const void *data, size_t len);
	/* End the session from the console's side (a `quit`, a soft exit); closed() follows. */
	void (*close)(void *ctx);
	void *ctx;
};

struct multiapp_console {
	const char *name;
	/*
	 * A new session. Return 0 and put the console's own handle in @p *session, or a negative errno: the
	 * session then gets the Zephyr shell. Called on the SSH server's thread: return at once, do the
	 * work in a thread of the console's.
	 */
	int (*open)(const struct multiapp_console_io *io, void **session);
	/* Input arrived (read() has it): called on the SSH server's thread, return at once. */
	void (*rx_ready)(void *session);
	/* The channel can take more output after a short write(). */
	void (*tx_ready)(void *session);
	/*
	 * The session is over: the peer left, or the console was unregistered. @p io is invalid after it
	 * returns. It runs with the console registry's lock held (and, for an unregistered console, on the
	 * thread that unregisters it): it must not wait for the SSH server, for a thread that waits for that
	 * lock or for the console's own tx_ready().
	 */
	void (*closed)(void *session);
	/*
	 * Optional: whether the console has taken all the input of the session (nothing left in its buffers, nothing it has
	 * not read from the channel). The client's input ended (`echo 'print(6*7)' | ssh <host>`): the mux ends the session
	 * once this is true and the output has been quiet for a moment, so that the answer is not cut off. Called with the
	 * registry's lock held, after rx_ready(): return at once.
	 */
	bool (*input_empty)(void *session);
};

/*
 * Make @p c the console of new SSH sessions. 0; -EINVAL (a missing function); -EBUSY (another is
 * registered: a REPL at a time). @p c must stay valid until it is unregistered.
 */
int multiapp_console_register(const struct multiapp_console *c);

/*
 * Take it back (the app stops): sessions that run on it are closed (their closed() runs before this
 * returns) and new ones get the Zephyr shell again. 0, or -ENOENT when it was not registered.
 */
int multiapp_console_unregister(const struct multiapp_console *c);

#endif /* MULTIAPP_CONSOLE_H_ */
