/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * OpenSSH public keys for the SSH server (doc/dev/network-services.md, phase 3): the lines of an
 * authorized_keys file, the SHA-256 fingerprint OpenSSH prints, and the DER the server's key import
 * wants. No Zephyr here: tests/host/test_sshkeys.py builds it for the PC.
 *
 * The SSH server (subsys/net/lib/ssh) takes ecdsa-sha2-nistp256 and ssh-ed25519 keys from users and has
 * an ecdsa-sha2-nistp256 host key; the build has no RSA. A line of another type (ssh-rsa,
 * ecdsa-sha2-nistp384, sk-*, ssh-dss) parses, is listed with its fingerprint, and is marked unsupported.
 */

#ifndef MULTIAPP_AUTHKEYS_H_
#define MULTIAPP_AUTHKEYS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The longest key blob kept: an RSA key of 4096 bits is 535 bytes. */
#define AK_BLOB_MAX    600
#define AK_TYPE_MAX    40
#define AK_COMMENT_MAX 48
/* "SHA256:" and 43 characters of base64, and the NUL. */
#define AK_FP_LEN      51

/* What a key is, for the server's key import */
enum ak_kind {
	AK_KIND_OTHER,
	AK_KIND_RSA,
	AK_KIND_ECDSA_P256,
	AK_KIND_ED25519,
};

#define AK_ECDSA_POINT_LEN 65
#define AK_ED25519_KEY_LEN 32

struct ak_key {
	char type[AK_TYPE_MAX];       /* "ssh-rsa", "ssh-ed25519", "ecdsa-sha2-nistp256" */
	enum ak_kind kind;
	uint8_t blob[AK_BLOB_MAX];    /* the wire form: type, e, n for RSA */
	size_t blob_len;
	char comment[AK_COMMENT_MAX]; /* cut when longer */
	unsigned int bits;            /* RSA modulus bits (not used), 256 for the others */
	bool supported;               /* the server can use it */
	const char *why;              /* when not: the reason (static string) */
};

/* ak_parse_line() results besides 0 (a key) */
#define AK_SKIP     1             /* a blank line or a comment */
#define AK_E_SYNTAX (-1)          /* not "type base64 [comment]" */
#define AK_E_OPTIONS (-2)         /* a line with options (command=..., from=...): never honoured, so refused */
#define AK_E_BLOB   (-3)          /* the base64 or the blob inside is malformed or does not match the type */
#define AK_E_SIZE   (-4)          /* the blob is longer than AK_BLOB_MAX */

/*
 * One line of authorized_keys (CR, LF and trailing blanks are ignored). 0: @p k is filled (check
 * k->supported); AK_SKIP: nothing to do; else an AK_E_* error and @p k is not usable.
 */
int ak_parse_line(const char *line, size_t len, struct ak_key *k);

/* What went wrong, for a user: a static string. */
const char *ak_strerror(int err);

/* "SHA256:..." of @p blob, as ssh-keygen -lf prints it. @p out holds AK_FP_LEN bytes. */
void ak_fingerprint(const uint8_t *blob, size_t len, char out[AK_FP_LEN]);

/*
 * The raw public key of an ECDSA P-256 key (the 65 byte uncompressed point) or an Ed25519 key (32 bytes), as
 * ssh_keygen_import(..., SSH_HOST_KEY_FORMAT_RAW, ...) takes it: points into @p k. Its length, or -EINVAL.
 */
int ak_raw_pubkey(const struct ak_key *k, const uint8_t **raw);

/* The key of an uncompressed P-256 point (the host key's public part): the wire blob, for the fingerprint. */
int ak_from_ecdsa_point(const uint8_t *point, size_t len, struct ak_key *k);

/* SHA-256 (here for the fingerprint, so that the code needs no crypto library and builds on the PC). */
void ak_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

#endif /* MULTIAPP_AUTHKEYS_H_ */
