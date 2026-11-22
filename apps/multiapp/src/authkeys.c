/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See authkeys.h.
 */

#include <errno.h>
#include <string.h>

#include "authkeys.h"

/* ---- SHA-256 (FIPS 180-4) ---- */

static const uint32_t K256[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const uint8_t *p)
{
	uint32_t w[64], a, b, c, d, e, f, g, hh;

	for (int i = 0; i < 16; i++) {
		w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
		       (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
	}
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; hh = h[7];
	for (int i = 0; i < 64; i++) {
		uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
		uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));

		hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
	}
	h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void ak_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
	uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
			 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	uint8_t tail[128];
	size_t full = len / 64, rem = len % 64, tail_len;
	uint64_t bits = (uint64_t)len * 8;

	for (size_t i = 0; i < full; i++) {
		sha256_block(h, data + 64 * i);
	}
	memset(tail, 0, sizeof(tail));
	if (rem != 0) {
		memcpy(tail, data + 64 * full, rem);
	}
	tail[rem] = 0x80;
	tail_len = rem < 56 ? 64 : 128;
	for (int i = 0; i < 8; i++) {
		tail[tail_len - 1 - i] = (uint8_t)(bits >> (8 * i));
	}
	sha256_block(h, tail);
	if (tail_len == 128) {
		sha256_block(h, tail + 64);
	}
	for (int i = 0; i < 8; i++) {
		out[4 * i] = (uint8_t)(h[i] >> 24);
		out[4 * i + 1] = (uint8_t)(h[i] >> 16);
		out[4 * i + 2] = (uint8_t)(h[i] >> 8);
		out[4 * i + 3] = (uint8_t)h[i];
	}
}

/* ---- base64 ---- */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_val(char c)
{
	if (c >= 'A' && c <= 'Z') {
		return c - 'A';
	}
	if (c >= 'a' && c <= 'z') {
		return c - 'a' + 26;
	}
	if (c >= '0' && c <= '9') {
		return c - '0' + 52;
	}
	return c == '+' ? 62 : c == '/' ? 63 : -1;
}

/* The decoded length, or -1 (not base64) or -2 (does not fit in @p max). Padding is optional. */
static int b64_decode(const char *s, size_t len, uint8_t *out, size_t max)
{
	size_t n = 0;
	uint32_t acc = 0;
	int bits = 0;

	while (len > 0 && s[len - 1] == '=') {
		len--;
	}
	if (len == 0 || len % 4 == 1) {
		return -1;
	}
	for (size_t i = 0; i < len; i++) {
		int v = b64_val(s[i]);

		if (v < 0) {
			return -1;
		}
		acc = (acc << 6) | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (n >= max) {
				return -2;
			}
			out[n++] = (uint8_t)(acc >> bits);
		}
	}
	return (int)n;
}

void ak_fingerprint(const uint8_t *blob, size_t len, char out[AK_FP_LEN])
{
	uint8_t d[32];
	size_t o = 0;

	ak_sha256(blob, len, d);
	memcpy(out, "SHA256:", 7);
	o = 7;
	for (size_t i = 0; i < 32; i += 3) {
		uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < 32 ? (uint32_t)d[i + 1] << 8 : 0U) |
			     (i + 2 < 32 ? d[i + 2] : 0U);
		size_t chars = i + 2 < 32 ? 4 : 3; /* the last group: 2 bytes, 3 characters, no padding */

		for (size_t c = 0; c < chars; c++) {
			out[o++] = B64[(v >> (18 - 6 * c)) & 63U];
		}
	}
	out[o] = '\0';
}

/* ---- the wire blob of an RSA key: string "ssh-rsa", mpint e, mpint n ---- */

static bool rd_u32(const uint8_t *p, size_t len, size_t *off, uint32_t *v)
{
	if (len - *off < 4) {
		return false;
	}
	*v = (uint32_t)p[*off] << 24 | (uint32_t)p[*off + 1] << 16 | (uint32_t)p[*off + 2] << 8 | p[*off + 3];
	*off += 4;
	return true;
}

