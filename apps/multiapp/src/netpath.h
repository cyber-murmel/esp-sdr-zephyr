/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What MCUmgr's file commands may name (src/netsvc.c). No Zephyr here: tests/host/test_mcumgrlib.py
 * builds it for the PC.
 */

#ifndef MULTIAPP_NETPATH_H_
#define MULTIAPP_NETPATH_H_

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>

#define NETPATH_VOLUME "/FLASH:/"

/* /FLASH:/..., no ".." element, no backslash. */
static inline bool netpath_ok(const char *path)
{
	if (strncmp(path, NETPATH_VOLUME, strlen(NETPATH_VOLUME)) != 0 || strchr(path, '\\') != NULL) {
		return false;
	}
	for (const char *p = path; (p = strstr(p, "..")) != NULL; p += 2) {
		bool start = p == path || p[-1] == '/';
		bool end = p[2] == '\0' || p[2] == '/';

		if (start && end) {
			return false;
		}
	}
	return true;
}

/*
 * A name FAT would open as an extension: it ignores case and trailing dots and spaces, and has short
 * aliases (NAME~1.LLE)
 */
static inline bool netpath_is_extension(const char *path)
{
	size_t n = strlen(path);

	if (strchr(path, '~') != NULL) {
		return true;
	}
	while (n > 0 && (path[n - 1] == '.' || path[n - 1] == ' ')) {
		n--;
	}
	return n >= 6 && strncasecmp(path + n - 6, ".llext", 6) == 0;
}

#endif /* MULTIAPP_NETPATH_H_ */
