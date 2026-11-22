/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The image version as sysbuild signed it (apps/common/sysbuild/signing.cmake): "X.Y.Z+<build>",
 * the build number the git short hash (8 hex digits), bit 15 of the patch field set for a dirty
 * tree. Printed as git describe does: "0.2.0 (g01a2b3c4-dirty)".
 */

#ifndef MULTIAPP_GITVER_H_
#define MULTIAPP_GITVER_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GITVER_DIRTY 0x8000UL /* in the patch field */

#if defined(CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION)
#define GITVER_STRING CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION
#else
/* Without sysbuild and MCUboot: the VERSION file's */
#define GITVER_STRING APP_VERSION_STRING
#endif

static inline void gitver_text(char *buf, size_t size)
{
	const char *v = GITVER_STRING;
	const char *plus = strchr(v, '+');
	unsigned long build = plus != NULL ? strtoul(plus + 1, NULL, 10) : 0UL;
	unsigned long maj, min = 0, patch = 0;
	char *end;

	/* strtoul, not sscanf: this runs on the S3's main stack too, which has little to spare */
	maj = strtoul(v, &end, 10);
	if (*end == '.') {
		min = strtoul(end + 1, &end, 10);
	}
	if (*end == '.') {
		patch = strtoul(end + 1, &end, 10);
	}
	if (build == 0UL) {
		snprintf(buf, size, "%lu.%lu.%lu", maj, min, patch & (GITVER_DIRTY - 1UL));
	} else {
		snprintf(buf, size, "%lu.%lu.%lu (g%08lx%s)", maj, min, patch & (GITVER_DIRTY - 1UL),
			 build, (patch & GITVER_DIRTY) != 0UL ? "-dirty" : "");
	}
}

#endif /* MULTIAPP_GITVER_H_ */
