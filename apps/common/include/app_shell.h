/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Shell argument parsing shared by the apps: a whole argument as a number in a range (decimal,
 * 0x hex or 0 octal), the error printed on the shell. Header only, so that apps which do not
 * build apps/common (wpan on the C6) can use it through the include path alone.
 */

#ifndef APP_SHELL_H_
#define APP_SHELL_H_

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/shell/shell.h>

/* s as a long in [min, max] into *out; -EINVAL (printed) otherwise. */
static inline int app_shell_long(const struct shell *sh, const char *s, long min, long max,
				 long *out)
{
	char *end;
	long v = strtol(s, &end, 0);

	if (*s == '\0' || *end != '\0' || v < min || v > max) {
		shell_error(sh, "bad value: %s (%ld to %ld)", s, min, max);
		return -EINVAL;
	}
	*out = v;
	return 0;
}

/* s as an unsigned 32-bit number up to max into *out; -EINVAL (printed) otherwise. */
static inline int app_shell_u32(const struct shell *sh, const char *s, uint32_t max,
				uint32_t *out)
{
	char *end;
	unsigned long v = strtoul(s, &end, 0);

	if (*s == '\0' || *s == '-' || *end != '\0' || v > max) {
		shell_error(sh, "bad value: %s (0 to %u)", s, (unsigned int)max);
		return -EINVAL;
	}
	*out = (uint32_t)v;
	return 0;
}

#endif /* APP_SHELL_H_ */
