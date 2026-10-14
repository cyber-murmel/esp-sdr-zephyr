/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AES-128, CCM* and the Zigbee MMO hash: see include/zigbee/zb_crypto.h.
 */
#include <errno.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <zigbee/zb_crypto.h>

static const uint8_t sbox[256] = {
	0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5,
	0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
	0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
	0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
	0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc,
	0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
	0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a,
	0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
	0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
	0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
	0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b,
	0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
	0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85,
	0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
	0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
	0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
	0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17,
	0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
	0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88,
	0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
	0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
	0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
	0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9,
	0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
	0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6,
	0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
	0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
	0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
	0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94,
	0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
	0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68,
	0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

static uint8_t xtime(uint8_t a)
{
	return (uint8_t)((a << 1) ^ ((a & 0x80U) != 0U ? 0x1BU : 0U));
}

/* Key schedule: 11 round keys of 16 bytes. */
static void expand_key(const uint8_t key[16], uint8_t rk[176])
{
	uint8_t rcon = 1;

	memcpy(rk, key, 16);
	for (int i = 16; i < 176; i += 4) {
		uint8_t t[4];

		memcpy(t, &rk[i - 4], 4);
		if (i % 16 == 0) {
			uint8_t k = t[0];

			t[0] = (uint8_t)(sbox[t[1]] ^ rcon);
			t[1] = sbox[t[2]];
			t[2] = sbox[t[3]];
			t[3] = sbox[k];
			rcon = xtime(rcon);
		}
		for (int j = 0; j < 4; j++) {
			rk[i + j] = (uint8_t)(rk[i - 16 + j] ^ t[j]);
		}
	}
}

static void add_round_key(uint8_t s[16], const uint8_t *rk)
{
	for (int i = 0; i < 16; i++) {
		s[i] ^= rk[i];
	}
}

/* SubBytes and ShiftRows in one pass (state is column major, as the block bytes). */
static void sub_shift(uint8_t s[16])
{
	uint8_t t[16];

	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			t[4 * c + r] = sbox[s[4 * ((c + r) % 4) + r]];
		}
	}
	memcpy(s, t, 16);
}

static void mix_columns(uint8_t s[16])
{
	for (int c = 0; c < 4; c++) {
		uint8_t *p = &s[4 * c];
		uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
		uint8_t all = a0 ^ a1 ^ a2 ^ a3;

		p[0] ^= all ^ xtime(a0 ^ a1);
		p[1] ^= all ^ xtime(a1 ^ a2);
		p[2] ^= all ^ xtime(a2 ^ a3);
		p[3] ^= all ^ xtime(a3 ^ a0);
	}
}

void zb_aes128_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
	uint8_t rk[176], s[16];

	expand_key(key, rk);
	memcpy(s, in, 16);
	add_round_key(s, rk);
	for (int round = 1; round < 10; round++) {
		sub_shift(s);
		mix_columns(s);
		add_round_key(s, &rk[16 * round]);
	}
	sub_shift(s);
	add_round_key(s, &rk[160]);
	memcpy(out, s, 16);
}

/* ------------------------------------------------------------------------------------------ */
/* CCM* (M = 4, L = 2) */

static void xor_into(uint8_t *dst, const uint8_t *src, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		dst[i] ^= src[i];
	}
}

/* CBC-MAC over the header block, the additional data and the message; returns T (16 bytes). */
static void cbc_mac(const uint8_t key[16], const uint8_t nonce[13], const uint8_t *aad,
		    size_t alen, const uint8_t *msg, size_t mlen, uint8_t t[16])
{
	uint8_t b[16] = {0};
	size_t off;

	/* B0: flags (Adata, M' = (4 - 2) / 2, L' = 1), nonce, message length */
	b[0] = (uint8_t)((alen > 0U ? 0x40U : 0U) | (((ZB_MIC_LEN - 2U) / 2U) << 3) | 1U);
	memcpy(&b[1], nonce, 13);
	b[14] = (uint8_t)(mlen >> 8);
	b[15] = (uint8_t)mlen;
	zb_aes128_encrypt(key, b, t);

	if (alen > 0U) {
		/* The length prefix (alen < 0xFF00: two bytes), then the data, zero padded. */
		memset(b, 0, sizeof(b));
		b[0] = (uint8_t)(alen >> 8);
		b[1] = (uint8_t)alen;
		off = 2;
		for (size_t i = 0; i < alen;) {
			size_t n = MIN(alen - i, 16U - off);

			memcpy(&b[off], &aad[i], n);
			i += n;
			off += n;
			if (off == 16U || i == alen) {
				xor_into(t, b, 16);
				zb_aes128_encrypt(key, t, t);
				memset(b, 0, sizeof(b));
				off = 0;
			}
		}
	}
	for (size_t i = 0; i < mlen; i += 16U) {
		size_t n = MIN(mlen - i, 16U);

		memset(b, 0, sizeof(b));
		memcpy(b, &msg[i], n);
		xor_into(t, b, 16);
		zb_aes128_encrypt(key, t, t);
	}
}

