/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The cryptography Zigbee needs: AES-128, CCM* with a 4 byte MIC (security level 5) and the
 * Matyas-Meyer-Oseas hash with its keyed form for deriving link keys. Portable C. The AES is a
 * plain table implementation: it does not run in constant time, which is no issue for keys that
 * only ever cross a radio link at this duty cycle.
 */
#ifndef ZIGBEE_ZB_CRYPTO_H_
#define ZIGBEE_ZB_CRYPTO_H_

#include <stddef.h>
#include <stdint.h>

#define ZB_KEY_LEN   16
#define ZB_MIC_LEN   4
#define ZB_NONCE_LEN 13

/* One block, ECB. */
void zb_aes128_encrypt(const uint8_t key[ZB_KEY_LEN], const uint8_t in[16], uint8_t out[16]);

/*
 * CCM* with M = 4, L = 2: encrypt @p data in place and write the MIC over @p aad and the plain
 * text. @p aad may be empty.
 */
void zb_ccm_encrypt(const uint8_t key[ZB_KEY_LEN], const uint8_t nonce[ZB_NONCE_LEN],
		    const uint8_t *aad, size_t aad_len, uint8_t *data, size_t len,
		    uint8_t mic[ZB_MIC_LEN]);

/* Decrypt in place and check the MIC; 0, or -EBADMSG (the data is then garbage). */
int zb_ccm_decrypt(const uint8_t key[ZB_KEY_LEN], const uint8_t nonce[ZB_NONCE_LEN],
		   const uint8_t *aad, size_t aad_len, uint8_t *data, size_t len,
		   const uint8_t mic[ZB_MIC_LEN]);

/* Matyas-Meyer-Oseas hash with AES-128 (Zigbee 4.5.? / B.6), 16 byte digest. */
void zb_mmo_hash(const uint8_t *msg, size_t len, uint8_t out[16]);

/* The keyed hash of the Zigbee spec (HMAC with the MMO hash), for a 16 byte key. */
void zb_keyed_hash(const uint8_t key[ZB_KEY_LEN], const uint8_t *msg, size_t len, uint8_t out[16]);

/* The key-transport key derived from a link key (keyed hash of the single byte 0x00). */
void zb_key_transport_key(const uint8_t link_key[ZB_KEY_LEN], uint8_t out[ZB_KEY_LEN]);

#endif /* ZIGBEE_ZB_CRYPTO_H_ */
