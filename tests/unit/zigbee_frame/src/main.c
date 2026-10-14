/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit test of lib/zigbee/zb_frame.c. Header layouts are scapy's (an independent implementation
 * of the same specification); the secured frames are built with Python cryptography (AES-CCM,
 * 4 byte tag) by the rules written in zb_frame.h: level 0 on air, level 5 in the nonce and in
 * the authenticated header. Generator: tmp/zstack/gen_frame_vectors.py.
 */
#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <zigbee/zb_frame.h>

ZTEST_SUITE(zigbee_frame, NULL, NULL, NULL, NULL, NULL);

ZTEST(zigbee_frame, test_nwk_header)
{
	static const uint8_t data[] = {0x08, 0x00, 0x34, 0x12, 0x00, 0x00, 0x1e, 0x07};
	static const uint8_t cmd[] = {0x49, 0x12, 0xfc, 0xff, 0xec, 0x5d, 0x01, 0xc8, 0xf1, 0xde, 0xbc, 0x2a, 0x00, 0x4b, 0x12, 0x00};
	struct zb_nwk_hdr h;
	uint8_t out[32];
	static const uint8_t ext[8] = {0xf1, 0xde, 0xbc, 0x2a, 0x00, 0x4b, 0x12, 0x00};

	zassert_ok(zb_nwk_parse(data, sizeof(data), &h));
	zassert_equal(h.type, ZB_NWK_DATA);
	zassert_equal(h.proto_ver, 2);
	zassert_false(h.security);
	zassert_equal(h.dst, 0x1234);
	zassert_equal(h.src, 0x0000);
	zassert_equal(h.radius, 30);
	zassert_equal(h.seq, 7);
	zassert_equal(h.len, 8);
	zassert_equal(zb_nwk_build(&h, out), sizeof(data));
	zassert_mem_equal(out, data, sizeof(data));

	zassert_ok(zb_nwk_parse(cmd, sizeof(cmd), &h));
	zassert_equal(h.type, ZB_NWK_COMMAND);
	zassert_equal(h.discover, 1);
	zassert_true(h.security);
	zassert_true(h.has_ext_src);
	zassert_false(h.has_ext_dst);
	zassert_equal(h.dst, 0xFFFC);
	zassert_equal(h.src, 0x5DEC);
	zassert_equal(h.seq, 200);
	zassert_mem_equal(h.ext_src, ext, 8);
	zassert_equal(h.len, 16);
	zassert_equal(zb_nwk_build(&h, out), sizeof(cmd));
	zassert_mem_equal(out, cmd, sizeof(cmd));

	zassert_equal(zb_nwk_parse(data, 7, &h), -EINVAL);
	zassert_equal(zb_nwk_parse(cmd, 12, &h), -EINVAL, "extended source cut short");
}

