/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit tests of the Zigbee MAC: the frame codec against the verified examples of
 * tmp/zstack/phase2/mac_final.md (every frame with its FCS, CRC-16/KERMIT), then the MAC state
 * machine against a mock radio.
 */
#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <zigbee/mt.h>
#include <zigbee/zb_mac_frame.h>

/* CRC-16/KERMIT, the 802.15.4 FCS (poly 0x8408 reflected, init 0), sent low byte first. */
static uint16_t fcs(const uint8_t *p, size_t n)
{
	uint16_t crc = 0;

	while (n-- > 0U) {
		crc ^= *p++;
		for (int i = 0; i < 8; i++) {
			crc = (crc & 1U) ? (crc >> 1) ^ 0x8408U : (crc >> 1);
		}
	}
	return crc;
}

/* Frame (no FCS) plus its FCS must equal the example hex. */
#define CHECK_FRAME(buf, n, ...)                                                                   \
	do {                                                                                       \
		static const uint8_t want[] = {__VA_ARGS__};                                       \
		uint8_t got[ZB_MAC_MAX_FRAME + 2];                                                 \
		uint16_t c = fcs(buf, n);                                                          \
                                                                                                   \
		memcpy(got, buf, n);                                                               \
		got[n] = (uint8_t)c;                                                               \
		got[n + 1] = (uint8_t)(c >> 8);                                                    \
		zassert_equal((n) + 2U, sizeof(want), "length %zu vs %zu", (size_t)(n) + 2U,       \
			      sizeof(want));                                                       \
		zassert_mem_equal(got, want, sizeof(want));                                        \
	} while (0)

ZTEST_SUITE(zigbee_frame, NULL, NULL, NULL, NULL, NULL);

