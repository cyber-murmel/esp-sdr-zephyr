/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit test of the ZNP server: MT framing with the frames a host sends first (ping,
 * version), resync after garbage, the NV commands zigpy-znp uses, RPC errors.
 */
#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <zigbee/znp.h>

static struct znp z;
static struct znp_nv nv;
static uint8_t txbuf[2048];
static size_t txlen;
static int stored, erased;

static void tx(void *user, const uint8_t *buf, size_t len)
{
	zassert_true(txlen + len <= sizeof(txbuf));
	memcpy(&txbuf[txlen], buf, len);
	txlen += len;
}

static int store(void *ctx, uint16_t slot, const struct znp_nv_key *key, const void *data,
		 uint16_t len)
{
	len == 0U ? erased++ : stored++;
	return 0;
}

static void before(void *f)
{
	struct znp_config cfg = {
		.tx = tx,
		.nv = &nv,
		.ext_addr = {1, 2, 3, 4, 5, 6, 7, 8},
	};
	struct znp_nv_backend be = {.store = store};

	znp_nv_init(&nv, &be);
	znp_init(&z, &cfg);
	txlen = 0;
	stored = erased = 0;
}

ZTEST_SUITE(zigbee_znp, NULL, NULL, before, NULL, NULL);

/* Send one SREQ and return the one frame the server answered with. */
static struct mt_frame sreq(uint8_t sub, uint8_t cmd, const void *data, uint8_t len)
{
	struct mt_frame f = {.type = MT_SREQ, .sub = sub, .cmd = cmd, .len = len};
	struct mt_parser p;
	uint8_t wire[MT_MAX_WIRE];
	size_t n;

	memcpy(f.data, data, len);
	n = mt_encode(&f, wire);
	txlen = 0;
	znp_rx(&z, wire, n);

	mt_parser_init(&p);
	for (size_t i = 0; i < txlen; i++) {
		if (mt_parser_feed(&p, txbuf[i]) == MT_FEED_FRAME) {
			zassert_equal(i + 1, txlen, "more than one frame sent");
			return p.frame;
		}
	}
	zassert_unreachable("no response frame");
	return f;
}

ZTEST(zigbee_znp, test_ping_wire_format)
{
	/* The first thing every host sends: FE 00 21 01 20. */
	static const uint8_t ping[] = {0xFE, 0x00, 0x21, 0x01, 0x20};
	static const uint8_t pong[] = {0xFE, 0x02, 0x61, 0x01, 0x41, 0x00, 0x23};

	znp_rx(&z, ping, sizeof(ping));
	zassert_equal(txlen, sizeof(pong));
	zassert_mem_equal(txbuf, pong, sizeof(pong));
}

ZTEST(zigbee_znp, test_resync)
{
	static const uint8_t in[] = {
		0x00, 0x13,			   /* noise */
		0xFE, 0x00, 0x21, 0x01, 0x20,	   /* good ping */
		0xFE, 0x00, 0x21, 0x01, 0x21,	   /* bad checksum */
		0xFE, 0xFF,			   /* length too large */
		0xFE, 0x00, 0x21, 0x01, 0x20,	   /* good ping */
	};

	znp_rx(&z, in, sizeof(in));
	zassert_equal(txlen, 14, "expected two pongs, got %zu bytes", txlen);
	zassert_true(z.parser.errors >= 2);
}

ZTEST(zigbee_znp, test_idle_reset)
{
	/* A false SOF with a long length waits for its payload; after an idle gap the
	 * transport drops it and the next ping gets through.
	 */
	static const uint8_t junk[] = {0xFE, 0x55, 0x21};
	static const uint8_t ping[] = {0xFE, 0x00, 0x21, 0x01, 0x20};

	znp_rx(&z, junk, sizeof(junk));
	znp_rx_idle(&z);
	znp_rx(&z, ping, sizeof(ping));
	zassert_equal(txlen, 7);
}

ZTEST(zigbee_znp, test_resync_overlapping_sof)
{
	/* A false frame (SOF len=0x21) swallows the real ping that follows it. */
	static const uint8_t in[] = {0xFE, 0x03, 0xFE, 0x00, 0x21, 0x01, 0x20, 0x00, 0x00};

	znp_rx(&z, in, sizeof(in));
	zassert_equal(txlen, 7);
}

ZTEST(zigbee_znp, test_version)
{
	struct mt_frame r = sreq(MT_SYS, 0x02, NULL, 0);

	zassert_equal(r.type, MT_SRSP);
	zassert_equal(r.len, 14);
	zassert_equal(r.data[0], 2, "transport revision");
}

