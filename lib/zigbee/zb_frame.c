/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * NWK, APS and security frames: see include/zigbee/zb_frame.h.
 */
#include <errno.h>
#include <string.h>

#include <zigbee/mt.h>
#include <zigbee/zb_frame.h>

/* Security level 5: encryption and a 4 byte MIC. */
#define SEC_LEVEL      5U
#define SEC_CTL_EXT_NONCE 0x20U

int zb_nwk_parse(const uint8_t *f, size_t n, struct zb_nwk_hdr *h)
{
	size_t o = 8;

	if (n < 8U) {
		return -EINVAL;
	}
	memset(h, 0, sizeof(*h));
	h->type = f[0] & 3U;
	h->proto_ver = (f[0] >> 2) & 15U;
	h->discover = f[0] >> 6;
	h->multicast = (f[1] & 0x01U) != 0U;
	h->security = (f[1] & 0x02U) != 0U;
	h->src_route = (f[1] & 0x04U) != 0U;
	h->has_ext_dst = (f[1] & 0x08U) != 0U;
	h->has_ext_src = (f[1] & 0x10U) != 0U;
	h->dst = mt_get_u16(&f[2]);
	h->src = mt_get_u16(&f[4]);
	h->radius = f[6];
	h->seq = f[7];
	if (h->has_ext_dst) {
		if (n < o + 8U) {
			return -EINVAL;
		}
		memcpy(h->ext_dst, &f[o], 8);
		o += 8;
	}
	if (h->has_ext_src) {
		if (n < o + 8U) {
			return -EINVAL;
		}
		memcpy(h->ext_src, &f[o], 8);
		o += 8;
	}
	if (h->multicast) {
		if (n < o + 1U) {
			return -EINVAL;
		}
		o += 1;
	}
	if (h->src_route) {
		/* relay count, relay index, relay list */
		if (n < o + 2U || n < o + 2U + 2U * f[o]) {
			return -EINVAL;
		}
		o += 2U + 2U * f[o];
	}
	h->len = (uint8_t)o;
	return 0;
}

size_t zb_nwk_build(const struct zb_nwk_hdr *h, uint8_t *out)
{
	size_t o = 8;

	out[0] = (uint8_t)((h->type & 3U) | ((h->proto_ver & 15U) << 2) | ((h->discover & 3U) << 6));
	out[1] = (uint8_t)((h->multicast ? 0x01U : 0U) | (h->security ? 0x02U : 0U) |
			   (h->has_ext_dst ? 0x08U : 0U) | (h->has_ext_src ? 0x10U : 0U));
	mt_put_u16(&out[2], h->dst);
	mt_put_u16(&out[4], h->src);
	out[6] = h->radius;
	out[7] = h->seq;
	if (h->has_ext_dst) {
		memcpy(&out[o], h->ext_dst, 8);
		o += 8;
	}
	if (h->has_ext_src) {
		memcpy(&out[o], h->ext_src, 8);
		o += 8;
	}
	return o;
}

int zb_aps_parse(const uint8_t *f, size_t n, struct zb_aps_hdr *h)
{
	size_t o = 1;

	if (n < 2U) {
		return -EINVAL;
	}
	memset(h, 0, sizeof(*h));
	h->type = f[0] & 3U;
	h->delivery = (f[0] >> 2) & 3U;
	h->ack_format = (f[0] & 0x10U) != 0U;
	h->security = (f[0] & 0x20U) != 0U;
	h->ack_req = (f[0] & 0x40U) != 0U;
	h->ext_hdr = (f[0] & 0x80U) != 0U;
	if (h->type == ZB_APS_DATA || (h->type == ZB_APS_ACK && !h->ack_format)) {
		bool dst = h->type == ZB_APS_ACK || h->delivery == ZB_APS_UNICAST ||
			   h->delivery == ZB_APS_BROADCAST;

		if (dst) {
			if (n < o + 1U) {
				return -EINVAL;
			}
			h->dst_ep = f[o++];
		}
		if (h->type == ZB_APS_DATA && h->delivery == ZB_APS_GROUP) {
			if (n < o + 2U) {
				return -EINVAL;
			}
			h->group = mt_get_u16(&f[o]);
			o += 2;
		}
		if (n < o + 5U) {
			return -EINVAL;
		}
		h->cluster = mt_get_u16(&f[o]);
		h->profile = mt_get_u16(&f[o + 2]);
		h->src_ep = f[o + 4];
		o += 5;
	}
	if (n < o + 1U) {
		return -EINVAL;
	}
	h->counter = f[o++];
	if (h->ext_hdr && (h->type == ZB_APS_DATA || h->type == ZB_APS_ACK)) {
		/* Fragmentation: not supported, the byte is skipped. */
		if (n < o + 1U) {
			return -EINVAL;
		}
		o += 1;
	}
	h->len = (uint8_t)o;
	return 0;
}

