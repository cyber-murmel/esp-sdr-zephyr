/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#if defined(MULTIAPP_EXT_VERIFY)
#include <tinycrypt/sha256.h>
#endif

#include "extsig_core.h"

const uint8_t *extsig_strip(const uint8_t *data, size_t *len)
{
	if (*len >= EXTSIG_BLOCK_LEN && memcmp(data + *len - 4U, "SIG1", 4U) == 0) {
		*len -= EXTSIG_BLOCK_LEN;
		return data + *len;
	}
	return NULL;
}

void extsig_digest(const char *name, const uint8_t *data, size_t len, uint8_t out[32])
{
#if defined(MULTIAPP_EXT_VERIFY)
	struct tc_sha256_state_struct s;

	(void)tc_sha256_init(&s);
	(void)tc_sha256_update(&s, (const uint8_t *)"MAPSIG1", 8U); /* with its 0 */
	(void)tc_sha256_update(&s, (const uint8_t *)name, strlen(name) + 1U);
	(void)tc_sha256_update(&s, data, len);
	(void)tc_sha256_final(out, &s);
#else
	/* A build that does not verify never asks */
	(void)name;
	(void)data;
	(void)len;
	memset(out, 0, 32);
#endif
}

int extsig_needed(const char *source)
{
	return strcmp(source, "built-in") != 0;
}