ZTEST(zigbee_znp, test_ext_addr)
{
	static const uint8_t a[8] = {8, 7, 6, 5, 4, 3, 2, 1};
	struct mt_frame r = sreq(MT_SYS, 0x04, NULL, 0);

	zassert_mem_equal(r.data, z.cfg.ext_addr, 8);
	r = sreq(MT_SYS, 0x03, a, 8);
	zassert_equal(r.data[0], 0);
	r = sreq(MT_SYS, 0x04, NULL, 0);
	zassert_mem_equal(r.data, a, 8);
}

ZTEST(zigbee_znp, test_rpc_errors)
{
	/* Subsystem without handler: FE 03 60 00 01 22 00. */
	struct mt_frame r = sreq(MT_MAC, 0x00, NULL, 0);

	zassert_equal(r.type, MT_SRSP);
	zassert_equal(r.sub, MT_RPC_ERROR);
	zassert_equal(r.len, 3);
	zassert_equal(r.data[0], MT_ERR_SUBSYSTEM);
	zassert_equal(r.data[1], 0x22);
	zassert_equal(r.data[2], 0x00);

	r = sreq(MT_SYS, 0x77, NULL, 0);
	zassert_equal(r.sub, MT_RPC_ERROR);
	zassert_equal(r.data[0], MT_ERR_COMMAND);

	r = sreq(MT_SYS, 0x13, (uint8_t[]){1}, 1);
	zassert_equal(r.data[0], MT_ERR_LENGTH);
}

static uint8_t osal_len(uint16_t id)
{
	uint8_t d[2];
	struct mt_frame r;

	mt_put_u16(d, id);
	r = sreq(MT_SYS, 0x13, d, 2);
	zassert_equal(r.len, 2);
	return (uint8_t)mt_get_u16(r.data);
}

ZTEST(zigbee_znp, test_osal_nv_roundtrip)
{
	/* ItemInit id=0x0083 len=4 initial value 4 bytes: NV_ITEM_UNINIT (0x09). */
	uint8_t init[] = {0x83, 0x00, 0x04, 0x00, 0x02, 0xAA, 0xBB};
	struct mt_frame r;

	zassert_equal(osal_len(0x83), 0);
	r = sreq(MT_SYS, 0x07, init, sizeof(init));
	zassert_equal(r.data[0], MT_STATUS_NV_ITEM_UNINIT);
	zassert_equal(osal_len(0x83), 4);

	/* Again: the item exists, SUCCESS. */
	r = sreq(MT_SYS, 0x07, init, sizeof(init));
	zassert_equal(r.data[0], MT_STATUS_SUCCESS);

	/* ReadExt offset 0: AA BB 00 00 */
	r = sreq(MT_SYS, 0x1C, (uint8_t[]){0x83, 0x00, 0x00, 0x00}, 4);
	zassert_equal(r.data[0], MT_STATUS_SUCCESS);
	zassert_equal(r.data[1], 4);
	zassert_mem_equal(&r.data[2], ((uint8_t[]){0xAA, 0xBB, 0, 0}), 4);

	/* WriteExt offset 2, 2 bytes */
	r = sreq(MT_SYS, 0x1D, (uint8_t[]){0x83, 0x00, 0x02, 0x00, 0x02, 0x00, 0xCC, 0xDD}, 8);
	zassert_equal(r.data[0], MT_STATUS_SUCCESS);
	/* Write that runs past the end is refused. */
	r = sreq(MT_SYS, 0x1D, (uint8_t[]){0x83, 0x00, 0x03, 0x00, 0x02, 0x00, 0xEE, 0xEE}, 8);
	zassert_equal(r.data[0], MT_STATUS_INVALID_PARAMETER);
	/* Legacy read at offset 2 */
	r = sreq(MT_SYS, 0x08, (uint8_t[]){0x83, 0x00, 0x02}, 3);
	zassert_equal(r.data[1], 2);
	zassert_mem_equal(&r.data[2], ((uint8_t[]){0xCC, 0xDD}), 2);

	/* Delete with the wrong length, then the right one. */
	r = sreq(MT_SYS, 0x12, (uint8_t[]){0x83, 0x00, 0x05, 0x00}, 4);
	zassert_equal(r.data[0], MT_STATUS_NV_BAD_ITEM_LEN);
	r = sreq(MT_SYS, 0x12, (uint8_t[]){0x83, 0x00, 0x04, 0x00}, 4);
	zassert_equal(r.data[0], MT_STATUS_SUCCESS);
	zassert_equal(osal_len(0x83), 0);

	/* Reading a missing item fails. */
	r = sreq(MT_SYS, 0x1C, (uint8_t[]){0x83, 0x00, 0x00, 0x00}, 4);
	zassert_equal(r.data[0], MT_STATUS_NV_OPER_FAILED);
}

