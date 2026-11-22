/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Signed extension files: apptrail.py --key appends the ED25519 signature (64 bytes) and "SIG1" to
 * the file (ELF, trailer, signature block). Whoever reads a trailer calls this first.
 */

#ifndef MULTIAPP_EXTSIG_H_
#define MULTIAPP_EXTSIG_H_

#include <stddef.h>
#include <stdint.h>

/*
 * Strip the signature block off the file's end (*len shrinks by it, if there is one), and when
 * @p source is not "built-in" (MCUboot's image signature covers those) check it against the key
 * compiled into the host. 0 when the file may be used; -EACCES for no or a wrong signature (logged
 * with the reason); only in a build that has the key, MULTIAPP_EXT_VERIFY: else nothing is checked.
 */
int multiapp_sig_check(const char *name, const char *source, const uint8_t *data, size_t *len);

/*
 * Verify an ED25519 signature of @p msg (the message itself) by @p pub, on the signature worker
 * (the 4.3 KB stack the verifier needs). False for a bad signature, and always without the verifier
 * (a build with no signing key: MULTIAPP_EXT_VERIFY).
 */
bool multiapp_ed25519_verify(const uint8_t pub[32], const uint8_t *msg, size_t len, const uint8_t sig[64]);

/* Whether this build checks signatures: then fs_mgmt may write *.llext files, else not. */
bool multiapp_sig_verifies(void);

#endif /* MULTIAPP_EXTSIG_H_ */