ZTEST(zigbee_frame, test_aps_header)
{
	static const uint8_t data[] = {0x40, 0x01, 0x06, 0x00, 0x04, 0x01, 0x02, 0x09};
	static const uint8_t ack[] = {0x02, 0x02, 0x06, 0x00, 0x04, 0x01, 0x01, 0x09};
	static const uint8_t bcast[] = {0x08, 0xff, 0x36, 0x00, 0x00, 0x00, 0x00, 0x21};
	static const uint8_t cmd[] = {0x21, 0x4d};
	struct zb_aps_hdr h;
	uint8_t out[16];

	zassert_ok(zb_aps_parse(data, sizeof(data), &h));
	zassert_equal(h.type, ZB_APS_DATA);
	zassert_equal(h.delivery, ZB_APS_UNICAST);
	zassert_true(h.ack_req);
	zassert_false(h.security);
	zassert_equal(h.dst_ep, 1);
	zassert_equal(h.cluster, 0x0006);
	zassert_equal(h.profile, 0x0104);
	zassert_equal(h.src_ep, 2);
	zassert_equal(h.counter, 9);
	zassert_equal(h.len, sizeof(data));
	zassert_equal(zb_aps_build(&h, out), sizeof(data));
	zassert_mem_equal(out, data, sizeof(data));

	zassert_ok(zb_aps_parse(ack, sizeof(ack), &h));
	zassert_equal(h.type, ZB_APS_ACK);
	zassert_equal(h.dst_ep, 2);
	zassert_equal(h.src_ep, 1);
	zassert_equal(h.counter, 9);
	zassert_equal(zb_aps_build(&h, out), sizeof(ack));
	zassert_mem_equal(out, ack, sizeof(ack));

	zassert_ok(zb_aps_parse(bcast, sizeof(bcast), &h));
	zassert_equal(h.delivery, ZB_APS_BROADCAST);
	zassert_equal(h.dst_ep, 0xFF);
	zassert_equal(h.cluster, 0x0036);
	zassert_equal(h.profile, 0);
	zassert_equal(h.counter, 33);

	zassert_ok(zb_aps_parse(cmd, sizeof(cmd), &h));
	zassert_equal(h.type, ZB_APS_COMMAND);
	zassert_true(h.security);
	zassert_equal(h.counter, 77);
	zassert_equal(h.len, 2, "a command frame has no endpoints or cluster");
	zassert_equal(zb_aps_build(&h, out), sizeof(cmd));
	zassert_mem_equal(out, cmd, sizeof(cmd));

	zassert_equal(zb_aps_parse(data, 5, &h), -EINVAL);
}

ZTEST(zigbee_frame, test_secured_nwk_frame)
{
	static const uint8_t key[] = {0xa5, 0x4d, 0xca, 0x18, 0x25, 0x30, 0xbb, 0x1d, 0x6d, 0x13, 0x2c, 0xde, 0xd6, 0x23, 0x7b, 0x2e};
	static const uint8_t hdr[] = {0x08, 0x02, 0x00, 0x00, 0xec, 0x5d, 0x1e, 0x0b};
	static const uint8_t plain[] = {0x40, 0x01, 0x06, 0x00, 0x04, 0x01, 0x01, 0x03, 0x01, 0x04, 0x00};
	static const uint8_t want[] = {0x08, 0x02, 0x00, 0x00, 0xec, 0x5d, 0x1e, 0x0b, 0x28, 0x40, 0xe2, 0x01, 0x00, 0xf1, 0xde, 0xbc, 0x2a, 0x00, 0x4b, 0x12, 0x00, 0x02, 0x12, 0xeb, 0x8e, 0xd7, 0x37, 0x62, 0x2e, 0x7e, 0x4c, 0x99, 0x11, 0x88, 0x72, 0x50, 0x7b};
	static const uint8_t src[] = {0xf1, 0xde, 0xbc, 0x2a, 0x00, 0x4b, 0x12, 0x00};
	uint8_t buf[100];
	struct zb_sec_hdr s;
	size_t n, plen;

	/* Protect: header, then the plain payload, in; header, aux, ciphertext, MIC out. */
	memcpy(buf, hdr, sizeof(hdr));
	memcpy(&buf[sizeof(hdr)], plain, sizeof(plain));
	n = zb_sec_protect(buf, sizeof(buf), sizeof(hdr), sizeof(plain), key, ZB_KEYID_NETWORK, 2,
			   0x0001E240, src);
	zassert_equal(n, sizeof(want));
	zassert_mem_equal(buf, want, sizeof(want));
	zassert_equal(buf[sizeof(hdr)], 0x28, "level 0 on air: key id 1, extended nonce");

	/* Open it again. */
	zassert_ok(zb_sec_parse(buf, n, sizeof(hdr), &s));
	zassert_equal(s.key_id, ZB_KEYID_NETWORK);
	zassert_equal(s.counter, 0x0001E240);
	zassert_equal(s.key_seq, 2);
	zassert_equal(s.len, 14);
	zassert_mem_equal(s.src, src, 8);
	zassert_ok(zb_sec_open(buf, n, sizeof(hdr), key, &s, &plen));
	zassert_equal(plen, sizeof(plain));
	zassert_mem_equal(&buf[sizeof(hdr) + s.len], plain, sizeof(plain));

	/* A flipped header bit, payload bit or a wrong key fail the MIC. */
	for (int where = 0; where < 3; where++) {
		memcpy(buf, want, sizeof(want));
		if (where == 0) {
			buf[2] ^= 1; /* destination in the NWK header (authenticated) */
		} else if (where == 1) {
			buf[sizeof(hdr) + 14] ^= 0x10; /* ciphertext */
		}
		zassert_ok(zb_sec_parse(buf, n, sizeof(hdr), &s));
		zassert_equal(zb_sec_open(buf, n, sizeof(hdr), where == 2 ? src : key, &s, &plen), -EBADMSG,
			      "case %d", where);
	}
	/* Not enough room. */
	memcpy(buf, hdr, sizeof(hdr));
	zassert_equal(zb_sec_protect(buf, sizeof(hdr) + 10, sizeof(hdr), sizeof(plain), key,
				     ZB_KEYID_NETWORK, 2, 1, src), 0);
}