/* A string: points into @p p. */
static bool rd_str(const uint8_t *p, size_t len, size_t *off, const uint8_t **s, size_t *slen)
{
	uint32_t n;

	if (!rd_u32(p, len, off, &n) || n > len - *off) {
		return false;
	}
	*s = p + *off;
	*slen = n;
	*off += n;
	return true;
}

/* An unsigned big-endian number without its leading zeros. */
static void strip0(const uint8_t **s, size_t *n)
{
	while (*n > 0 && **s == 0) {
		(*s)++;
		(*n)--;
	}
}

static unsigned int bit_length(const uint8_t *s, size_t n)
{
	unsigned int b = 0;

	if (n == 0) {
		return 0;
	}
	for (uint8_t top = s[0]; top != 0; top >>= 1) {
		b++;
	}
	return (unsigned int)(n - 1) * 8U + b;
}

/* Type, e and n of an ssh-rsa blob, the numbers stripped. false: malformed. */
static bool rsa_parts(const uint8_t *blob, size_t len, const uint8_t **e, size_t *elen,
		      const uint8_t **n, size_t *nlen)
{
	const uint8_t *t;
	size_t off = 0, tlen;

	if (!rd_str(blob, len, &off, &t, &tlen) || tlen != 7 || memcmp(t, "ssh-rsa", 7) != 0 ||
	    !rd_str(blob, len, &off, e, elen) || !rd_str(blob, len, &off, n, nlen) || off != len) {
		return false;
	}
	strip0(e, elen);
	strip0(n, nlen);
	return *elen >= 1 && *elen <= 8 && *nlen >= 1;
}

/* ---- authorized_keys lines ---- */

static bool is_blank(char c)
{
	return c == ' ' || c == '\t';
}

static size_t token(const char *s, size_t len)
{
	size_t n = 0;

	while (n < len && !is_blank(s[n])) {
		n++;
	}
	return n;
}

static bool has_prefix(const char *s, size_t len, const char *prefix)
{
	size_t n = strlen(prefix);

	return len >= n && memcmp(s, prefix, n) == 0;
}

int ak_parse_line(const char *line, size_t len, struct ak_key *k)
{
	size_t n;
	int blob;

	memset(k, 0, sizeof(*k));
	while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || is_blank(line[len - 1]))) {
		len--;
	}
	while (len > 0 && is_blank(*line)) {
		line++;
		len--;
	}
	if (len == 0 || *line == '#') {
		return AK_SKIP;
	}
	/*
	 * No control bytes: the line is written to the key file as it is, and a newline in it (an exec command
	 * with a quoted \n) would make a second line that nothing has checked.
	 */
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)line[i];

		if ((c < 0x20 && c != '\t') || c == 0x7f) {
			return AK_E_SYNTAX;
		}
	}
	n = token(line, len);
	if (!has_prefix(line, n, "ssh-") && !has_prefix(line, n, "ecdsa-sha2-") &&
	    !has_prefix(line, n, "sk-")) {
		/* command="...", from="...", restrict, no-pty: not honoured, so not accepted */
		return AK_E_OPTIONS;
	}
	if (n >= sizeof(k->type)) {
		return AK_E_SYNTAX;
	}
	memcpy(k->type, line, n);
	line += n;
	len -= n;
	while (len > 0 && is_blank(*line)) {
		line++;
		len--;
	}
	n = token(line, len);
	if (n == 0) {
		return AK_E_SYNTAX;
	}
	blob = b64_decode(line, n, k->blob, sizeof(k->blob));
	if (blob == -2) {
		return AK_E_SIZE;
	}
	if (blob < 0) {
		return AK_E_BLOB;
	}
	k->blob_len = (size_t)blob;
	line += n;
	len -= n;
	while (len > 0 && is_blank(*line)) {
		line++;
		len--;
	}
	if (len >= sizeof(k->comment)) {
		len = sizeof(k->comment) - 1;
	}
	memcpy(k->comment, line, len);

	/* The blob starts with the type again: a key of another type under this name is no key. */
	{
		const uint8_t *t;
		size_t off = 0, tlen;

		if (!rd_str(k->blob, k->blob_len, &off, &t, &tlen) || tlen != strlen(k->type) ||
		    memcmp(t, k->type, tlen) != 0) {
			return AK_E_BLOB;
		}
	}
	if (strcmp(k->type, "ssh-rsa") == 0) {
		const uint8_t *e, *m;
		size_t elen, mlen;

		/* Listed (with its size) but never used: this build has no RSA */
		k->kind = AK_KIND_RSA;
		if (!rsa_parts(k->blob, k->blob_len, &e, &elen, &m, &mlen)) {
			return AK_E_BLOB;
		}
		k->bits = bit_length(m, mlen);
		k->why = "RSA keys are not supported (use ssh-ed25519 or ecdsa-sha2-nistp256)";
		return 0;
	}
	if (strcmp(k->type, "ssh-ed25519") == 0) {
		const uint8_t *key;

		k->kind = AK_KIND_ED25519;
		if (ak_raw_pubkey(k, &key) < 0) {
			return AK_E_BLOB;
		}
		k->bits = 256;
		k->supported = true;
		return 0;
	}
	if (strcmp(k->type, "ecdsa-sha2-nistp256") == 0) {
		const uint8_t *key;

		k->kind = AK_KIND_ECDSA_P256;
		if (ak_raw_pubkey(k, &key) < 0) {
			return AK_E_BLOB;
		}
		k->bits = 256;
		k->supported = true;
		return 0;
	}
	k->why = "the server takes ssh-ed25519 and ecdsa-sha2-nistp256 keys only";
	return 0;
}

