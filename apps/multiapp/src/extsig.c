/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ED25519 check of extension files (extsig.h, extsig_core.h): MCUboot's own verifier (ext/fiat,
 * tinycrypt SHA-512 and SHA-256), the public key generated at build time from the signing key
 * (pubkey.py).
 */

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "extsig.h"
#include "extsig_core.h"

LOG_MODULE_DECLARE(multiapp, LOG_LEVEL_INF);

#if defined(MULTIAPP_EXT_VERIFY)
#include "multiapp_pubkey.h"

extern int ED25519_verify(const uint8_t *message, size_t message_len, const uint8_t signature[EXTSIG_LEN],
			  const uint8_t public_key[32]);

/*
 * The verifier's frame is 3136 bytes (-fstack-usage) and its callees and SHA-512 add some 1.3 KB: more
 * than the main thread (4096, 1736 used) or the shell has. So it runs on a worker of its own, a stack of
 * 6 KiB in internal RAM (group 0 has room; never PSRAM, the worker may run while the flash is busy), and
 * the caller, from whichever thread, waits for it. One at a time.
 */
#define VERIFY_STACK 6144

static K_MUTEX_DEFINE(verify_lock);
static K_SEM_DEFINE(verify_start, 0, 1);
static K_SEM_DEFINE(verify_done, 0, 1);
static struct {
	const char *name; /* NULL: a plain message (multiapp_ed25519_verify) */
	const uint8_t *data;
	size_t len;
	const uint8_t *sig;
	const uint8_t *pub;
	int ok;
} job;

static void verify_thread(void *a, void *b, void *c)
{
	uint8_t digest[32];

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		k_sem_take(&verify_start, K_FOREVER);
		if (job.name != NULL) {
			extsig_digest(job.name, job.data, job.len, digest);
			job.ok = ED25519_verify(digest, sizeof(digest), job.sig, multiapp_ext_pubkey);
		} else {
			job.ok = ED25519_verify(job.data, job.len, job.sig, job.pub);
		}
		k_sem_give(&verify_done);
	}
}

K_THREAD_DEFINE(verify_tid, VERIFY_STACK, verify_thread, NULL, NULL, NULL, 5, 0, 0);

static bool verify(const char *name, const uint8_t *data, size_t len, const uint8_t *sig)
{
	bool ok;

	k_mutex_lock(&verify_lock, K_FOREVER);
	job.name = name;
	job.data = data;
	job.len = len;
	job.sig = sig;
	job.pub = multiapp_ext_pubkey;
	k_sem_give(&verify_start);
	k_sem_take(&verify_done, K_FOREVER);
	ok = job.ok != 0;
	k_mutex_unlock(&verify_lock);
	return ok;
}

/* A signature of the SSH server's users (src/sshd.c), on the same worker, below the streams' threads. */
bool multiapp_ed25519_verify(const uint8_t pub[32], const uint8_t *msg, size_t len, const uint8_t sig[64])
{
	bool ok;

	k_mutex_lock(&verify_lock, K_FOREVER);
	job.name = NULL;
	job.data = msg;
	job.len = len;
	job.sig = sig;
	job.pub = pub;
	/* The worker runs at 5 for the app loader; a login must not take the USB threads' (8) time */
	k_thread_priority_set(verify_tid, 10);
	k_sem_give(&verify_start);
	k_sem_take(&verify_done, K_FOREVER);
	k_thread_priority_set(verify_tid, 5);
	ok = job.ok != 0;
	k_mutex_unlock(&verify_lock);
	return ok;
}
#else
bool multiapp_ed25519_verify(const uint8_t pub[32], const uint8_t *msg, size_t len, const uint8_t sig[64])
{
	ARG_UNUSED(pub);
	ARG_UNUSED(msg);
	ARG_UNUSED(len);
	ARG_UNUSED(sig);
	return false;
}
#endif

bool multiapp_sig_verifies(void)
{
	return IS_ENABLED(MULTIAPP_EXT_VERIFY);
}

int multiapp_sig_check(const char *name, const char *source, const uint8_t *data, size_t *len)
{
	const uint8_t *sig = extsig_strip(data, len);
	size_t n = *len;

#if defined(MULTIAPP_EXT_VERIFY)
	if (extsig_needed(source)) {
		uint32_t t0;
		bool ok;

		if (sig == NULL) {
			LOG_ERR("%s (%s): no signature block in %zu bytes", name, source, n);
			return -EACCES;
		}
		if (n == 0U) {
			LOG_ERR("%s (%s): nothing to verify", name, source);
			return -EACCES;
		}
		LOG_INF("%s (%s): verifying %zu bytes", name, source, n);
		t0 = k_cycle_get_32();
		ok = verify(name, data, n, sig);
		LOG_INF("%s (%s): signature %s after %u us", name, source, ok ? "ok" : "BAD",
			(unsigned int)k_cyc_to_us_floor32(k_cycle_get_32() - t0));
		if (!ok) {
			LOG_ERR("%s (%s): the signature does not match the host's key, or is for another name",
				name, source);
			return -EACCES;
		}
	}
#else
	ARG_UNUSED(name);
	ARG_UNUSED(source);
	ARG_UNUSED(sig);
	ARG_UNUSED(n);
#endif
	return 0;
}