/* CTR keystream block @p ctr (A_i): flags L' = 1, nonce, counter. */
static void ctr_block(const uint8_t key[16], const uint8_t nonce[13], uint16_t ctr, uint8_t s[16])
{
	uint8_t a[16];

	a[0] = 1;
	memcpy(&a[1], nonce, 13);
	a[14] = (uint8_t)(ctr >> 8);
	a[15] = (uint8_t)ctr;
	zb_aes128_encrypt(key, a, s);
}

static void ctr_crypt(const uint8_t key[16], const uint8_t nonce[13], uint8_t *data, size_t len)
{
	uint8_t s[16];

	for (size_t i = 0; i < len; i += 16U) {
		ctr_block(key, nonce, (uint16_t)(1U + i / 16U), s);
		xor_into(&data[i], s, MIN(len - i, 16U));
	}
}

void zb_ccm_encrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t *aad,
		    size_t aad_len, uint8_t *data, size_t len, uint8_t mic[4])
{
	uint8_t t[16], s0[16];

	cbc_mac(key, nonce, aad, aad_len, data, len, t);
	ctr_block(key, nonce, 0, s0);
	for (int i = 0; i < ZB_MIC_LEN; i++) {
		mic[i] = (uint8_t)(t[i] ^ s0[i]);
	}
	ctr_crypt(key, nonce, data, len);
}

int zb_ccm_decrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t *aad,
		   size_t aad_len, uint8_t *data, size_t len, const uint8_t mic[4])
{
	uint8_t t[16], s0[16], diff = 0;

	ctr_crypt(key, nonce, data, len);
	cbc_mac(key, nonce, aad, aad_len, data, len, t);
	ctr_block(key, nonce, 0, s0);
	for (int i = 0; i < ZB_MIC_LEN; i++) {
		diff |= (uint8_t)(t[i] ^ s0[i] ^ mic[i]);
	}
	return diff == 0U ? 0 : -EBADMSG;
}

/* ------------------------------------------------------------------------------------------ */
/* Matyas-Meyer-Oseas hash */

/* H_i = E_{H_{i-1}}(M_i) xor M_i over the padded message. */
static void mmo_block(uint8_t h[16], const uint8_t m[16])
{
	uint8_t e[16];

	zb_aes128_encrypt(h, m, e);
	for (int i = 0; i < 16; i++) {
		h[i] = (uint8_t)(e[i] ^ m[i]);
	}
}

void zb_mmo_hash(const uint8_t *msg, size_t len, uint8_t out[16])
{
	uint8_t h[16] = {0}, b[16];
	size_t i = 0;
	size_t bits = len * 8U;

	for (; len - i >= 16U; i += 16U) {
		mmo_block(h, &msg[i]);
	}
	/* Pad: a 1 bit, zeros up to 14 bytes of the last block, then the length in bits. */
	memset(b, 0, sizeof(b));
	memcpy(b, &msg[i], len - i);
	b[len - i] = 0x80;
	if (len - i >= 14U) {
		mmo_block(h, b);
		memset(b, 0, sizeof(b));
	}
	b[14] = (uint8_t)(bits >> 8);
	b[15] = (uint8_t)bits;
	mmo_block(h, b);
	memcpy(out, h, 16);
}

void zb_keyed_hash(const uint8_t key[16], const uint8_t *msg, size_t len, uint8_t out[16])
{
	uint8_t buf[16 + 64], inner[16];

	len = MIN(len, 64U); /* only short messages (a link key derivation) are hashed with a key */
	for (int i = 0; i < 16; i++) {
		buf[i] = (uint8_t)(key[i] ^ 0x36U);
	}
	memcpy(&buf[16], msg, len);
	zb_mmo_hash(buf, 16U + len, inner);
	for (int i = 0; i < 16; i++) {
		buf[i] = (uint8_t)(key[i] ^ 0x5CU);
	}
	memcpy(&buf[16], inner, 16);
	zb_mmo_hash(buf, 32, out);
}

void zb_key_transport_key(const uint8_t link_key[16], uint8_t out[16])
{
	static const uint8_t zero = 0x00;

	zb_keyed_hash(link_key, &zero, 1, out);
}