ZTEST(zigbee_znp, test_extended_nv)
{
	/* NVCreate sys=1 item=4 (TCLK table) sub=0 length=21 */
	uint8_t create[] = {1, 4, 0, 0, 0, 21, 0, 0, 0};
	uint8_t len[] = {1, 4, 0, 0, 0};
	struct mt_frame r = sreq(MT_SYS, 0x32, len, sizeof(len));

	zassert_equal(r.len, 4);
	zassert_equal(mt_get_u32(r.data), 0, "absent item has length 0 (zigpy: Z-Stack 3.30)");

	r = sreq(MT_SYS, 0x30, create, sizeof(create));
	zassert_equal(r.data[0], MT_STATUS_SUCCESS, "herdsman requires 0 on create");
	r = sreq(MT_SYS, 0x32, len, sizeof(len));
	zassert_equal(mt_get_u32(r.data), 21);

	/* NVWrite offset 0, 3 bytes, then NVRead offset 1 length 2 */
	r = sreq(MT_SYS, 0x34, (uint8_t[]){1, 4, 0, 0, 0, 0, 0, 3, 7, 8, 9}, 11);
	zassert_equal(r.data[0], MT_STATUS_SUCCESS);
	r = sreq(MT_SYS, 0x33, (uint8_t[]){1, 4, 0, 0, 0, 1, 0, 2}, 8);
	zassert_equal(r.data[0], MT_STATUS_SUCCESS);
	zassert_equal(r.data[1], 2);
	zassert_mem_equal(&r.data[2], ((uint8_t[]){8, 9}), 2);

	r = sreq(MT_SYS, 0x31, len, sizeof(len));
	zassert_equal(r.data[0], MT_STATUS_SUCCESS);
	r = sreq(MT_SYS, 0x32, len, sizeof(len));
	zassert_equal(mt_get_u32(r.data), 0);
}

ZTEST(zigbee_znp, test_nv_pool_compaction_and_flush)
{
	struct znp_nv_key a = znp_nv_osal(1), b = znp_nv_osal(2), c = znp_nv_osal(3);
	uint8_t v = 0;

	zassert_ok(znp_nv_set(&nv, a, "AAAA", 4));
	zassert_ok(znp_nv_set(&nv, b, "BBBBBB", 6));
	zassert_ok(znp_nv_set(&nv, c, "CC", 2));
	zassert_ok(znp_nv_delete(&nv, b));
	zassert_equal(nv.pool_used, 6, "pool compacted");
	zassert_equal(znp_nv_read(&nv, c, 0, &v, 1), 1);
	zassert_equal(v, 'C');

	zassert_true(znp_nv_dirty(&nv));
	zassert_ok(znp_nv_flush(&nv));
	zassert_equal(stored, 2);
	zassert_equal(erased, 1, "the deleted slot is erased in the backend");
	zassert_false(znp_nv_dirty(&nv));

	/* A pool that is full refuses the next item. */
	zassert_equal(znp_nv_create(&nv, znp_nv_osal(9), 2000, NULL, 0), -ENOMEM);
}

ZTEST(zigbee_znp, test_reset_req_sends_indication)
{
	/* SYS_RESET_REQ is an AREQ; without an app hook the ZNP answers with the indication. */
	struct mt_frame f = {.type = MT_AREQ, .sub = MT_SYS, .cmd = 0x00, .len = 1, .data = {1}};
	uint8_t wire[MT_MAX_WIRE];
	size_t n = mt_encode(&f, wire);

	znp_rx(&z, wire, n);
	zassert_equal(txlen, 11);
	zassert_equal(txbuf[2], 0x41, "AREQ SYS");
	zassert_equal(txbuf[3], 0x80, "RESET_IND");
	zassert_equal(txbuf[4], ZNP_RESET_EXTERNAL);
	zassert_equal(txbuf[5], 2, "transport revision");
}

ZTEST(zigbee_znp, test_util_device_info)
{
	struct mt_frame r = sreq(MT_UTIL, 0x00, NULL, 0);

	zassert_equal(r.data[0], 0);
	zassert_mem_equal(&r.data[1], z.cfg.ext_addr, 8);
	r = sreq(MT_UTIL, 0x49, (uint8_t[]){0}, 1);
	zassert_equal(r.len, 36, "zigpy-znp reads this length as struct alignment");
}

static int dummy(struct znp *zn, const struct mt_frame *req, struct mt_frame *rsp)
{
	return 0;
}

ZTEST(zigbee_znp, test_capabilities_follow_registered_subsystems)
{
	zassert_equal(z.caps, BIT(0) | BIT(6), "SYS and UTIL");
	znp_register(&z, MT_AF, dummy);
	znp_register(&z, MT_ZDO, dummy);
	znp_register(&z, MT_APP_CNF, dummy);
	zassert_equal(z.caps, BIT(0) | BIT(3) | BIT(4) | BIT(6) | BIT(10));

	struct mt_frame r = sreq(MT_SYS, 0x01, NULL, 0);

	zassert_equal(mt_get_u16(r.data), z.caps);
}