static const uint8_t JOINER[8] = {0xF1, 0xDE, 0xBC, 0x2A, 0x00, 0x4B, 0x12, 0x00};
static const uint8_t COORD[8] = {0x21, 0x6A, 0x3F, 0x1D, 0x00, 0x4B, 0x12, 0x00};
static const uint8_t EPID[8] = {0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
#define PAN 0x1A62

static struct zb_beacon_info beacon(bool permit, bool router, bool ed)
{
	struct zb_beacon_info b = {
		.pan = PAN,
		.src_short = 0,
		.permit = permit,
		.pan_coord = true,
		.stack_profile = 2,
		.proto_ver = 2,
		.router_cap = router,
		.ed_cap = ed,
		.depth = 0,
		.upd = 0,
	};

	memcpy(b.epid, EPID, 8);
	return b;
}

ZTEST(zigbee_frame, test_checksum_helper_matches_known_ack)
{
	/* mac_final 4.1: ACK DSN 0x22 */
	static const uint8_t ack[] = {0x02, 0x00, 0x22};

	CHECK_FRAME(ack, sizeof(ack), 0x02, 0x00, 0x22, 0xA8, 0xB7);
}

ZTEST(zigbee_frame, test_beacon_request)
{
	uint8_t f[ZB_MAC_MAX_FRAME];
	size_t n = zb_mac_build_beacon_req(f, 0x5A);

	CHECK_FRAME(f, n, 0x03, 0x08, 0x5A, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x57, 0x40);
}

ZTEST(zigbee_frame, test_beacons)
{
	uint8_t f[ZB_MAC_MAX_FRAME];
	struct zb_beacon_info b = beacon(true, true, true);
	size_t n = zb_mac_build_beacon(f, 0x10, &b);

	CHECK_FRAME(f, n, 0x00, 0x80, 0x10, 0x62, 0x1A, 0x00, 0x00, 0xFF, 0xCF, 0x00, 0x00, 0x00,
		    0x22, 0x84, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xFF, 0xFF, 0xFF,
		    0x00, 0x31, 0x75);

	b = beacon(false, true, true);
	n = zb_mac_build_beacon(f, 0x11, &b);
	CHECK_FRAME(f, n, 0x00, 0x80, 0x11, 0x62, 0x1A, 0x00, 0x00, 0xFF, 0x4F, 0x00, 0x00, 0x00,
		    0x22, 0x84, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xFF, 0xFF, 0xFF,
		    0x00, 0x7A, 0xA2);

	b = beacon(true, false, true);
	n = zb_mac_build_beacon(f, 0x12, &b);
	CHECK_FRAME(f, n, 0x00, 0x80, 0x12, 0x62, 0x1A, 0x00, 0x00, 0xFF, 0xCF, 0x00, 0x00, 0x00,
		    0x22, 0x80, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xFF, 0xFF, 0xFF,
		    0x00, 0x0D, 0xD5);

	b = beacon(false, false, false);
	n = zb_mac_build_beacon(f, 0x13, &b);
	CHECK_FRAME(f, n, 0x00, 0x80, 0x13, 0x62, 0x1A, 0x00, 0x00, 0xFF, 0x4F, 0x00, 0x00, 0x00,
		    0x22, 0x00, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xFF, 0xFF, 0xFF,
		    0x00, 0xE4, 0xC4);
}

ZTEST(zigbee_frame, test_beacon_roundtrip)
{
	uint8_t f[ZB_MAC_MAX_FRAME];
	struct zb_beacon_info b = beacon(true, true, false), r;
	struct zb_mac_hdr h;
	size_t n;

	b.upd = 7;
	b.depth = 3;
	n = zb_mac_build_beacon(f, 1, &b);
	zassert_ok(zb_mac_parse(f, n, &h));
	zassert_equal(h.type, ZB_FT_BEACON);
	zassert_equal(h.src_mode, ZB_ADDR_SHORT);
	zassert_equal(h.src_pan, PAN);
	zassert_ok(zb_mac_parse_beacon(&h, &f[h.len], n - h.len, &r));
	zassert_equal(r.pan, PAN);
	zassert_true(r.permit);
	zassert_true(r.pan_coord);
	zassert_true(r.router_cap);
	zassert_false(r.ed_cap);
	zassert_equal(r.depth, 3);
	zassert_equal(r.upd, 7);
	zassert_equal(r.stack_profile, 2);
	zassert_equal(r.proto_ver, 2);
	zassert_mem_equal(r.epid, EPID, 8);
}

ZTEST(zigbee_frame, test_association)
{
	uint8_t f[ZB_MAC_MAX_FRAME];
	size_t n = zb_mac_build_assoc_req(f, 0x21, PAN, 0x0000, JOINER, 0x8E);

	CHECK_FRAME(f, n, 0x23, 0xC8, 0x21, 0x62, 0x1A, 0x00, 0x00, 0xFF, 0xFF, 0xF1, 0xDE, 0xBC,
		    0x2A, 0x00, 0x4B, 0x12, 0x00, 0x01, 0x8E, 0x58, 0xF8);

	n = zb_mac_build_assoc_rsp(f, 0x11, PAN, JOINER, COORD, 0x7F3C, ZB_ASSOC_OK);
	CHECK_FRAME(f, n, 0x63, 0xCC, 0x11, 0x62, 0x1A, 0xF1, 0xDE, 0xBC, 0x2A, 0x00, 0x4B, 0x12,
		    0x00, 0x21, 0x6A, 0x3F, 0x1D, 0x00, 0x4B, 0x12, 0x00, 0x02, 0x3C, 0x7F, 0x00,
		    0x73, 0xF0);
	n = zb_mac_build_assoc_rsp(f, 0x12, PAN, JOINER, COORD, 0xFFFF, ZB_ASSOC_PAN_FULL);
	CHECK_FRAME(f, n, 0x63, 0xCC, 0x12, 0x62, 0x1A, 0xF1, 0xDE, 0xBC, 0x2A, 0x00, 0x4B, 0x12,
		    0x00, 0x21, 0x6A, 0x3F, 0x1D, 0x00, 0x4B, 0x12, 0x00, 0x02, 0xFF, 0xFF, 0x01,
		    0x82, 0x1A);
	n = zb_mac_build_assoc_rsp(f, 0x13, PAN, JOINER, COORD, 0xFFFF, ZB_ASSOC_DENIED);
	CHECK_FRAME(f, n, 0x63, 0xCC, 0x13, 0x62, 0x1A, 0xF1, 0xDE, 0xBC, 0x2A, 0x00, 0x4B, 0x12,
		    0x00, 0x21, 0x6A, 0x3F, 0x1D, 0x00, 0x4B, 0x12, 0x00, 0x02, 0xFF, 0xFF, 0x02,
		    0xD0, 0xA1);
}

ZTEST(zigbee_frame, test_data_request_and_data)
{
	uint8_t f[ZB_MAC_MAX_FRAME];
	uint16_t sh = 0x7F3C;
	static const uint8_t pl[] = {0xAA, 0xBB, 0xCC};
	size_t n = zb_mac_build_data_req(f, 0x22, PAN, 0x0000, JOINER, NULL);

	CHECK_FRAME(f, n, 0x63, 0xC8, 0x22, 0x62, 0x1A, 0x00, 0x00, 0xF1, 0xDE, 0xBC, 0x2A, 0x00,
		    0x4B, 0x12, 0x00, 0x04, 0x52, 0x65);
	n = zb_mac_build_data_req(f, 0x23, PAN, 0x0000, NULL, &sh);
	CHECK_FRAME(f, n, 0x63, 0x88, 0x23, 0x62, 0x1A, 0x00, 0x00, 0x3C, 0x7F, 0x04, 0x3F, 0xE0);

	n = zb_mac_build_empty_data(f, 0x14, PAN, &sh, NULL, 0x0000);
	CHECK_FRAME(f, n, 0x61, 0x88, 0x14, 0x62, 0x1A, 0x3C, 0x7F, 0x00, 0x00, 0x90, 0x06);
	n = zb_mac_build_empty_data(f, 0x15, PAN, NULL, JOINER, 0x0000);
	CHECK_FRAME(f, n, 0x61, 0x8C, 0x15, 0x62, 0x1A, 0xF1, 0xDE, 0xBC, 0x2A, 0x00, 0x4B, 0x12,
		    0x00, 0x00, 0x00, 0x57, 0x19);

	n = zb_mac_build_data(f, 0x05, PAN, 0x7F3C, 0x0000, true, pl, sizeof(pl));
	CHECK_FRAME(f, n, 0x61, 0x88, 0x05, 0x62, 0x1A, 0x3C, 0x7F, 0x00, 0x00, 0xAA, 0xBB, 0xCC,
		    0xD7, 0x8D);
	n = zb_mac_build_data(f, 0x06, PAN, 0xFFFF, 0x0000, false, pl, sizeof(pl));
	CHECK_FRAME(f, n, 0x41, 0x88, 0x06, 0x62, 0x1A, 0xFF, 0xFF, 0x00, 0x00, 0xAA, 0xBB, 0xCC,
		    0xD9, 0x3E);
}

ZTEST(zigbee_frame, test_parse_addressing)
{
	struct zb_mac_hdr h;
	uint8_t f[ZB_MAC_MAX_FRAME];
	size_t n = zb_mac_build_assoc_req(f, 0x21, PAN, 0x0000, JOINER, 0x8E);

	zassert_ok(zb_mac_parse(f, n, &h));
	zassert_equal(h.type, ZB_FT_CMD);
	zassert_true(h.ar);
	zassert_false(h.comp);
	zassert_equal(h.dst_mode, ZB_ADDR_SHORT);
	zassert_equal(h.src_mode, ZB_ADDR_EXT);
	zassert_equal(h.dst_pan, PAN);
	zassert_equal(h.dst_short, 0);
	zassert_equal(h.src_pan, 0xFFFF);
	zassert_mem_equal(h.src_ext, JOINER, 8);
	zassert_equal(h.len, 17);
	zassert_equal(f[h.len], ZB_CMD_ASSOC_REQ);
	zassert_equal(f[h.len + 1], 0x8E);

	/* Compression: the source PAN is the destination PAN. */
	n = zb_mac_build_data_req(f, 0x22, PAN, 0x0000, JOINER, NULL);
	zassert_ok(zb_mac_parse(f, n, &h));
	zassert_true(h.comp);
	zassert_equal(h.src_pan, PAN);
	zassert_equal(h.len, 15);

	/* Both orphan notification forms (with and without the source PAN). */
	static const uint8_t orphan_a[] = {0x03, 0xC8, 0x24, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
					   0xF1, 0xDE, 0xBC, 0x2A, 0x00, 0x4B, 0x12, 0x00, 0x06};
	static const uint8_t orphan_b[] = {0x43, 0xC8, 0x24, 0xFF, 0xFF, 0xFF, 0xFF, 0xF1, 0xDE,
					   0xBC, 0x2A, 0x00, 0x4B, 0x12, 0x00, 0x06};

	zassert_ok(zb_mac_parse(orphan_a, sizeof(orphan_a), &h));
	zassert_mem_equal(h.src_ext, JOINER, 8);
	zassert_equal(orphan_a[h.len], ZB_CMD_ORPHAN);
	zassert_ok(zb_mac_parse(orphan_b, sizeof(orphan_b), &h));
	zassert_mem_equal(h.src_ext, JOINER, 8);
	zassert_equal(orphan_b[h.len], ZB_CMD_ORPHAN);
}

ZTEST(zigbee_frame, test_parse_rejects)
{
	struct zb_mac_hdr h;
	static const uint8_t ver2[] = {0x41, 0xA8, 0x00, 0x62, 0x1A, 0x00, 0x00, 0x01, 0x00};
	static const uint8_t sec[] = {0x49, 0x88, 0x00, 0x62, 0x1A, 0x00, 0x00, 0x01, 0x00};
	static const uint8_t mode1[] = {0x41, 0x84, 0x00, 0x62, 0x1A, 0x00, 0x00};
	static const uint8_t comp_nodst[] = {0x40, 0x80, 0x00, 0x62, 0x1A, 0x00, 0x00};
	static const uint8_t trunc[] = {0x61, 0x88, 0x05, 0x62, 0x1A, 0x3C};

	zassert_equal(zb_mac_parse(ver2, sizeof(ver2), &h), -ENOTSUP);
	zassert_equal(zb_mac_parse(sec, sizeof(sec), &h), -ENOTSUP);
	zassert_equal(zb_mac_parse(mode1, sizeof(mode1), &h), -EINVAL);
	zassert_equal(zb_mac_parse(comp_nodst, sizeof(comp_nodst), &h), -EINVAL);
	zassert_equal(zb_mac_parse(trunc, sizeof(trunc), &h), -EINVAL);
	zassert_equal(zb_mac_parse(trunc, 2, &h), -EINVAL);
}

ZTEST(zigbee_frame, test_parse_ack_and_real_world_frames)
{
	struct zb_mac_hdr h;
	static const uint8_t ack[] = {0x02, 0x00, 0x41};
	/* Frames captured off the air (tmp/zstack/sniff, FCS removed by wpan): a data request of a
	 * router (10 bytes), a NWK link status of PAN f630.
	 */
	static const uint8_t datareq[] = {0x63, 0x88, 0x78, 0x30, 0xF6, 0x00, 0x00, 0xC8, 0x4F, 0x04};

	zassert_ok(zb_mac_parse(ack, sizeof(ack), &h));
	zassert_equal(h.type, ZB_FT_ACK);
	zassert_equal(h.seq, 0x41);
	zassert_equal(h.len, 3);

	zassert_ok(zb_mac_parse(datareq, sizeof(datareq), &h));
	zassert_equal(h.type, ZB_FT_CMD);
	zassert_true(h.ar);
	zassert_true(h.comp);
	zassert_equal(h.dst_pan, 0xF630);
	zassert_equal(h.dst_short, 0x0000);
	zassert_equal(h.src_short, 0x4FC8);
	zassert_equal(datareq[h.len], ZB_CMD_DATA_REQ);
}

/* ------------------------------------------------------------------------------------------ */
/* MAC state machine against a mock radio. */

#include <zigbee/zb_mac.h>

#include "mock_radio.h"

static struct zb_mac mac;
static K_THREAD_STACK_DEFINE(mac_stack, 4096);
static bool mac_running;

struct up_log {
	int assoc_calls;
	uint8_t last_ext[8];
	uint8_t last_cap;
	uint8_t verdict;
	uint16_t grant;
	int done_ok, done_fail;
	int data_calls;
	uint16_t data_src;
	uint8_t data_payload[16];
	size_t data_len;
};

static struct up_log up;

static uint8_t up_assoc_req(void *ctx, const uint8_t ext[8], uint8_t cap, uint16_t *short_addr)
{
	up.assoc_calls++;
	memcpy(up.last_ext, ext, 8);
	up.last_cap = cap;
	*short_addr = up.grant;
	return up.verdict;
}

static void up_assoc_done(void *ctx, const uint8_t ext[8], uint16_t short_addr, bool delivered)
{
	delivered ? up.done_ok++ : up.done_fail++;
}

static void up_data_ind(void *ctx, const struct zb_mac_hdr *h, const uint8_t *p, size_t n,
			uint8_t lqi, int8_t rssi)
{
	up.data_calls++;
	up.data_src = h->src_short;
	up.data_len = MIN(n, sizeof(up.data_payload));
	memcpy(up.data_payload, p, up.data_len);
}

static void *mac_setup(void)
{
	static const struct zb_mac_up ups = {
		.assoc_req = up_assoc_req, .assoc_done = up_assoc_done, .data_ind = up_data_ind,
	};

	memset(&mr, 0, sizeof(mr));
	k_sem_init(&mr.tx_sem, 0, 64);
	zb_mac_init(&mac, &mock_ops, NULL, &ups, COORD);
	mr.mac = &mac;
	zb_mac_run(&mac, mac_stack, K_THREAD_STACK_SIZEOF(mac_stack), K_PRIO_PREEMPT(4));
	mac_running = true;
	return NULL;
}

/* Fresh radio log and callbacks, MAC stopped, before each test. */
static void mac_before(void *f)
{
	(void)zb_mac_stop(&mac);
	k_sem_reset(&mr.tx_sem);
	mr.tx_count = 0;
	mr.script_n = mr.script_i = 0;
	mr.nnets = 0;
	mr.ed_calls = 0;
	memset(mr.ed_dbm, 0, sizeof(mr.ed_dbm));
	memset(&up, 0, sizeof(up));
	up.verdict = ZB_ASSOC_OK;
	up.grant = 0x7F3C;
}

ZTEST_SUITE(zigbee_mac, NULL, mac_setup, mac_before, NULL, NULL);

static struct zb_beacon_info my_beacon(void)
{
	return beacon(false, true, true);
}

static void start_net(void)
{
	struct zb_beacon_info b = my_beacon();

	zassert_ok(zb_mac_start(&mac, PAN, 15, &b));
}

static void inject(const uint8_t *f, size_t n)
{
	zb_mac_rx(&mac, f, (uint8_t)n, 220, -50);
}

/* Wait until the mock saw @p n transmissions. */
static void wait_tx(int n)
{
	for (int i = 0; i < n; i++) {
		zassert_ok(k_sem_take(&mr.tx_sem, K_SECONDS(2)), "transmission %d never came", i);
	}
}

ZTEST(zigbee_mac, test_start_programs_the_radio)
{
	start_net();
	zassert_equal(mr.pan, PAN);
	zassert_equal(mr.short_addr, 0x0000);
	zassert_mem_equal(mr.ext, COORD, 8);
	zassert_true(mr.coordinator);
	zassert_false(mr.promisc);
	zassert_equal(mr.chan, 15);
	zassert_true(mr.starts >= 1);
}

ZTEST(zigbee_mac, test_beacon_request_is_answered_closed_then_open)
{
	uint8_t req[ZB_MAC_MAX_FRAME], f[ZB_MAC_MAX_FRAME];
	struct zb_mac_hdr h;
	struct zb_beacon_info b;
	size_t n = zb_mac_build_beacon_req(req, 0x5A);

	start_net();
	inject(req, n);
	wait_tx(1);
	zassert_equal(mr.tx_count, 1);
	memcpy(f, mr.tx_log[0].f, mr.tx_log[0].n);
	zassert_ok(zb_mac_parse(f, mr.tx_log[0].n, &h));
	zassert_equal(h.type, ZB_FT_BEACON);
	zassert_false(h.ar);
	zassert_ok(zb_mac_parse_beacon(&h, &f[h.len], mr.tx_log[0].n - h.len, &b));
	zassert_equal(b.pan, PAN);
	zassert_false(b.permit, "closed unless the host opens joining");
	zassert_true(b.router_cap);
	zassert_true(b.ed_cap);
	zassert_mem_equal(b.epid, EPID, 8, "the beacon carries the template's extended PAN ID");
	zassert_true(mr.tx_log[0].csma);

	zassert_ok(zb_mac_permit(&mac, 0xFF));
	zassert_true(zb_mac_permit_open(&mac));
	inject(req, n);
	wait_tx(1);
	zassert_equal(mr.tx_count, 2);
	zassert_ok(zb_mac_parse(mr.tx_log[1].f, mr.tx_log[1].n, &h));
	zassert_ok(zb_mac_parse_beacon(&h, &mr.tx_log[1].f[h.len], mr.tx_log[1].n - h.len, &b));
	zassert_true(b.permit);
	/* The beacon sequence number runs on. */
	zassert_equal(mr.tx_log[1].f[2], (uint8_t)(mr.tx_log[0].f[2] + 1));
}

ZTEST(zigbee_mac, test_permit_times_out)
{
	zassert_false(zb_mac_permit_open(&mac));
	start_net();
	zassert_ok(zb_mac_permit(&mac, 5));
	zassert_true(zb_mac_permit_open(&mac));
	k_sleep(K_SECONDS(4));
	zassert_true(zb_mac_permit_open(&mac));
	k_sleep(K_SECONDS(2));
	zassert_false(zb_mac_permit_open(&mac));
	zassert_ok(zb_mac_permit(&mac, 0));
	zassert_false(zb_mac_permit_open(&mac));
}

ZTEST(zigbee_mac, test_not_started_stays_silent)
{
	uint8_t req[ZB_MAC_MAX_FRAME];
	size_t n = zb_mac_build_beacon_req(req, 1);

	inject(req, n);
	k_sleep(K_MSEC(100));
	zassert_equal(mr.tx_count, 0);
}

ZTEST(zigbee_mac, test_association_handshake)
{
	uint8_t f[ZB_MAC_MAX_FRAME];
	struct zb_mac_hdr h;
	size_t n;

	start_net();
	zassert_ok(zb_mac_permit(&mac, 0xFF));

	n = zb_mac_build_assoc_req(f, 0x21, PAN, 0x0000, JOINER, 0x8E);
	inject(f, n);
	k_sleep(K_MSEC(50));
	zassert_equal(up.assoc_calls, 1);
	zassert_mem_equal(up.last_ext, JOINER, 8);
	zassert_equal(up.last_cap, 0x8E);
	/* The response waits for the joiner's data request: nothing on air yet. */
	zassert_equal(mr.tx_count, 0);

	/* The joiner polls with its extended address. */
	n = zb_mac_build_data_req(f, 0x22, PAN, 0x0000, JOINER, NULL);
	inject(f, n);
	wait_tx(1);
	zassert_equal(mr.tx_count, 1);
	zassert_ok(zb_mac_parse(mr.tx_log[0].f, mr.tx_log[0].n, &h));
	zassert_equal(h.type, ZB_FT_CMD);
	zassert_true(h.ar);
	zassert_mem_equal(h.dst_ext, JOINER, 8);
	zassert_mem_equal(h.src_ext, COORD, 8);
	zassert_equal(mr.tx_log[0].f[h.len], ZB_CMD_ASSOC_RSP);
	zassert_equal(mt_get_u16(&mr.tx_log[0].f[h.len + 1]), 0x7F3C);
	zassert_equal(mr.tx_log[0].f[h.len + 3], ZB_ASSOC_OK);
	k_sleep(K_MSEC(20));
	zassert_equal(up.done_ok, 1);

	/* A second poll finds nothing: the empty data frame (to the extended address). */
	inject(f, n);
	wait_tx(1);
	zassert_equal(mr.tx_count, 2);
	zassert_ok(zb_mac_parse(mr.tx_log[1].f, mr.tx_log[1].n, &h));
	zassert_equal(h.type, ZB_FT_DATA);
	zassert_equal(mr.tx_log[1].n, h.len);
	zassert_mem_equal(h.dst_ext, JOINER, 8);
}

ZTEST(zigbee_mac, test_association_refused_when_closed_or_full)
{
	uint8_t f[ZB_MAC_MAX_FRAME];
	size_t n = zb_mac_build_assoc_req(f, 0x21, PAN, 0x0000, JOINER, 0x8E);
	uint8_t poll[ZB_MAC_MAX_FRAME];
	size_t pn = zb_mac_build_data_req(poll, 0x22, PAN, 0x0000, JOINER, NULL);

	start_net();
	/* Joining closed: the layer above is not asked, the answer is "access denied". */
	inject(f, n);
	k_sleep(K_MSEC(50));
	zassert_equal(up.assoc_calls, 0);
	inject(poll, pn);
	wait_tx(1);
	zassert_equal(mr.tx_log[0].f[mr.tx_log[0].n - 1], ZB_ASSOC_DENIED);
	zassert_equal(mt_get_u16(&mr.tx_log[0].f[mr.tx_log[0].n - 3]), 0xFFFF);

	/* Open but the layer above says the PAN is full. */
	zassert_ok(zb_mac_permit(&mac, 0xFF));
	up.verdict = ZB_ASSOC_PAN_FULL;
	inject(f, n);
	k_sleep(K_MSEC(50));
	zassert_equal(up.assoc_calls, 1);
	inject(poll, pn);
	wait_tx(1);
	zassert_equal(mr.tx_log[1].f[mr.tx_log[1].n - 1], ZB_ASSOC_PAN_FULL);
}

ZTEST(zigbee_mac, test_unclaimed_response_expires)
{
	uint8_t f[ZB_MAC_MAX_FRAME];
	size_t n = zb_mac_build_assoc_req(f, 0x21, PAN, 0x0000, JOINER, 0x8E);

	start_net();
	zassert_ok(zb_mac_permit(&mac, 0xFF));
	inject(f, n);
	k_sleep(K_MSEC(7000));
	zassert_equal(up.done_fail, 0);
	k_sleep(K_MSEC(1500));
	zassert_equal(up.done_fail, 1, "macTransactionPersistenceTime is 7.68 s");
	zassert_equal(mr.tx_count, 0);
}

ZTEST(zigbee_mac, test_poll_with_short_address_gets_empty_frame)
{
	uint8_t f[ZB_MAC_MAX_FRAME];
	uint16_t sh = 0x7F3C;
	struct zb_mac_hdr h;
	size_t n = zb_mac_build_data_req(f, 0x23, PAN, 0x0000, NULL, &sh);

	start_net();
	inject(f, n);
	wait_tx(1);
	zassert_ok(zb_mac_parse(mr.tx_log[0].f, mr.tx_log[0].n, &h));
	zassert_equal(h.type, ZB_FT_DATA);
	zassert_equal(h.dst_short, 0x7F3C);
	zassert_equal(h.src_short, 0x0000);
	zassert_true(h.ar);
	zassert_equal(mr.tx_log[0].n, 9, "zero length payload");
}

ZTEST(zigbee_mac, test_retries_follow_the_csma_and_ack_rules)
{
	static const uint8_t pl[] = {1, 2, 3};

	start_net();
	/* Channel busy twice, no ACK once, then through: four transmissions of one frame. */
	mr.script[0] = -EBUSY;
	mr.script[1] = -EBUSY;
	mr.script[2] = -ENOMSG;
	mr.script_n = 3;
	zassert_ok(zb_mac_send_data(&mac, 0x7F3C, pl, sizeof(pl), true));
	zassert_equal(mr.tx_count, 4);
	/* Retransmissions reuse the sequence number. */
	zassert_equal(mr.tx_log[0].f[2], mr.tx_log[3].f[2]);

	/* Never an ACK: gives up after the first transmission plus three retries. */
	mr.tx_count = 0;
	mr.script_i = 0;
	for (int i = 0; i < 8; i++) {
		mr.script[i] = -ENOMSG;
	}
	mr.script_n = 8;
	zassert_equal(zb_mac_send_data(&mac, 0x7F3C, pl, sizeof(pl), true), -ENOMSG);
	zassert_equal(mr.tx_count, 4);

	/* Channel always busy: channel access failure after macMaxCSMABackoffs + 1 tries. */
	mr.tx_count = 0;
	mr.script_i = 0;
	for (int i = 0; i < 8; i++) {
		mr.script[i] = -EBUSY;
	}
	zassert_equal(zb_mac_send_data(&mac, 0x7F3C, pl, sizeof(pl), true), -EAGAIN);
	zassert_equal(mr.tx_count, 5);

	/* A broadcast needs no ACK, so no ACK retries. */
	mr.tx_count = 0;
	mr.script_i = 0;
	mr.script[0] = -ENOMSG;
	mr.script_n = 1;
	zassert_equal(zb_mac_send_data(&mac, 0xFFFF, pl, sizeof(pl), false), -ENOMSG);
	zassert_equal(mr.tx_count, 1);
}

ZTEST(zigbee_mac, test_data_frames_reach_the_layer_above)
{
	uint8_t f[ZB_MAC_MAX_FRAME];
	static const uint8_t pl[] = {0xDE, 0xAD};
	size_t n = zb_mac_build_data(f, 9, PAN, 0x0000, 0x7F3C, true, pl, sizeof(pl));

	start_net();
	inject(f, n);
	k_sleep(K_MSEC(50));
	zassert_equal(up.data_calls, 1);
	zassert_equal(up.data_src, 0x7F3C);
	zassert_equal(up.data_len, 2);
	zassert_mem_equal(up.data_payload, pl, 2);

	/* A frame for another PAN is not ours. */
	n = zb_mac_build_data(f, 10, 0x1111, 0x0000, 0x7F3C, true, pl, sizeof(pl));
	inject(f, n);
	k_sleep(K_MSEC(50));
	zassert_equal(up.data_calls, 1);
}

ZTEST(zigbee_mac, test_energy_scan)
{
	uint8_t ed[3];

	mr.ed_dbm[11] = -95;
	mr.ed_dbm[15] = -75;
	mr.ed_dbm[25] = -31;
	start_net();
	zassert_ok(zb_mac_energy_scan(&mac, BIT(11) | BIT(15) | BIT(25), 3, ed));
	zassert_equal(ed[0], 0);
	zassert_equal(ed[1], 60);
	zassert_equal(ed[2], 192);
	/* 138.24 ms in windows of at most 16 ms, 9 per channel. */
	zassert_equal(mr.ed_calls, 27);
	/* The radio is back on the operating channel. */
	zassert_equal(mr.chan, 15);
	zassert_true(mr.coordinator);

	mr.ed_dbm[20] = 0;
	zassert_ok(zb_mac_energy_scan(&mac, BIT(20), 0, ed));
	zassert_equal(ed[0], 255, "clamped");
}

ZTEST(zigbee_mac, test_active_scan_finds_the_networks)
{
	struct zb_beacon_info found[8];
	size_t count = 0;

	mr.nets[0] = (struct mock_net){.channel = 15, .pan = 0xF74B, .permit = false};
	mr.nets[1] = (struct mock_net){.channel = 25, .pan = 0xC2BD, .permit = true};
	mr.nnets = 2;

	zassert_ok(zb_mac_active_scan(&mac, BIT(11) | BIT(15) | BIT(25), 2, found, 8, &count));
	zassert_equal(count, 2);
	zassert_equal(found[0].channel, 15);
	zassert_equal(found[0].pan, 0xF74B);
	zassert_false(found[0].permit);
	zassert_equal(found[1].channel, 25);
	zassert_equal(found[1].pan, 0xC2BD);
	zassert_true(found[1].permit);
	zassert_true(found[1].router_cap);
	zassert_equal(found[1].upd, 3);
	zassert_equal(found[1].lqi, 190);

	/* One beacon request per channel, sent in promiscuous mode with no ACK request. */
	zassert_equal(mr.tx_count, 3);
	for (int i = 0; i < 3; i++) {
		zassert_true(mr.tx_log[i].promisc);
		zassert_equal(mr.tx_log[i].n, 8);
		zassert_equal(mr.tx_log[i].f[7], ZB_CMD_BEACON_REQ);
	}
	zassert_equal(mr.tx_log[0].chan, 11);
	zassert_equal(mr.tx_log[1].chan, 15);
	zassert_equal(mr.tx_log[2].chan, 25);

	/* Back to a normal, not promiscuous radio. */
	zassert_false(mr.promisc);
	zassert_equal(mr.pan, 0xFFFF);
}

ZTEST(zigbee_mac, test_active_scan_keeps_a_running_network_alive)
{
	struct zb_beacon_info found[4];
	size_t count;

	start_net();
	mr.nets[0] = (struct mock_net){.channel = 20, .pan = 0x1111, .permit = true};
	mr.nnets = 1;
	zassert_ok(zb_mac_active_scan(&mac, BIT(20), 1, found, 4, &count));
	zassert_equal(count, 1);
	zassert_equal(mr.pan, PAN);
	zassert_equal(mr.short_addr, 0x0000);
	zassert_true(mr.coordinator);
	zassert_false(mr.promisc);
	zassert_equal(mr.chan, 15);
}

ZTEST(zigbee_mac, test_stop_clears_state)
{
	uint8_t req[ZB_MAC_MAX_FRAME];
	size_t n = zb_mac_build_beacon_req(req, 1);

	start_net();
	zassert_ok(zb_mac_permit(&mac, 0xFF));
	zassert_ok(zb_mac_stop(&mac));
	zassert_false(zb_mac_permit_open(&mac));
	inject(req, n);
	k_sleep(K_MSEC(50));
	zassert_equal(mr.tx_count, 0);
	zassert_equal(mr.pan, 0xFFFF);
	zassert_false(mr.coordinator);
}

ZTEST(zigbee_mac, test_capacity_and_network_ids_follow_updates)
{
	uint8_t req[ZB_MAC_MAX_FRAME];
	static const uint8_t epid2[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	struct zb_mac_hdr h;
	struct zb_beacon_info b;
	size_t n = zb_mac_build_beacon_req(req, 1);

	start_net();
	zassert_ok(zb_mac_set_capacity(&mac, false, true));
	zassert_ok(zb_mac_set_network(&mac, epid2, 9));
	inject(req, n);
	wait_tx(1);
	zassert_ok(zb_mac_parse(mr.tx_log[0].f, mr.tx_log[0].n, &h));
	zassert_ok(zb_mac_parse_beacon(&h, &mr.tx_log[0].f[h.len], mr.tx_log[0].n - h.len, &b));
	zassert_false(b.router_cap);
	zassert_true(b.ed_cap);
	zassert_equal(b.upd, 9);
	zassert_mem_equal(b.epid, epid2, 8);
}

ZTEST(zigbee_mac, test_full_pending_table_does_not_leak_a_child)
{
	uint8_t f[ZB_MAC_MAX_FRAME];
	uint8_t ext[8] = {1, 2, 3, 4, 5, 6, 7, 0};

	start_net();
	zassert_ok(zb_mac_permit(&mac, 0xFF));
	/* More joiners than response slots: only the ones that can be answered are asked about. */
	for (int i = 0; i < ZB_MAC_PENDING_MAX + 3; i++) {
		size_t n;

		ext[7] = (uint8_t)i;
		n = zb_mac_build_assoc_req(f, (uint8_t)i, PAN, 0x0000, ext, 0x8E);
		inject(f, n);
	}
	k_sleep(K_MSEC(100));
	zassert_equal(up.assoc_calls, ZB_MAC_PENDING_MAX, "no child allocated without a slot");

	/* Stopping the MAC releases the tentative ones: the layer above hears about every one. */
	zassert_ok(zb_mac_stop(&mac));
	zassert_equal(up.done_fail, ZB_MAC_PENDING_MAX);
}

ZTEST(zigbee_mac, test_frames_for_other_networks_are_ignored)
{
	uint8_t f[ZB_MAC_MAX_FRAME];
	uint16_t sh = 0x7F3C;
	size_t n;

	start_net();
	/* A data request to another PAN's coordinator, and to another short address. */
	n = zb_mac_build_data_req(f, 1, 0x4242, 0x0000, NULL, &sh);
	inject(f, n);
	n = zb_mac_build_data_req(f, 2, PAN, 0x1234, NULL, &sh);
	inject(f, n);
	k_sleep(K_MSEC(100));
	zassert_equal(mr.tx_count, 0);
	/* To this one: answered. */
	n = zb_mac_build_data_req(f, 3, PAN, 0x0000, NULL, &sh);
	inject(f, n);
	wait_tx(1);
	zassert_equal(mr.tx_count, 1);
}

ZTEST(zigbee_mac, test_scan_durations_are_bounded)
{
	struct zb_beacon_info found[2];
	size_t count;
	uint8_t ed[1];

	zassert_equal(zb_mac_energy_scan(&mac, BIT(15), ZB_MAC_SCAN_EXP_MAX + 1, ed), -EINVAL);
	zassert_equal(zb_mac_active_scan(&mac, BIT(15), 40, found, 2, &count), -EINVAL);
	zassert_equal(mr.ed_calls, 0);
	zassert_equal(mr.tx_count, 0);
}

ZTEST(zigbee_mac, test_tx_power_reaches_the_radio)
{
	start_net();
	mr.txpower_calls = 0;
	zassert_ok(zb_mac_set_txpower(&mac, 8));
	zassert_equal(mr.txpower, 8);
	/* It is part of every later setup too (a scan puts the radio back). */
	mr.txpower = 0;
	zassert_ok(zb_mac_energy_scan(&mac, BIT(15), 0, (uint8_t[1]){0}));
	zassert_equal(mr.txpower, 8);
}