size_t zb_aps_build(const struct zb_aps_hdr *h, uint8_t *out)
{
	size_t o = 1;

	out[0] = (uint8_t)((h->type & 3U) | ((h->delivery & 3U) << 2) | (h->ack_format ? 0x10U : 0U) |
			   (h->security ? 0x20U : 0U) | (h->ack_req ? 0x40U : 0U));
	if (h->type == ZB_APS_DATA || (h->type == ZB_APS_ACK && !h->ack_format)) {
		if (h->type == ZB_APS_ACK || h->delivery == ZB_APS_UNICAST ||
		    h->delivery == ZB_APS_BROADCAST) {
			out[o++] = h->dst_ep;
		}
		if (h->type == ZB_APS_DATA && h->delivery == ZB_APS_GROUP) {
			mt_put_u16(&out[o], h->group);
			o += 2;
		}
		mt_put_u16(&out[o], h->cluster);
		mt_put_u16(&out[o + 2], h->profile);
		out[o + 4] = h->src_ep;
		o += 5;
	}
	out[o++] = h->counter;
	return o;
}

/* ------------------------------------------------------------------------------------------ */
/* Security */

/* Nonce: source address, frame counter, security control with the real level. */
static void make_nonce(uint8_t nonce[ZB_NONCE_LEN], const uint8_t src[8], uint32_t counter,
		       uint8_t ctl)
{
	memcpy(nonce, src, 8);
	mt_put_u32(&nonce[8], counter);
	nonce[12] = (uint8_t)(ctl | SEC_LEVEL);
}

size_t zb_sec_protect(uint8_t *buf, size_t cap, size_t hdr_len, size_t plen,
		      const uint8_t key[ZB_KEY_LEN], uint8_t key_id, uint8_t key_seq,
		      uint32_t counter, const uint8_t src[8])
{
	uint8_t ctl = (uint8_t)((key_id << 3) | SEC_CTL_EXT_NONCE);
	size_t alen = 1U + 4U + 8U + (key_id == ZB_KEYID_NETWORK ? 1U : 0U);
	uint8_t aad[64], nonce[ZB_NONCE_LEN], *aux = &buf[hdr_len];

	if (hdr_len + alen + plen + ZB_MIC_LEN > cap || hdr_len + alen > sizeof(aad)) {
		return 0;
	}
	memmove(&buf[hdr_len + alen], &buf[hdr_len], plen);
	aux[0] = ctl; /* level 0 on air */
	mt_put_u32(&aux[1], counter);
	memcpy(&aux[5], src, 8);
	if (key_id == ZB_KEYID_NETWORK) {
		aux[13] = key_seq;
	}
	make_nonce(nonce, src, counter, ctl);
	memcpy(aad, buf, hdr_len + alen);
	aad[hdr_len] = (uint8_t)(ctl | SEC_LEVEL);
	zb_ccm_encrypt(key, nonce, aad, hdr_len + alen, &buf[hdr_len + alen], plen,
		       &buf[hdr_len + alen + plen]);
	return hdr_len + alen + plen + ZB_MIC_LEN;
}

int zb_sec_parse(const uint8_t *f, size_t n, size_t hdr_len, struct zb_sec_hdr *s)
{
	const uint8_t *a = &f[hdr_len];
	size_t alen;

	if (n < hdr_len + 1U) {
		return -EINVAL;
	}
	memset(s, 0, sizeof(*s));
	if ((a[0] & SEC_CTL_EXT_NONCE) == 0U) {
		return -EINVAL; /* every Zigbee PRO frame carries the sender's address */
	}
	s->key_id = (a[0] >> 3) & 3U;
	alen = 1U + 4U + 8U + (s->key_id == ZB_KEYID_NETWORK ? 1U : 0U);
	if (n < hdr_len + alen + ZB_MIC_LEN) {
		return -EINVAL;
	}
	s->counter = mt_get_u32(&a[1]);
	memcpy(s->src, &a[5], 8);
	s->key_seq = s->key_id == ZB_KEYID_NETWORK ? a[13] : 0U;
	s->len = (uint8_t)alen;
	return 0;
}

int zb_sec_open(uint8_t *buf, size_t n, size_t hdr_len, const uint8_t key[ZB_KEY_LEN],
		const struct zb_sec_hdr *s, size_t *plen)
{
	uint8_t ctl = (uint8_t)((s->key_id << 3) | SEC_CTL_EXT_NONCE);
	size_t off = hdr_len + s->len, len;
	uint8_t aad[64], nonce[ZB_NONCE_LEN];

	if (n < off + ZB_MIC_LEN || off > sizeof(aad)) {
		return -EINVAL;
	}
	len = n - off - ZB_MIC_LEN;
	make_nonce(nonce, s->src, s->counter, ctl);
	memcpy(aad, buf, off);
	aad[hdr_len] = (uint8_t)(ctl | SEC_LEVEL);
	if (zb_ccm_decrypt(key, nonce, aad, off, &buf[off], len, &buf[off + len]) != 0) {
		return -EBADMSG;
	}
	*plen = len;
	return 0;
}

size_t zb_aps_transport_key_payload(uint8_t *out, const uint8_t key[ZB_KEY_LEN], uint8_t key_seq,
				    const uint8_t dst[8], const uint8_t src[8])
{
	out[0] = ZB_APSC_TRANSPORT_KEY;
	out[1] = 0x01; /* standard network key */
	memcpy(&out[2], key, ZB_KEY_LEN);
	out[18] = key_seq;
	memcpy(&out[19], dst, 8);
	memcpy(&out[27], src, 8);
	return 35;
}
