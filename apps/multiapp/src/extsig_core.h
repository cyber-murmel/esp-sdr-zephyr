/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The pure part of the extension signature check (no Zephyr: tests/host/test_extsig.py builds it for
 * the PC). The file is ELF, trailer, then a block: the 64 byte ED25519 signature and "SIG1". What is
 * signed is the SHA-256 of "MAPSIG1", 0, the name the file is for, 0, ELF and trailer: a file signed
 * as one app does not verify as another, as a module or as a kernel.
 */

#ifndef MULTIAPP_EXTSIG_CORE_H_
#define MULTIAPP_EXTSIG_CORE_H_

#include <stddef.h>
#include <stdint.h>

#define EXTSIG_LEN       64U
#define EXTSIG_BLOCK_LEN (EXTSIG_LEN + 4U)

/* The signature block at the end of data[0..*len), if there is one: *len shrinks by it, returns its
 * start; else NULL and *len stays. */
const uint8_t *extsig_strip(const uint8_t *data, size_t *len);

/* What is signed: the digest over the name and data[0..len). */
void extsig_digest(const char *name, const uint8_t *data, size_t len, uint8_t out[32]);

/* Whether a file from @p source needs the signature: all but the image's own built-in copies. */
int extsig_needed(const char *source);

#endif /* MULTIAPP_EXTSIG_CORE_H_ */