const char *ak_strerror(int err)
{
	switch (err) {
	case AK_E_SYNTAX:
		return "not a line of the form: <type> <base64 key> [comment]";
	case AK_E_OPTIONS:
		return "key options are not supported (a line starts with the key type)";
	case AK_E_BLOB:
		return "the key is malformed";
	case AK_E_SIZE:
		return "the key is too long";
	default:
		return "error";
	}
}

/* ---- raw keys ---- */

static uint8_t *put_u32(uint8_t *p, size_t v)
{
	*p++ = (uint8_t)(v >> 24);
	*p++ = (uint8_t)(v >> 16);
	*p++ = (uint8_t)(v >> 8);
	*p++ = (uint8_t)v;
	return p;
}

int ak_raw_pubkey(const struct ak_key *k, const uint8_t **raw)
{
	const uint8_t *t, *curve, *key;
	size_t off = 0, tlen, clen, klen;

	if (!rd_str(k->blob, k->blob_len, &off, &t, &tlen)) {
		return -EINVAL;
	}
	if (k->kind == AK_KIND_ED25519) {
		if (!rd_str(k->blob, k->blob_len, &off, &key, &klen) || off != k->blob_len ||
		    klen != AK_ED25519_KEY_LEN) {
			return -EINVAL;
		}
	} else if (k->kind == AK_KIND_ECDSA_P256) {
		if (!rd_str(k->blob, k->blob_len, &off, &curve, &clen) || clen != 8 ||
		    memcmp(curve, "nistp256", 8) != 0 || !rd_str(k->blob, k->blob_len, &off, &key, &klen) ||
		    off != k->blob_len || klen != AK_ECDSA_POINT_LEN || key[0] != 0x04) {
			return -EINVAL;
		}
	} else {
		return -EINVAL;
	}
	*raw = key;
	return (int)klen;
}

int ak_from_ecdsa_point(const uint8_t *point, size_t len, struct ak_key *k)
{
	uint8_t *p;

	memset(k, 0, sizeof(*k));
	if (len != AK_ECDSA_POINT_LEN || point[0] != 0x04) {
		return -EINVAL;
	}
	p = k->blob;
	p = put_u32(p, 19);
	memcpy(p, "ecdsa-sha2-nistp256", 19);
	p += 19;
	p = put_u32(p, 8);
	memcpy(p, "nistp256", 8);
	p += 8;
	p = put_u32(p, len);
	memcpy(p, point, len);
	p += len;
	k->blob_len = (size_t)(p - k->blob);
	strcpy(k->type, "ecdsa-sha2-nistp256");
	k->kind = AK_KIND_ECDSA_P256;
	k->bits = 256;
	k->supported = true;
	return 0;
}