ZTEST(zigbee_frame, test_transport_key_frame_for_the_default_link_key)
{
	static const uint8_t link[] = {0x5a, 0x69, 0x67, 0x42, 0x65, 0x65, 0x41, 0x6c, 0x6c, 0x69, 0x61, 0x6e, 0x63, 0x65, 0x30, 0x39};
	static const uint8_t tk_key[] = {0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf};
	static const uint8_t joiner[] = {0xf1, 0xde, 0xbc, 0x2a, 0x00, 0x4b, 0x12, 0x00};
	static const uint8_t tc[] = {0xf0, 0x91, 0x82, 0xfe, 0xff, 0x16, 0xa3, 0x98};
	static const uint8_t nwk_tk[] = {0x08, 0x00, 0xec, 0x5d, 0x00, 0x00, 0x01, 0x01};
	static const uint8_t want[] = {0x08, 0x00, 0xec, 0x5d, 0x00, 0x00, 0x01, 0x01, 0x21, 0x2a, 0x30, 0x10, 0x00, 0x00, 0x00, 0xf0, 0x91, 0x82, 0xfe, 0xff, 0x16, 0xa3, 0x98, 0xcb, 0x94, 0xf0, 0xb0, 0xa3, 0xf3, 0x36, 0xea, 0xbd, 0x0e, 0x8b, 0x15, 0xba, 0xce, 0x56, 0x64, 0x80, 0xf4, 0xd6, 0x65, 0xda, 0x2b, 0x05, 0xfc, 0x59, 0xc0, 0xf5, 0xaa, 0xf7, 0x1d, 0x9e, 0x51, 0x2b, 0x63, 0x84, 0xf0, 0xf0, 0xcc, 0xbf};
	uint8_t buf[100], ktk[16], payload[35];
	size_t n = sizeof(nwk_tk);

	zb_key_transport_key(link, ktk);
	memcpy(buf, nwk_tk, sizeof(nwk_tk));
	buf[n++] = 0x21; /* APS command frame, secured */
	buf[n++] = 42;   /* APS counter */
	zassert_equal(zb_aps_transport_key_payload(payload, tk_key, 0, joiner, tc), 35);
	memcpy(&buf[n], payload, 35);
	n = sizeof(nwk_tk) + zb_sec_protect(&buf[sizeof(nwk_tk)], sizeof(buf) - sizeof(nwk_tk), 2, 35, ktk,
					    ZB_KEYID_TRANSPORT, 0, 0x10, tc);
	zassert_equal(n, sizeof(want));
	zassert_mem_equal(buf, want, sizeof(want));
	zassert_equal(buf[sizeof(nwk_tk) + 2], 0x30, "key-transport key id, extended nonce");
}
