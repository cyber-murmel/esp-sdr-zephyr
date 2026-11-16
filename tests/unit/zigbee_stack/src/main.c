/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host sessions against the whole stack (ZNP, MAC, network layer, ZDO) with a mock radio:
 * what zigpy-znp and zigbee-herdsman send at startup and formation, and what the coordinator
 * must answer, byte for byte where the hosts parse it.
 */
#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <zigbee/zb_stack.h>

#include "../../zigbee_mac/src/mock_radio.h"

static const uint8_t EUI[8] = {0xF0, 0x91, 0x8E, 0xFE, 0xFF, 0x16, 0xA3, 0x98};
static const uint8_t EPID[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
static const uint8_t KEY[16] = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8,
				0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB0};
static const uint8_t JOINER[8] = {0xF1, 0xDE, 0xBC, 0x2A, 0x00, 0x4B, 0x12, 0x00};

static struct znp z;
static struct znp_nv nv;
static struct zb_stack st;
static K_THREAD_STACK_DEFINE(mac_stack, 4096);

/* ------------------------------------------------------------------------------------------ */
/* Capture of everything the ZNP sends to the host */

#define CAP_MAX 128

static struct mt_frame cap[CAP_MAX];
static int cap_n;
static K_MUTEX_DEFINE(cap_lock);

/* The ZNP serialises its transmissions, so one parser is enough. */
static struct mt_parser cap_parser;

static void tx_cb(void *user, const uint8_t *buf, size_t len)
{
	mt_parser_init(&cap_parser);
	k_mutex_lock(&cap_lock, K_FOREVER);
	for (size_t i = 0; i < len; i++) {
		if (mt_parser_feed(&cap_parser, buf[i]) == MT_FEED_FRAME) {
			zassert_true(cap_n < CAP_MAX, "capture full");
			cap[cap_n++] = cap_parser.frame;
		}
	}
	k_mutex_unlock(&cap_lock);
}

static int find(int from, int type, int sub, int cmd, struct mt_frame *out)
{
	int at = -1;

	k_mutex_lock(&cap_lock, K_FOREVER);
	for (int i = from; i < cap_n && at < 0; i++) {
		if (cap[i].type == type && cap[i].sub == sub && cap[i].cmd == cmd) {
			at = i;
			if (out != NULL) {
				*out = cap[i];
			}
		}
	}
	k_mutex_unlock(&cap_lock);
	return at;
}

/* Wait (simulated time) for an AREQ at or after capture index @p from. */
static bool wait_areq(int from, int sub, int cmd, int ms, struct mt_frame *out)
{
	for (int t = 0; t <= ms; t += 5) {
		if (find(from, MT_AREQ, sub, cmd, out) >= 0) {
			return true;
		}
		k_sleep(K_MSEC(5));
	}
	return false;
}

/* Wait for ZDO_STATE_CHANGE_IND with @p state at or after capture index @p from. */
static bool wait_state(int from, uint8_t state, int ms)
{
	for (int t = 0; t <= ms; t += 5) {
		struct mt_frame f;

		for (int i = from; i < CAP_MAX; i++) {
			int at = find(i, MT_AREQ, MT_ZDO, 0xC0, &f);

			if (at < 0) {
				break;
			}
			if (f.data[0] == state) {
				return true;
			}
			i = at;
		}
		k_sleep(K_MSEC(5));
	}
	return false;
}

/* The first commissioning notification at or after @p from that leaves nothing remaining. */
static bool wait_final_bdb(int from, int ms, struct mt_frame *out)
{
	for (int t = 0; t <= ms; t += 5) {
		for (int i = from; i < CAP_MAX; i++) {
			struct mt_frame f;
			int at = find(i, MT_AREQ, MT_APP_CNF, 0x80, &f);

			if (at < 0) {
				break;
			}
			if (f.data[2] == 0) {
				*out = f;
				return true;
			}
			i = at;
		}
		k_sleep(K_MSEC(5));
	}
	return false;
}

static int mark(void)
{
	int m;

	k_mutex_lock(&cap_lock, K_FOREVER);
	m = cap_n;
	k_mutex_unlock(&cap_lock);
	return m;
}

/* One SREQ; returns its SRSP (an RPC error shows as subsystem 0). */
static struct mt_frame sreq(uint8_t sub, uint8_t cmd, const void *data, uint8_t len)
{
	struct mt_frame f = {.type = MT_SREQ, .sub = sub, .cmd = cmd, .len = len}, r;
	uint8_t wire[MT_MAX_WIRE];
	int from = mark();
	size_t n;

	if (len > 0U) {
		memcpy(f.data, data, len);
	}
	n = mt_encode(&f, wire);
	znp_rx(&z, wire, n);
	if (find(from, MT_SRSP, sub, cmd, &r) >= 0 || find(from, MT_SRSP, 0, 0, &r) >= 0) {
		return r;
	}
	zassert_unreachable("no SRSP for %02x/%02x", sub, cmd);
	return r;
}

static void areq(uint8_t sub, uint8_t cmd, const void *data, uint8_t len)
{
	struct mt_frame f = {.type = MT_AREQ, .sub = sub, .cmd = cmd, .len = len};
	uint8_t wire[MT_MAX_WIRE];

	memcpy(f.data, data, len);
	znp_rx(&z, wire, mt_encode(&f, wire));
}

/* NV like zigpy-znp's NVRAMHelper does it (OSAL items with the extended calls). */
static uint16_t osal_len(uint16_t id)
{
	uint8_t d[2];
	struct mt_frame r;

	mt_put_u16(d, id);
	r = sreq(MT_SYS, 0x13, d, 2);
	return mt_get_u16(r.data);
}

static void osal_write(uint16_t id, const void *v, uint16_t len)
{
	uint8_t d[8 + 160];
	struct mt_frame r;
	uint16_t cur = osal_len(id);

	if (cur != len) {
		if (cur != 0U) {
			mt_put_u16(d, id);
			mt_put_u16(d + 2, cur);
			r = sreq(MT_SYS, 0x12, d, 4);
			zassert_equal(r.data[0], 0);
		}
		mt_put_u16(d, id);
		mt_put_u16(d + 2, len);
		d[4] = 1;
		d[5] = ((const uint8_t *)v)[0];
		r = sreq(MT_SYS, 0x07, d, 6);
		zassert_equal(r.data[0], 0x09, "ITEM_INIT must say NV_ITEM_UNINIT");
	}
	mt_put_u16(d, id);
	mt_put_u16(d + 2, 0);
	mt_put_u16(d + 4, len);
	memcpy(d + 6, v, len);
	r = sreq(MT_SYS, 0x1D, d, (uint8_t)(6 + len));
	zassert_equal(r.data[0], 0, "write of NV %04x failed", id);
}

static uint16_t osal_read(uint16_t id, void *out, uint16_t max)
{
	uint16_t len = osal_len(id), got = 0;

	zassert_true(len <= max);
	while (got < len) {
		uint8_t d[4];
		struct mt_frame r;

		mt_put_u16(d, id);
		mt_put_u16(d + 2, got);
		r = sreq(MT_SYS, 0x1C, d, 4);
		zassert_equal(r.data[0], 0);
		zassert_true(r.data[1] > 0);
		memcpy((uint8_t *)out + got, &r.data[2], r.data[1]);
		got += r.data[1];
	}
	return len;
}

static uint32_t ex_len(uint8_t item, uint16_t sub)
{
	uint8_t d[5] = {1, item, 0, (uint8_t)sub, (uint8_t)(sub >> 8)};
	struct mt_frame r = sreq(MT_SYS, 0x32, d, 5);

	return mt_get_u32(r.data);
}

static uint16_t ex_read(uint8_t item, uint16_t sub, void *out)
{
	uint32_t len = ex_len(item, sub);
	uint8_t d[8] = {1, item, 0, (uint8_t)sub, (uint8_t)(sub >> 8), 0, 0, (uint8_t)len};
	struct mt_frame r;

	if (len == 0U) {
		return 0;
	}
	r = sreq(MT_SYS, 0x33, d, 8);
	zassert_equal(r.data[0], 0);
	memcpy(out, &r.data[2], r.data[1]);
	return r.data[1];
}

/* ------------------------------------------------------------------------------------------ */

static void *setup(void)
{
	struct znp_config cfg = {.tx = tx_cb, .nv = &nv};

	memset(&mr, 0, sizeof(mr));
	k_sem_init(&mr.tx_sem, 0, 256);
	znp_nv_init(&nv, NULL);
	znp_init(&z, &cfg);
	zassert_ok(zb_stack_init(&st, &z, &nv, &mock_ops, NULL, EUI, mac_stack,
				 K_THREAD_STACK_SIZEOF(mac_stack), K_PRIO_PREEMPT(4)));
	mr.mac = &st.mac;
	return NULL;
}

static void before(void *f)
{
	/* A factory fresh device: empty NV, no network, nothing captured. */
	znp_nv_init(&nv, NULL);
	zb_stack_reset(&st, 0xFF);
	k_mutex_lock(&cap_lock, K_FOREVER);
	cap_n = 0;
	k_mutex_unlock(&cap_lock);
	mr.tx_count = 0;
	mr.nnets = 0;
	mr.script_n = mr.script_i = 0;
	memset(mr.ed_dbm, 0, sizeof(mr.ed_dbm));
	k_sem_reset(&mr.tx_sem);
	st.trace_level = 0;
}

ZTEST_SUITE(zigbee_stack, NULL, setup, before, NULL, NULL);

/* zigpy-znp's temporary network: host settings, BDB channel, commissioning. Returns the mark. */
static int zigpy_form(uint32_t mask)
{
	uint8_t d[5];
	uint8_t pan[2] = {0xFF, 0xFF}, zero = 0;
	uint8_t chan[4];
	struct mt_frame r;
	int m;

	mt_put_u32(chan, mask);
	osal_write(0x0083, pan, 2);
	osal_write(0x0047, EPID, 8);
	osal_write(0x0062, KEY, 16);
	osal_write(0x0063, &zero, 1);
	osal_write(0x0084, chan, 4);
	d[0] = 1;
	mt_put_u32(&d[1], mask);
	r = sreq(MT_APP_CNF, 0x08, d, 5);
	zassert_equal(r.sub, MT_APP_CNF);
	zassert_equal(r.data[0], 0);
	memset(d, 0, sizeof(d));
	r = sreq(MT_APP_CNF, 0x08, d, 5);
	zassert_equal(r.data[0], 0);
	m = mark();
	r = sreq(MT_APP_CNF, 0x05, (uint8_t[]){0x04}, 1);
	zassert_equal(r.sub, MT_APP_CNF);
	zassert_equal(r.data[0], 0);
	return m;
}

ZTEST(zigbee_stack, test_factory_state_and_capabilities)
{
	struct mt_frame r = sreq(MT_SYS, 0x01, NULL, 0);
	uint8_t b[8 + ZB_NIB_LEN];

	/* SYS, AF, ZDO, UTIL, DEBUG, APP_CNF: hosts then take it for Z-Stack 3. */
	zassert_equal(mt_get_u16(r.data), 0x04D9);

	/* Items the hosts read before they write anything. */
	zassert_equal(osal_len(0x0082), 24, "herdsman: 21 packed, else aligned");
	zassert_equal(osal_len(0x0003), 1);
	for (uint16_t id = 0x0030; id <= 0x0038; id++) {
		if (id == 0x0030 || id == 0x0032 || id == 0x0033 || id == 0x0036 || id == 0x0038) {
			zassert_equal(osal_len(id), 1, "stack setting %04x", id);
		}
	}
	zassert_equal(osal_len(0x003E), 1);
	zassert_equal(osal_len(0x0087), 1);
	zassert_equal(osal_len(0x008F), 1);
	zassert_equal(osal_len(0x005F), 1);
	zassert_equal(osal_len(0x004C), 2 + 6 * ZB_LINK_KEY_TABLE_ENTRIES);
	zassert_equal(osal_len(0x0062), 16);
	zassert_equal(osal_len(0x0001), 8);
	osal_read(0x0001, b, 8);
	zassert_mem_equal(b, EUI, 8);
	osal_read(0x0083, b, 2);
	zassert_equal(mt_get_u16(b), 0xFFFF);
	osal_read(0x008F, b, 1);
	zassert_equal(b[0], 1);

	/* No network: no NIB, no "on a network" flag. */
	zassert_equal(osal_len(0x0021), 0);
	zassert_equal(osal_len(0x0055), 0);

	/* Extended NV tables exist (empty). zigpy-znp's dialect probe is the TCLK table. */
	zassert_equal(ex_len(ZB_NVEX_TCLK_TABLE, 0), ZB_TCLK_LEN);
	zassert_equal(ex_len(ZB_NVEX_ADDRMGR, 31), ZB_ADDRMGR_LEN);
	zassert_equal(ex_len(ZB_NVEX_ADDRMGR, 32), 0, "the table ends at its last entry");
	zassert_equal(ex_len(ZB_NVEX_NWK_SEC_MATERIAL, 3), ZB_SECMAT_LEN);
	zassert_equal(ex_len(ZB_NVEX_APS_KEY_DATA, 0), ZB_APS_KEY_LEN);
	ex_read(ZB_NVEX_ADDRMGR, 0, b);
	zassert_equal(b[0], 0xFF, "empty address manager entry");
	ex_read(ZB_NVEX_TCLK_TABLE, 0, b);
	zassert_equal(b[16], 0xFF, "empty TCLK entry has keyAttributes DEFAULT");

	/* Not formed: device info says so. */
	r = sreq(MT_UTIL, 0x00, NULL, 0);
	zassert_equal(r.data[0], 0);
	zassert_equal(mt_get_u16(&r.data[9]), 0xFFFE);
	zassert_equal(r.data[11], 1, "coordinator");
	zassert_equal(r.data[12], 0, "not started");
	zassert_equal(r.data[13], 0);

	/* The alignment probe and the stack tune reply. */
	r = sreq(MT_UTIL, 0x49, (uint8_t[]){0}, 1);
	zassert_equal(r.len, 36);
	r = sreq(MT_SYS, 0x0F, (uint8_t[]){0, 10}, 2);
	zassert_equal(r.data[0], 10);
}

ZTEST(zigbee_stack, test_zigpy_forms_a_network_on_the_quietest_channel)
{
	uint8_t nib[ZB_NIB_LEN], key[17], on = 0;
	struct mt_frame ev;
	int m;

	mr.ed_dbm[15] = -60;
	mr.ed_dbm[20] = -90;
	mr.ed_dbm[25] = -70;
	mr.nets[0] = (struct mock_net){.channel = 15, .pan = 0xF74B, .permit = false};
	mr.nets[1] = (struct mock_net){.channel = 25, .pan = 0xC2BD, .permit = true};
	mr.nnets = 2;

	m = zigpy_form(0x02108000);

	/* ZDO state 9 and the commissioning notification (Success, formation, nothing remaining). */
	zassert_true(wait_state(m, 9, 10000), "never reached state 9");
	k_sleep(K_MSEC(200));
	/* The progress report must not look like the final one (RemainingModes != 0). */
	zassert_true(find(m, MT_AREQ, MT_APP_CNF, 0x80, &ev) >= 0);
	zassert_equal(ev.data[0], 1, "InProgress first");
	zassert_equal(ev.data[2], 0x04);

	/* The final notification: nothing remaining, Success on a fresh network. */
	zassert_true(wait_final_bdb(m, 1000, &ev), "no final commissioning notification");
	zassert_equal(ev.data[0], 0, "Success on a fresh network");
	zassert_equal(ev.data[1], 0x04);

	/* Fewest neighbours wins (channel 20), then the host's PAN rule: 0xFFFF means "pick one". */
	zassert_equal(osal_len(0x0021), ZB_NIB_LEN);
	osal_read(0x0021, nib, sizeof(nib));
	zassert_equal(nib[ZB_NIB_CHANNEL], 20);
	zassert_true(mt_get_u16(&nib[ZB_NIB_PAN_ID]) >= 1 && mt_get_u16(&nib[ZB_NIB_PAN_ID]) <= 0x3FFF);
	zassert_equal(nib[ZB_NIB_KEY_LOADED], 1);
	zassert_equal(nib[ZB_NIB_STATE], ZB_NWK_ROUTER);
	zassert_equal(mt_get_u16(&nib[ZB_NIB_DEV_ADDR]), 0x0000);
	zassert_equal(mt_get_u32(&nib[ZB_NIB_CHANNEL_LIST]), BIT(20));
	zassert_mem_equal(&nib[ZB_NIB_EPID], EPID, 8);
	zassert_mem_equal(&nib[ZB_NIB_COORD_EXT], EUI, 8);
	zassert_equal(nib[ZB_NIB_SECURITY_LEVEL], 5);
	zassert_equal(nib[ZB_NIB_UPDATE_ID], 0);

	osal_read(0x0055, &on, 1);
	zassert_equal(on, 1);
	osal_read(0x003A, key, sizeof(key));
	zassert_equal(key[0], 0);
	zassert_mem_equal(&key[1], KEY, 16, "the host's pre-configured key becomes the network key");
	osal_read(0x003B, key, sizeof(key));
	zassert_mem_equal(&key[1], KEY, 16);
	zassert_equal(osal_len(0x0101), 16, "TCLK seed");
	zassert_equal(osal_len(0x002D), 8);
	{
		uint8_t sm[ZB_SECMAT_LEN];

		zassert_equal(ex_read(ZB_NVEX_NWK_SEC_MATERIAL, 0, sm), ZB_SECMAT_LEN);
		zassert_equal(mt_get_u32(sm), 0, "frame counter starts at 0");
		zassert_mem_equal(&sm[4], EPID, 8, "keyed by the extended PAN ID");
	}

	/* The radio runs the network. */
	zassert_equal(mr.chan, 20);
	zassert_equal(mr.pan, mt_get_u16(&nib[ZB_NIB_PAN_ID]));
	zassert_equal(mr.short_addr, 0x0000);
	zassert_true(mr.coordinator);
	zassert_false(mr.promisc);

	/* A device on the channel finds it: the beacon carries the extended PAN ID, permit closed. */
	{
		uint8_t req[ZB_MAC_MAX_FRAME], bf[ZB_MAC_MAX_FRAME];
		struct zb_mac_hdr h;
		struct zb_beacon_info b;
		int before_tx = mr.tx_count;

		zb_mac_rx(&st.mac, req, zb_mac_build_beacon_req(req, 3), 200, -40);
		for (int i = 0; i < 100 && mr.tx_count == before_tx; i++) {
			k_sleep(K_MSEC(5));
		}
		zassert_equal(mr.tx_count, before_tx + 1);
		memcpy(bf, mr.tx_log[before_tx].f, mr.tx_log[before_tx].n);
		zassert_ok(zb_mac_parse(bf, mr.tx_log[before_tx].n, &h));
		zassert_ok(zb_mac_parse_beacon(&h, &bf[h.len], mr.tx_log[before_tx].n - h.len, &b));
		zassert_false(b.permit);
		zassert_mem_equal(b.epid, EPID, 8);
		zassert_equal(b.pan, mt_get_u16(&nib[ZB_NIB_PAN_ID]));
		zassert_true(b.router_cap && b.ed_cap);
	}

	/* The host reads the state back. */
	{
		struct mt_frame r = sreq(MT_UTIL, 0x00, NULL, 0);

		zassert_equal(mt_get_u16(&r.data[9]), 0x0000);
		zassert_equal(r.data[12], 9, "StartedAsCoordinator");
		r = sreq(MT_ZDO, 0x50, NULL, 0);
		zassert_equal(r.len, 24);
		zassert_equal(r.data[2], 9);
		zassert_equal(r.data[23], 20);
		zassert_mem_equal(&r.data[7], EPID, 8);
	}
}

ZTEST(zigbee_stack, test_reset_restores_the_network_without_forming)
{
	uint8_t nib[ZB_NIB_LEN], nib2[ZB_NIB_LEN];
	struct mt_frame ev;
	int m;

	mr.ed_dbm[20] = -90;
	m = zigpy_form(BIT(20));
	zassert_true(wait_state(m, 9, 10000), "never reached state 9");
	k_sleep(K_MSEC(100));
	osal_read(0x0021, nib, sizeof(nib));

	/* SYS_RESET_REQ (soft): no SRSP, SYS_RESET_IND, the network is up again from NV. */
	m = mark();
	areq(MT_SYS, 0x00, (uint8_t[]){1}, 1);
	zassert_true(wait_areq(m, MT_SYS, 0x80, 1000, &ev));
	zassert_equal(ev.len, 6);
	zassert_equal(ev.data[1], 2, "transport revision");
	zassert_equal(mr.chan, 20);
	zassert_true(mr.coordinator);
	osal_read(0x0021, nib2, sizeof(nib2));
	zassert_mem_equal(nib, nib2, sizeof(nib), "the NIB is the source of truth and is kept");
	zassert_equal(sreq(MT_UTIL, 0x00, NULL, 0).data[12], 9);

	/* BDB start on a formed network: state 9 again and FormationFailure, both accepted. */
	m = mark();
	zassert_equal(sreq(MT_APP_CNF, 0x05, (uint8_t[]){0x04}, 1).data[0], 0);
	zassert_true(wait_state(m, 9, 1000));
	zassert_true(wait_final_bdb(m, 1000, &ev), "no final commissioning notification");
	zassert_equal(ev.data[0], ZB_BDB_FORMATION_FAILURE);
	osal_read(0x0021, nib2, sizeof(nib2));
	zassert_mem_equal(nib, nib2, sizeof(nib), "no new network");

	/* herdsman resumes with ZDO_STARTUP_FROM_APP when the state is not 9. */
	m = mark();
	zassert_equal(sreq(MT_ZDO, 0x40, (uint8_t[]){100, 0}, 2).data[0], 0, "restored network state");
	zassert_true(wait_state(m, 9, 1000));
}

ZTEST(zigbee_stack, test_host_rewrites_the_nib_then_resets_like_zigpy)
{
	uint8_t nib[ZB_NIB_LEN], key[17] = {0};
	static const uint8_t epid2[8] = {9, 8, 7, 6, 5, 4, 3, 2};
	static const uint8_t key2[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
					 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00};
	struct mt_frame ev;
	uint8_t one = 1;
	int m;

	mr.ed_dbm[15] = -80;
	m = zigpy_form(BIT(15));
	zassert_true(wait_state(m, 9, 10000), "never reached state 9");
	osal_read(0x0021, nib, sizeof(nib));

	/* zigpy-znp's write_network_info(): NIB with the real settings, keys, then a reset. */
	mt_put_u16(&nib[ZB_NIB_PAN_ID], 0x1A62);
	memcpy(&nib[ZB_NIB_EPID], epid2, 8);
	nib[ZB_NIB_CHANNEL] = 25;
	mt_put_u32(&nib[ZB_NIB_CHANNEL_LIST], BIT(25));
	nib[ZB_NIB_UPDATE_ID] = 3;
	osal_write(0x0021, nib, sizeof(nib));
	osal_write(0x0047, epid2, 8);
	osal_write(0x002D, epid2, 8);
	memcpy(&key[1], key2, 16);
	osal_write(0x003A, key, 17);
	osal_write(0x003B, key, 17);
	osal_write(0x0055, &one, 1);

	m = mark();
	areq(MT_SYS, 0x00, (uint8_t[]){1}, 1);
	zassert_true(wait_areq(m, MT_SYS, 0x80, 1000, &ev));

	/* The firmware takes everything from NV: new channel, PAN, extended PAN ID, update id. */
	zassert_equal(mr.chan, 25);
	zassert_equal(mr.pan, 0x1A62);
	zassert_equal(st.net.channel, 25);
	zassert_equal(st.net.upd, 3);
	{
		uint8_t req[ZB_MAC_MAX_FRAME], bf[ZB_MAC_MAX_FRAME];
		struct zb_mac_hdr h;
		struct zb_beacon_info b;
		int before_tx = mr.tx_count;

		zb_mac_rx(&st.mac, req, zb_mac_build_beacon_req(req, 4), 200, -40);
		for (int i = 0; i < 100 && mr.tx_count == before_tx; i++) {
			k_sleep(K_MSEC(5));
		}
		zassert_equal(mr.tx_count, before_tx + 1);
		memcpy(bf, mr.tx_log[before_tx].f, mr.tx_log[before_tx].n);
		zassert_ok(zb_mac_parse(bf, mr.tx_log[before_tx].n, &h));
		zassert_ok(zb_mac_parse_beacon(&h, &bf[h.len], mr.tx_log[before_tx].n - h.len, &b));
		zassert_equal(b.pan, 0x1A62);
		zassert_equal(b.upd, 3);
		zassert_mem_equal(b.epid, epid2, 8);
	}
}

ZTEST(zigbee_stack, test_clear_state_forgets_the_network_but_not_the_identity)
{
	uint8_t sm[ZB_SECMAT_LEN], b[ZB_NIB_LEN], three = 3;
	struct mt_frame ev;
	int m;

	mr.ed_dbm[20] = -90;
	m = zigpy_form(BIT(20));
	zassert_true(wait_state(m, 9, 10000), "never reached state 9");
	k_sleep(K_MSEC(100));
	zassert_equal(osal_len(0x0021), ZB_NIB_LEN);

	/* Both hosts do this: startup option 3 (clear config and state), reset. */
	osal_write(0x0003, &three, 1);
	m = mark();
	areq(MT_SYS, 0x00, (uint8_t[]){1}, 1);
	zassert_true(wait_areq(m, MT_SYS, 0x80, 1000, &ev));

	zassert_equal(osal_len(0x0021), 0, "NIB gone");
	zassert_equal(osal_len(0x0055), 0);
	zassert_equal(osal_len(0x003A), 0);
	osal_read(0x0003, b, 1);
	zassert_equal(b[0], 0, "startup option consumed");
	osal_read(0x0001, b, 8);
	zassert_mem_equal(b, EUI, 8, "the IEEE address stays");
	zassert_equal(ex_read(ZB_NVEX_NWK_SEC_MATERIAL, 0, sm), ZB_SECMAT_LEN);
	zassert_mem_equal(&sm[4], EPID, 8, "frame counters survive ClearState");
	zassert_equal(sreq(MT_UTIL, 0x00, NULL, 0).data[12], 0);
	zassert_false(mr.coordinator);
	zassert_equal(osal_len(0x0062), 16, "config items are back (ClearConfig)");
	osal_read(0x0083, b, 2);
	zassert_equal(mt_get_u16(b), 0xFFFF);
}

ZTEST(zigbee_stack, test_zigpy_local_zdo_through_af)
{
	/* zigpy-znp's registration of its two endpoints, then the coordinator's own discovery. */
	static const uint8_t ep1[] = {1, 0x04, 0x01, 0x00, 0x04, 0, 0, 5, 0x00, 0x00, 0x06, 0x00, 0x0A,
				      0x00, 0x19, 0x00, 0x01, 0x05, 4, 0x01, 0x00, 0x20, 0x00, 0x00, 0x05,
				      0x02, 0x05};
	static const uint8_t ep2[] = {2, 0x5E, 0xC0, 0x20, 0x08, 0, 0, 1, 0x00, 0x00, 0};
	struct mt_frame r, ev;
	uint8_t d[64];
	int m;

	zassert_equal(sreq(MT_AF, 0x00, ep1, sizeof(ep1)).data[0], 0);
	zassert_equal(sreq(MT_AF, 0x00, ep2, sizeof(ep2)).data[0], 0);
	zassert_equal(sreq(MT_ZDO, 0x3E, (uint8_t[]){0xFF, 0xFF}, 2).data[0], 0);

	/* AF_DATA_REQUEST_EXT: Node_Desc_req (0x0002) to NWK 0 endpoint 0, TSN 5, payload nwk. */
	memset(d, 0, sizeof(d));
	d[0] = 2;                       /* NWK address mode */
	d[9] = 0;                       /* destination endpoint: ZDO */
	d[12] = 0;                      /* source endpoint */
	mt_put_u16(&d[13], 0x0002);
	d[15] = 7; /* ZNP transaction id */
	d[17] = 30;
	mt_put_u16(&d[18], 3);
	d[20] = 5; /* ZDO TSN */
	mt_put_u16(&d[21], 0x0000);
	m = mark();
	r = sreq(MT_AF, 0x02, d, 23);
	zassert_equal(r.sub, MT_AF);
	zassert_equal(r.data[0], 0);
	zassert_true(wait_areq(m, MT_ZDO, 0xFF, 1000, &ev));
	zassert_equal(mt_get_u16(&ev.data[0]), 0x0000, "source");
	zassert_equal(mt_get_u16(&ev.data[3]), 0x8002, "response cluster");
	zassert_equal(ev.data[6], 5, "the TSN is the request's");
	zassert_equal(ev.data[9], 0, "status");
	zassert_equal(ev.len, 9 + 3 + 13);
	zassert_equal(ev.data[12], 0x00, "coordinator");
	zassert_equal(ev.data[13], 0x40, "2.4 GHz");
	zassert_equal(ev.data[14], 0x8F);

	/* Active_EP_req (0x0005). */
	d[15] = 8;
	mt_put_u16(&d[13], 0x0005);
	d[20] = 6;
	m = mark();
	zassert_equal(sreq(MT_AF, 0x02, d, 23).data[0], 0);
	zassert_true(wait_areq(m, MT_ZDO, 0xFF, 1000, &ev));
	zassert_equal(mt_get_u16(&ev.data[3]), 0x8005);
	zassert_equal(ev.data[6], 6);
	zassert_equal(ev.data[9], 0);
	zassert_equal(ev.data[12], 2, "two endpoints");
	zassert_equal(ev.data[13], 1);
	zassert_equal(ev.data[14], 2);

	/* Simple_Desc_req (0x0004) for endpoint 1. */
	d[15] = 9;
	mt_put_u16(&d[13], 0x0004);
	d[20] = 7;
	mt_put_u16(&d[21], 0x0000);
	d[23] = 1;
	mt_put_u16(&d[18], 4);
	m = mark();
	zassert_equal(sreq(MT_AF, 0x02, d, 24).data[0], 0);
	zassert_true(wait_areq(m, MT_ZDO, 0xFF, 1000, &ev));
	zassert_equal(mt_get_u16(&ev.data[3]), 0x8004);
	zassert_equal(ev.data[9], 0);
	zassert_equal(ev.data[12], 1 + 2 + 2 + 1 + 1 + 2 * 5 + 1 + 2 * 4, "descriptor length byte");
	zassert_equal(ev.data[13], 1, "endpoint");
	zassert_equal(mt_get_u16(&ev.data[14]), 0x0104, "profile");
	zassert_equal(mt_get_u16(&ev.data[16]), 0x0400, "device");
	zassert_equal(ev.data[19], 5, "input clusters");
	zassert_equal(mt_get_u16(&ev.data[20]), 0x0000);

	/* An endpoint that does not exist: invalid endpoint status. */
	d[23] = 77;
	m = mark();
	zassert_equal(sreq(MT_AF, 0x02, d, 24).data[0], 0);
	zassert_true(wait_areq(m, MT_ZDO, 0xFF, 1000, &ev));
	zassert_equal(ev.data[9], 0x82);
}

ZTEST(zigbee_stack, test_herdsman_endpoint_and_group_calls)
{
	struct mt_frame r, ev;
	int m;

	/* ZDO_ACTIVE_EP_REQ: SRSP status, then the answer as an AREQ (DIRECT_CB). */
	m = mark();
	r = sreq(MT_ZDO, 0x05, (uint8_t[]){0, 0, 0, 0}, 4);
	zassert_equal(r.len, 1);
	zassert_equal(r.data[0], 0);
	zassert_true(wait_areq(m, MT_ZDO, 0x85, 1000, &ev));
	zassert_equal(mt_get_u16(&ev.data[0]), 0x0000, "srcaddr");
	zassert_equal(ev.data[2], 0, "status");
	zassert_equal(ev.data[5], 0, "no endpoints yet");

	/* AF_REGISTER of herdsman's endpoint 11 with clusters, then the descriptor request. */
	{
		static const uint8_t ep11[] = {11, 0x04, 0x01, 0x00, 0x04, 0, 0, 2, 0x01, 0x05, 0x0A, 0x00,
					       2, 0x00, 0x05, 0x02, 0x05};

		zassert_equal(sreq(MT_AF, 0x00, ep11, sizeof(ep11)).data[0], 0);
	}
	m = mark();
	zassert_equal(sreq(MT_ZDO, 0x04, (uint8_t[]){0, 0, 0, 0, 11}, 5).data[0], 0);
	zassert_true(wait_areq(m, MT_ZDO, 0x84, 1000, &ev));
	zassert_equal(ev.data[2], 0);
	zassert_equal(ev.data[6], 11, "endpoint");
	zassert_equal(mt_get_u16(&ev.data[7]), 0x0104);

	/* Group 0x0B84 (Green Power) on endpoint 242: not found, add, found. */
	r = sreq(MT_ZDO, 0x4A, (uint8_t[]){242, 0x84, 0x0B}, 3);
	zassert_equal(r.len, 4, "all four fields or herdsman drops the reply");
	zassert_equal(r.data[0], 1);
	r = sreq(MT_ZDO, 0x4B, (uint8_t[]){242, 0x84, 0x0B, 0}, 4);
	zassert_equal(r.data[0], 0);
	r = sreq(MT_ZDO, 0x4A, (uint8_t[]){242, 0x84, 0x0B}, 3);
	zassert_equal(r.data[0], 0);
	zassert_equal(mt_get_u16(&r.data[1]), 0x0B84);

	/* Node descriptor straight from the MT API. */
	m = mark();
	zassert_equal(sreq(MT_ZDO, 0x02, (uint8_t[]){0, 0, 0, 0}, 4).data[0], 0);
	zassert_true(wait_areq(m, MT_ZDO, 0x82, 1000, &ev));
	zassert_equal(ev.len, 2 + 3 + 13);

	/* A request for another device is refused until the NWK layer exists. */
	zassert_equal(sreq(MT_ZDO, 0x05, (uint8_t[]){0x34, 0x12, 0x34, 0x12}, 4).data[0], 1);
}

ZTEST(zigbee_stack, test_permit_join_follows_the_host)
{
	struct mt_frame ev;
	uint8_t d[5];
	int m;

	mr.ed_dbm[20] = -90;
	m = zigpy_form(BIT(20));
	zassert_true(wait_state(m, 9, 10000), "never reached state 9");

	/* permit(0) at zigpy startup: ZDO_MGMT_PERMIT_JOIN_REQ to the coordinator, AREQ answer. */
	d[0] = 2;
	mt_put_u16(&d[1], 0x0000);
	d[3] = 0;
	d[4] = 1;
	m = mark();
	zassert_equal(sreq(MT_ZDO, 0x36, d, 5).data[0], 0);
	zassert_true(wait_areq(m, MT_ZDO, 0xB6, 1000, &ev));
	zassert_equal(mt_get_u16(&ev.data[0]), 0x0000);
	zassert_equal(ev.data[2], 0);
	zassert_false(zb_mac_permit_open(&st.mac));

	/* Open for 60 s through the broadcast form herdsman and zigpy both send. */
	d[0] = 0x0F;
	mt_put_u16(&d[1], 0xFFFC);
	d[3] = 60;
	zassert_equal(sreq(MT_ZDO, 0x36, d, 5).data[0], 0);
	zassert_true(zb_mac_permit_open(&st.mac));
	m = mark();
	k_sleep(K_SECONDS(61));
	zassert_false(zb_mac_permit_open(&st.mac));

	/* BDB steering opens it for 180 s. */
	m = mark();
	zassert_equal(sreq(MT_APP_CNF, 0x05, (uint8_t[]){0x02}, 1).data[0], 0);
	zassert_true(wait_areq(m, MT_APP_CNF, 0x80, 1000, &ev));
	zassert_true(zb_mac_permit_open(&st.mac));
}

ZTEST(zigbee_stack, test_network_discovery_reports_beacons_like_a_scan_tool)
{
	struct mt_frame ev;
	int m = mark();
	uint8_t d[5];

	mr.nets[0] = (struct mock_net){.channel = 15, .pan = 0xF74B, .permit = false};
	mr.nets[1] = (struct mock_net){.channel = 25, .pan = 0xC2BD, .permit = true};
	mr.nnets = 2;
	mt_put_u32(d, 0x02108000);
	d[4] = 2;
	zassert_equal(sreq(MT_ZDO, 0x26, d, 5).data[0], 0);
	zassert_true(wait_areq(m, MT_ZDO, 0xC7, 5000, &ev), "no confirmation");
	zassert_equal(ev.data[0], 0);
	zassert_true(find(m, MT_AREQ, MT_ZDO, 0xC5, &ev) >= 0);
	zassert_equal(ev.data[0], 2, "two beacons");
	zassert_equal(ev.len, 1 + 2 * 21);
	/* Src, PanId, Channel, PermitJoining, RouterCapacity, DeviceCapacity, ProtocolVersion, ... */
	zassert_equal(mt_get_u16(&ev.data[1 + 2]), 0xF74B);
	zassert_equal(ev.data[1 + 4], 15);
	zassert_equal(ev.data[1 + 5], 0, "permit");
	zassert_equal(ev.data[1 + 6], 1, "router capacity");
	zassert_equal(ev.data[1 + 8], 2, "protocol version");
	zassert_equal(ev.data[1 + 9], 2, "stack profile");
	zassert_equal(ev.data[1 + 10], 200, "LQI");
	zassert_equal(ev.data[1 + 12], 3, "update id");
	zassert_equal(mt_get_u16(&ev.data[1 + 21 + 2]), 0xC2BD);
	zassert_equal(ev.data[1 + 21 + 5], 1, "permit");
	/* A second scan while idle works too. */
	m = mark();
	zassert_equal(sreq(MT_ZDO, 0x26, d, 5).data[0], 0);
	zassert_true(wait_areq(m, MT_ZDO, 0xC7, 5000, &ev));
}

ZTEST(zigbee_stack, test_energy_scan_both_ways)
{
	struct mt_frame ev;
	uint8_t d[32];
	int m;

	mr.ed_dbm[15] = -75;
	mr.ed_dbm[20] = -31;

	/* herdsman: ZDO_MGMT_NWK_UPDATE_REQ, answered by the ZDO_MGMT_NWK_UPDATE_NOTIFY AREQ. */
	memset(d, 0, sizeof(d));
	mt_put_u16(d, 0x0000);
	d[2] = 2;
	mt_put_u32(&d[3], BIT(15) | BIT(20));
	d[7] = 2;
	d[8] = 1;
	m = mark();
	zassert_equal(sreq(MT_ZDO, 0x37, d, 11).data[0], 0);
	zassert_true(wait_areq(m, MT_ZDO, 0xB8, 5000, &ev));
	zassert_equal(mt_get_u16(&ev.data[0]), 0x0000);
	zassert_equal(ev.data[2], 0);
	zassert_equal(mt_get_u32(&ev.data[3]), BIT(15) | BIT(20));
	zassert_equal(ev.data[11], 2, "one value per channel (zigpy zips strictly)");
	zassert_equal(ev.data[12], 60);
	zassert_equal(ev.data[13], 192);

	/* zigpy: Mgmt_NWK_Update_req (0x0038) through AF_DATA_REQUEST_EXT, answered as 0x8038. */
	memset(d, 0, sizeof(d));
	d[0] = 2;
	mt_put_u16(&d[13], 0x0038);
	d[15] = 9;
	mt_put_u16(&d[18], 7);
	d[20] = 0x42; /* ZDO TSN */
	mt_put_u32(&d[21], BIT(15) | BIT(20));
	d[25] = 4;
	d[26] = 1;
	m = mark();
	zassert_equal(sreq(MT_AF, 0x02, d, 27).data[0], 0);
	zassert_true(wait_areq(m, MT_ZDO, 0xFF, 5000, &ev));
	zassert_equal(mt_get_u16(&ev.data[3]), 0x8038);
	zassert_equal(ev.data[6], 0x42);
	zassert_equal(ev.data[9], 0, "status");
	zassert_equal(mt_get_u32(&ev.data[10]), BIT(15) | BIT(20));
	zassert_equal(ev.data[18], 2);
	zassert_equal(ev.data[19], 60);
	zassert_equal(ev.data[20], 192);
}

ZTEST(zigbee_stack, test_a_device_joins_and_is_remembered)
{
	uint8_t f[ZB_MAC_MAX_FRAME], e[ZB_ADDRMGR_LEN], d[5], nib[ZB_NIB_LEN];
	struct mt_frame ev, r;
	uint16_t child;
	int m, txs;

	mr.ed_dbm[20] = -90;
	m = zigpy_form(BIT(20));
	zassert_true(wait_state(m, 9, 10000), "never reached state 9");
	osal_read(0x0021, nib, sizeof(nib));
	d[0] = 2;
	mt_put_u16(&d[1], 0);
	d[3] = 0xFF;
	d[4] = 1;
	zassert_equal(sreq(MT_ZDO, 0x36, d, 5).data[0], 0);

	/* The joiner's association request and, a moment later, its data request. */
	txs = mr.tx_count;
	zb_mac_rx(&st.mac, f, zb_mac_build_assoc_req(f, 0x21, mt_get_u16(&nib[ZB_NIB_PAN_ID]), 0, JOINER, 0x8E),
		  200, -50);
	k_sleep(K_MSEC(50));
	zassert_equal(mr.tx_count, txs, "the response waits for the poll");
	zb_mac_rx(&st.mac, f, zb_mac_build_data_req(f, 0x22, mt_get_u16(&nib[ZB_NIB_PAN_ID]), 0, JOINER, NULL),
		  200, -50);
	for (int i = 0; i < 200 && mr.tx_count == txs; i++) {
		k_sleep(K_MSEC(5));
	}
	zassert_equal(mr.tx_count, txs + 1);
	{
		struct zb_mac_hdr h;

		zassert_ok(zb_mac_parse(mr.tx_log[txs].f, mr.tx_log[txs].n, &h));
		zassert_equal(mr.tx_log[txs].f[h.len], ZB_CMD_ASSOC_RSP);
		child = mt_get_u16(&mr.tx_log[txs].f[h.len + 1]);
		zassert_equal(mr.tx_log[txs].f[h.len + 3], ZB_ASSOC_OK);
	}
	zassert_true(child >= 1 && child <= 0xFFF7);
	k_sleep(K_MSEC(50));

	/* It is a child now: device info, association table and the address manager entry. */
	r = sreq(MT_UTIL, 0x00, NULL, 0);
	zassert_equal(r.data[13], 1);
	zassert_equal(mt_get_u16(&r.data[14]), child);
	r = sreq(MT_UTIL, 0x49, (uint8_t[]){0}, 1);
	zassert_equal(r.len, 36);
	zassert_equal(mt_get_u16(&r.data[0]), child);
	zassert_equal(r.data[4], 4, "CHILD_FFD_RX_IDLE");
	r = sreq(MT_UTIL, 0x49, (uint8_t[]){1}, 1);
	zassert_equal(r.data[4], 0xFF, "no second device");
	zassert_equal(ex_read(ZB_NVEX_ADDRMGR, 0, e), ZB_ADDRMGR_LEN);
	zassert_equal(e[0], 0x03, "AddrMgrUserType Assoc and Security: it holds the network key");
	zassert_equal(mt_get_u16(&e[2]), child);
	zassert_mem_equal(&e[4], JOINER, 8);

	/* After a reset it is still there. */
	m = mark();
	areq(MT_SYS, 0x00, (uint8_t[]){1}, 1);
	zassert_true(wait_areq(m, MT_SYS, 0x80, 1000, &ev));
	r = sreq(MT_UTIL, 0x00, NULL, 0);
	zassert_equal(r.data[13], 1);
	zassert_equal(mt_get_u16(&r.data[14]), child);
	zassert_not_null(zb_net_child_by_ext(&st.net, JOINER));

	/* The same device associating again gets its address back. */
	txs = mr.tx_count;
	zb_mac_rx(&st.mac, f, zb_mac_build_assoc_req(f, 0x31, mt_get_u16(&nib[ZB_NIB_PAN_ID]), 0, JOINER, 0x8E),
		  200, -50);
	zb_mac_rx(&st.mac, f, zb_mac_build_data_req(f, 0x32, mt_get_u16(&nib[ZB_NIB_PAN_ID]), 0, JOINER, NULL),
		  200, -50);
	for (int i = 0; i < 200 && mr.tx_count == txs; i++) {
		k_sleep(K_MSEC(5));
	}
	zassert_equal(mr.tx_count, txs + 1);
	zassert_equal(zb_net_child_count(&st.net), 1);
}

ZTEST(zigbee_stack, test_trace_streams_frames_to_the_host)
{
	struct mt_frame ev;
	uint8_t req[ZB_MAC_MAX_FRAME];
	int m;

	mr.ed_dbm[20] = -90;
	m = zigpy_form(BIT(20));
	zassert_true(wait_state(m, 9, 10000), "never reached state 9");

	zassert_equal(sreq(MT_DEBUG, 0x00, (uint8_t[]){0, 2}, 2).data[0], 0);
	m = mark();
	zb_mac_rx(&st.mac, req, zb_mac_build_beacon_req(req, 1), 200, -40);
	zassert_true(wait_areq(m, MT_DEBUG, 0x80, 1000, &ev));
	zassert_equal(ev.data[0], 2, "level");
	zassert_not_null(strstr((const char *)&ev.data[1], "RX 8 "));
	zassert_equal(sreq(MT_DEBUG, 0x00, (uint8_t[]){0, 0}, 2).data[0], 0);
}

ZTEST(zigbee_stack, test_endpoints_of_both_hosts_register)
{
	/* herdsman registers 15 endpoints, among them 239 (Shelly) and 242 (Green Power). */
	static const uint8_t eps[] = {1, 2, 3, 4, 5, 6, 8, 10, 11, 110, 12, 13, 47, 239, 242};

	for (size_t i = 0; i < ARRAY_SIZE(eps); i++) {
		uint8_t d[] = {eps[i], 0x04, 0x01, 0x05, 0x00, 0, 0, 0, 0};

		zassert_equal(sreq(MT_AF, 0x00, d, sizeof(d)).data[0], 0, "endpoint %u", eps[i]);
	}
	/* Endpoint 0 is the ZDO, 0xFF is the broadcast endpoint. */
	zassert_equal(sreq(MT_AF, 0x00, (uint8_t[]){0, 4, 1, 5, 0, 0, 0, 0, 0}, 9).data[0], 2);
	zassert_equal(sreq(MT_AF, 0x00, (uint8_t[]){0xFF, 4, 1, 5, 0, 0, 0, 0, 0}, 9).data[0], 2);
	/* 15 distinct registered (the table holds 16); a re-registration replaces. */
	zassert_equal(sreq(MT_AF, 0x00, (uint8_t[]){242, 0xE0, 0xA1, 5, 0, 0, 0, 0, 0}, 9).data[0], 0);
}

ZTEST(zigbee_stack, test_a_host_areq_cannot_crash_the_stick)
{
	int m = mark();

	/* AREQs of commands that are SREQs: ignored, nothing sent back. */
	areq(MT_SYS, 0x01, NULL, 0);
	areq(MT_ZDO, 0x50, NULL, 0);
	areq(MT_UTIL, 0x00, NULL, 0);
	areq(MT_SYS, 0x13, (uint8_t[]){0x21, 0x00}, 2);
	areq(MT_AF, 0x00, (uint8_t[]){1, 2}, 2);
	zassert_equal(mark(), m, "an AREQ gets no answer");
	/* The one AREQ a host sends works, and the stick still answers. */
	areq(MT_SYS, 0x00, (uint8_t[]){1}, 1);
	zassert_true(wait_areq(m, MT_SYS, 0x80, 1000, NULL));
	zassert_equal(mt_get_u16(sreq(MT_SYS, 0x01, NULL, 0).data), 0x04D9);
}

ZTEST(zigbee_stack, test_scan_duration_is_validated)
{
	uint8_t d[11];

	mt_put_u32(d, 0x02108000);
	d[4] = 15;
	zassert_equal(sreq(MT_ZDO, 0x26, d, 5).data[0], 2, "INVALID_PARAMETER");
	d[4] = 14;
	zassert_equal(sreq(MT_ZDO, 0x26, d, 5).data[0], 0, "14 is the longest allowed");
	/* Energy scans: 0..5 scan, 0xFE and 0xFF are the special values, the rest is invalid. */
	memset(d, 0, sizeof(d));
	mt_put_u32(&d[3], BIT(15));
	d[7] = 6;
	zassert_equal(sreq(MT_ZDO, 0x37, d, 11).data[0], 2);
	d[7] = 0xFE;
	mt_put_u32(&d[3], BIT(15) | BIT(20));
	zassert_equal(sreq(MT_ZDO, 0x37, d, 11).data[0], 2, "a channel change names one channel");
}

ZTEST(zigbee_stack, test_channel_change_moves_the_network)
{
	uint8_t d[11], nib[ZB_NIB_LEN];
	struct mt_frame ev;
	int m;

	mr.ed_dbm[20] = -90;
	m = zigpy_form(BIT(20));
	zassert_true(wait_state(m, 9, 10000));
	osal_read(0x0021, nib, sizeof(nib));
	zassert_equal(nib[ZB_NIB_UPDATE_ID], 0);

	/* zigpy-znp: Mgmt_NWK_Update_req to the coordinator with ScanDuration 0xFE. */
	memset(d, 0, sizeof(d));
	d[2] = 2;
	mt_put_u32(&d[3], BIT(25));
	d[7] = 0xFE;
	zassert_equal(sreq(MT_ZDO, 0x37, d, 11).data[0], 0);
	osal_read(0x0021, nib, sizeof(nib));
	zassert_equal(nib[ZB_NIB_CHANNEL], 25);
	zassert_equal(mt_get_u32(&nib[ZB_NIB_CHANNEL_LIST]), BIT(25));
	zassert_equal(nib[ZB_NIB_UPDATE_ID], 1, "the update id counts channel changes");
	zassert_equal(mr.chan, 25);
	zassert_equal(st.net.channel, 25);
	zassert_equal(st.net.upd, 1);
	zassert_equal(mt_get_u16(&nib[ZB_NIB_PAN_ID]), mr.pan);

	/* It survives a reset. */
	m = mark();
	areq(MT_SYS, 0x00, (uint8_t[]){1}, 1);
	zassert_true(wait_areq(m, MT_SYS, 0x80, 1000, &ev));
	zassert_equal(mr.chan, 25);

	/* ScanDuration 0xFF sets the mask and the manager address in the NIB. */
	d[7] = 0xFF;
	mt_put_u32(&d[3], BIT(25) | BIT(15));
	mt_put_u16(&d[9], 0x0000);
	zassert_equal(sreq(MT_ZDO, 0x37, d, 11).data[0], 0);
	osal_read(0x0021, nib, sizeof(nib));
	zassert_equal(mt_get_u32(&nib[ZB_NIB_CHANNEL_LIST]), BIT(25) | BIT(15));
	zassert_equal(nib[ZB_NIB_CHANNEL], 25, "the operating channel stays");

	/* Without a network there is nothing to move. */
	{
		uint8_t three = 3;

		osal_write(0x0003, &three, 1);
		areq(MT_SYS, 0x00, (uint8_t[]){1}, 1);
		d[7] = 0xFE;
		mt_put_u32(&d[3], BIT(15));
		zassert_equal(sreq(MT_ZDO, 0x37, d, 11).data[0], 1);
	}
}

ZTEST(zigbee_stack, test_tx_power_reaches_the_radio)
{
	mr.txpower_calls = 0;
	zassert_equal(sreq(MT_SYS, 0x0F, (uint8_t[]){0, 100}, 2).data[0], 20, "clamped to the maximum");
	zassert_equal(mr.txpower, 20);
	zassert_equal(sreq(MT_SYS, 0x0F, (uint8_t[]){0, (uint8_t)-100}, 2).data[0], (uint8_t)-20);
	zassert_equal(mr.txpower, -20);
	zassert_equal(sreq(MT_SYS, 0x14, (uint8_t[]){8}, 1).data[0], 0, "3.30: a status");
	zassert_equal(mr.txpower, 8);
	zassert_true(mr.txpower_calls >= 3);
}

ZTEST(zigbee_stack, test_more_local_zdo_answers)
{
	struct mt_frame ev;
	uint8_t d[64];
	int m;

	/* Match_Desc_req for the Home Automation profile and the Basic cluster. */
	zassert_equal(sreq(MT_AF, 0x00, (uint8_t[]){1, 0x04, 0x01, 0x00, 0x04, 0, 0, 1, 0x00, 0x00, 0}, 11).data[0], 0);
	m = mark();
	zassert_equal(sreq(MT_ZDO, 0x06, (uint8_t[]){0, 0, 0, 0, 0x04, 0x01, 1, 0x00, 0x00, 0}, 10).data[0], 0);
	zassert_true(wait_areq(m, MT_ZDO, 0x86, 1000, &ev));
	zassert_equal(mt_get_u16(&ev.data[0]), 0, "srcaddr");
	zassert_equal(ev.data[2], 0);
	zassert_equal(ev.data[5], 1, "one match");
	zassert_equal(ev.data[6], 1);

	/* Power descriptor and the empty neighbour, route and binding tables. */
	m = mark();
	zassert_equal(sreq(MT_ZDO, 0x03, (uint8_t[]){0, 0, 0, 0}, 4).data[0], 0);
	zassert_true(wait_areq(m, MT_ZDO, 0x83, 1000, &ev));
	zassert_equal(ev.len, 2 + 5);
	m = mark();
	zassert_equal(sreq(MT_ZDO, 0x31, (uint8_t[]){0, 0, 0}, 3).data[0], 0);
	zassert_true(wait_areq(m, MT_ZDO, 0xB1, 1000, &ev));
	zassert_equal(ev.len, 2 + 4);
	zassert_equal(ev.data[2], 0, "status");
	zassert_equal(ev.data[3], 0, "no neighbours");

	/* herdsman asks for the coordinator's own IEEE address by network address. */
	m = mark();
	zassert_equal(sreq(MT_ZDO, 0x01, (uint8_t[]){0, 0, 0, 0}, 4).data[0], 0);
	zassert_true(wait_areq(m, MT_ZDO, 0x81, 1000, &ev));
	zassert_equal(ev.data[0], 0, "status");
	zassert_mem_equal(&ev.data[1], EUI, 8);
	zassert_equal(mt_get_u16(&ev.data[9]), 0x0000);
	zassert_equal(ev.len, 1 + 8 + 2 + 2, "status, IEEE, NWK, start index, count");

	/* A request this firmware does not serve gets an answer: not supported. */
	memset(d, 0, sizeof(d));
	d[0] = 2;
	mt_put_u16(&d[13], 0x0034); /* Mgmt_Leave_req */
	d[15] = 3;
	mt_put_u16(&d[18], 10);
	d[20] = 0x55;
	m = mark();
	zassert_equal(sreq(MT_AF, 0x02, d, 30).data[0], 0);
	zassert_true(wait_areq(m, MT_ZDO, 0xFF, 1000, &ev));
	zassert_equal(mt_get_u16(&ev.data[3]), 0x8034);
	zassert_equal(ev.data[6], 0x55);
	zassert_equal(ev.data[9], 0x84, "ZDO status NOT_SUPPORTED");

	/* Route discovery calls of both hosts are answered. */
	zassert_equal(sreq(MT_ZDO, 0x45, (uint8_t[]){0x34, 0x12, 0, 30}, 4).data[0], 0);
	zassert_equal(sreq(MT_ZDO, 0x46, (uint8_t[]){0x34, 0x12, 1, 3}, 4).data[0], 4);
}

ZTEST(zigbee_stack, test_association_table_commands)
{
	static const uint8_t other[8] = {9, 9, 9, 9, 9, 9, 9, 9};
	uint8_t d[11];
	struct mt_frame r;

	/* ASSOC_ADD (nwk, ieee, relation), ASSOC_GET_WITH_ADDRESS by IEEE and by short address. */
	mt_put_u16(d, 0x1234);
	memcpy(&d[2], JOINER, 8);
	d[10] = 4;
	zassert_equal(sreq(MT_UTIL, 0x64, d, 11).data[0], 0);
	r = sreq(MT_UTIL, 0x00, NULL, 0);
	zassert_equal(r.data[13], 1);
	memcpy(d, JOINER, 8);
	mt_put_u16(&d[8], 0xFFFF);
	r = sreq(MT_UTIL, 0x4A, d, 10);
	zassert_equal(r.len, 36);
	zassert_equal(mt_get_u16(&r.data[0]), 0x1234);
	zassert_equal(r.data[4], 4);
	memset(d, 0, 8);
	mt_put_u16(&d[8], 0x1234);
	r = sreq(MT_UTIL, 0x4A, d, 10);
	zassert_equal(mt_get_u16(&r.data[0]), 0x1234);
	/* An unknown device: the "not used" struct. */
	memcpy(d, other, 8);
	r = sreq(MT_UTIL, 0x4A, d, 10);
	zassert_equal(r.data[4], 0xFF);
	/* ASSOC_REMOVE: once, then it is gone. */
	zassert_equal(sreq(MT_UTIL, 0x63, JOINER, 8).data[0], 0);
	zassert_equal(sreq(MT_UTIL, 0x63, JOINER, 8).data[0], 1);
	zassert_equal(sreq(MT_UTIL, 0x00, NULL, 0).data[13], 0);
}

ZTEST(zigbee_stack, test_key_table_entries_of_the_host_are_not_children)
{
	uint8_t e[ZB_ADDRMGR_LEN], d[ZB_ADDRMGR_LEN + 8], nib[ZB_NIB_LEN];
	struct mt_frame ev;
	int m;

	/* zigpy-znp restores a router as a Default|Security address manager entry (no Assoc). */
	memset(e, 0, sizeof(e));
	e[0] = 0x02;
	e[1] = 0xFF;
	mt_put_u16(&e[2], 0x4321);
	memcpy(&e[4], JOINER, 8);
	d[0] = 1;
	d[1] = ZB_NVEX_ADDRMGR;
	d[2] = 0;
	d[3] = 0;
	d[4] = 0;
	d[5] = 0;
	d[6] = 0;
	d[7] = ZB_ADDRMGR_LEN;
	memcpy(&d[8], e, sizeof(e));
	zassert_equal(sreq(MT_SYS, 0x34, d, 8 + ZB_ADDRMGR_LEN).data[0], 0);

	mr.ed_dbm[20] = -90;
	m = zigpy_form(BIT(20));
	zassert_true(wait_state(m, 9, 10000));
	osal_read(0x0021, nib, sizeof(nib));
	m = mark();
	areq(MT_SYS, 0x00, (uint8_t[]){1}, 1);
	zassert_true(wait_areq(m, MT_SYS, 0x80, 1000, &ev));
	zassert_equal(sreq(MT_UTIL, 0x00, NULL, 0).data[13], 0, "not a child");

	/* A real join takes another table slot, and the host's entry is left alone. */
	{
		uint8_t f[ZB_MAC_MAX_FRAME];
		static const uint8_t joiner2[8] = {7, 7, 7, 7, 7, 7, 7, 7};
		uint8_t g[ZB_ADDRMGR_LEN];
		int txs = mr.tx_count;

		zassert_equal(sreq(MT_ZDO, 0x36, (uint8_t[]){2, 0, 0, 0xFF, 1}, 5).data[0], 0);
		zb_mac_rx(&st.mac, f, zb_mac_build_assoc_req(f, 1, mt_get_u16(&nib[ZB_NIB_PAN_ID]), 0, joiner2, 0x8E), 200, -50);
		zb_mac_rx(&st.mac, f, zb_mac_build_data_req(f, 2, mt_get_u16(&nib[ZB_NIB_PAN_ID]), 0, joiner2, NULL), 200, -50);
		for (int i = 0; i < 200 && mr.tx_count == txs; i++) {
			k_sleep(K_MSEC(5));
		}
		k_sleep(K_MSEC(50));
		zassert_equal(ex_read(ZB_NVEX_ADDRMGR, 0, g), ZB_ADDRMGR_LEN);
		zassert_equal(g[0], 0x02, "the host's entry is untouched");
		zassert_equal(mt_get_u16(&g[2]), 0x4321);
		zassert_equal(ex_read(ZB_NVEX_ADDRMGR, 1, g), ZB_ADDRMGR_LEN);
		zassert_equal(g[0], 0x03, "the child took the next slot");
		zassert_equal(sreq(MT_UTIL, 0x00, NULL, 0).data[13], 1);
	}
}

ZTEST(zigbee_stack, test_a_stored_network_is_never_overwritten_by_formation)
{
	uint8_t nib[ZB_NIB_LEN], nib2[ZB_NIB_LEN];
	struct mt_frame ev;
	int m;

	mr.ed_dbm[20] = -90;
	m = zigpy_form(BIT(20));
	zassert_true(wait_state(m, 9, 10000));
	osal_read(0x0021, nib, sizeof(nib));

	/* The MAC is down (as after a failed radio start) but the NV network is valid. */
	zassert_ok(zb_mac_stop(&st.mac));
	st.net.formed = false;
	m = mark();
	zassert_equal(sreq(MT_APP_CNF, 0x05, (uint8_t[]){0x04}, 1).data[0], 0);
	zassert_true(wait_final_bdb(m, 5000, &ev));
	zassert_equal(ev.data[0], ZB_BDB_FORMATION_FAILURE, "restored, not formed");
	osal_read(0x0021, nib2, sizeof(nib2));
	zassert_mem_equal(nib, nib2, sizeof(nib));
	zassert_true(st.net.formed);
	zassert_equal(mr.chan, 20);
}

ZTEST(zigbee_stack, test_overlapping_energy_scans_report_to_their_own_host_path)
{
	struct mt_frame ev;
	uint8_t d[32];
	int m;

	mr.ed_dbm[15] = -75;
	/* The MT request starts a scan; a loopback request while it runs is refused. */
	memset(d, 0, sizeof(d));
	d[2] = 2;
	mt_put_u32(&d[3], BIT(15));
	d[7] = 5;
	d[8] = 1;
	m = mark();
	zassert_equal(sreq(MT_ZDO, 0x37, d, 11).data[0], 0);
	{
		uint8_t a[27];

		memset(a, 0, sizeof(a));
		a[0] = 2;
		mt_put_u16(&a[13], 0x0038);
		a[15] = 9;
		mt_put_u16(&a[18], 7);
		a[20] = 0x42;
		mt_put_u32(&a[21], BIT(15));
		a[25] = 0;
		a[26] = 1;
		(void)sreq(MT_AF, 0x02, a, 27);
	}
	/* The result of the first scan is the AREQ, not a ZDO response with the second's TSN. */
	zassert_true(wait_areq(m, MT_ZDO, 0xB8, 5000, &ev));
	k_sleep(K_MSEC(100));
	zassert_true(find(m, MT_AREQ, MT_ZDO, 0xFF, NULL) < 0 || find(m, MT_AREQ, MT_ZDO, 0xB8, NULL) >= 0);
}

/* ------------------------------------------------------------------------------------------ */
/* The trust center, NWK security and the APS data path, against a simulated device. */

struct txd {
	struct zb_nwk_hdr nh;
	struct zb_sec_hdr sh;
	uint8_t pl[ZB_MAC_MAX_FRAME];
	size_t plen;
	uint16_t mac_dst;
	bool ar;
};

/* Open a frame the stick transmitted (MAC data frame carrying a NWK frame). */
static bool open_tx(int idx, const uint8_t key[16], struct txd *o)
{
	uint8_t f[ZB_MAC_MAX_FRAME];
	size_t n = mr.tx_log[idx].n, plen, nl;
	struct zb_mac_hdr mh;
	uint8_t *nw;

	memcpy(f, mr.tx_log[idx].f, n);
	if (zb_mac_parse(f, n, &mh) != 0 || mh.type != ZB_FT_DATA) {
		return false;
	}
	o->mac_dst = mh.dst_short;
	o->ar = mh.ar;
	nw = &f[mh.len];
	nl = n - mh.len;
	if (zb_nwk_parse(nw, nl, &o->nh) != 0) {
		return false;
	}
	if (!o->nh.security) {
		o->plen = nl - o->nh.len;
		memcpy(o->pl, nw + o->nh.len, o->plen);
		return true;
	}
	if (zb_sec_parse(nw, nl, o->nh.len, &o->sh) != 0 ||
	    zb_sec_open(nw, nl, o->nh.len, key, &o->sh, &plen) != 0) {
		return false;
	}
	o->plen = plen;
	memcpy(o->pl, nw + o->nh.len + o->sh.len, plen);
	return true;
}

/* Wait until the mock radio has transmitted @p count frames in total. */
static void wait_tx_total(int count)
{
	for (int i = 0; i < 400 && mr.tx_count < count; i++) {
		k_sleep(K_MSEC(5));
	}
	zassert_true(mr.tx_count >= count, "expected %d transmissions, saw %d", count, mr.tx_count);
}

/* The first NWK data frame transmitted at or after index @p from (link status and the like skipped). */
static int wait_data_tx(int from, const uint8_t key[16], struct txd *t)
{
	for (int w = 0; w < 400; w++) {
		for (int i = from; i < mr.tx_count; i++) {
			if (open_tx(i, key, t) && t->nh.type == ZB_NWK_DATA) {
				return i;
			}
		}
		k_sleep(K_MSEC(5));
	}
	zassert_unreachable("no NWK data frame after index %d", from);
	return -1;
}

/* A device sends the stick a NWK frame (secured with the network key, as a joined device does). */
static void dev_sends(uint16_t child, uint8_t type, uint16_t nwk_dst, const uint8_t *pl, size_t len,
		      uint32_t counter, const uint8_t key[16])
{
	uint8_t f[ZB_MAC_MAX_FRAME], mf[ZB_MAC_MAX_FRAME];
	struct zb_nwk_hdr nh = {.type = type, .proto_ver = 2, .security = true, .dst = nwk_dst,
				.src = child, .radius = 30, .seq = (uint8_t)counter};
	size_t hl = zb_nwk_build(&nh, f), tl, mn;

	memcpy(&f[hl], pl, len);
	tl = zb_sec_protect(f, 116, hl, len, key, ZB_KEYID_NETWORK, 0, counter, JOINER);
	zassert_true(tl > 0U);
	mn = zb_mac_build_data(mf, (uint8_t)counter, mr.pan, nwk_dst == 0U ? 0U : 0xFFFFU, child,
			       nwk_dst == 0U, f, tl);
	zb_mac_rx(&st.mac, mf, mn, 200, -50);
}

/* The join of JOINER up to and including the network key. Returns the device's short address. */
static int join_tx0;

static uint16_t join_device(void)
{
	uint8_t f[ZB_MAC_MAX_FRAME];
	int txs;
	uint16_t child;
	struct mt_frame ev;
	int m;

	mr.ed_dbm[20] = -90;
	m = zigpy_form(BIT(20));
	zassert_true(wait_state(m, 9, 10000));
	zassert_equal(sreq(MT_ZDO, 0x36, (uint8_t[]){2, 0, 0, 0xFF, 1}, 5).data[0], 0);
	m = mark();
	txs = join_tx0 = mr.tx_count;
	zb_mac_rx(&st.mac, f, zb_mac_build_assoc_req(f, 1, mr.pan, 0, JOINER, 0x8E), 200, -50);
	zb_mac_rx(&st.mac, f, zb_mac_build_data_req(f, 2, mr.pan, 0, JOINER, NULL), 200, -50);
	wait_tx_total(txs + 2);
	{
		struct zb_mac_hdr h;

		zassert_ok(zb_mac_parse(mr.tx_log[txs].f, mr.tx_log[txs].n, &h));
		child = mt_get_u16(&mr.tx_log[txs].f[h.len + 1]);
	}
	zassert_true(wait_areq(m, MT_ZDO, 0xCA, 1000, &ev), "no ZDO_TC_DEV_IND");
	zassert_equal(mt_get_u16(&ev.data[0]), child);
	zassert_mem_equal(&ev.data[2], JOINER, 8);
	zassert_equal(mt_get_u16(&ev.data[10]), 0x0000, "parent");
	return child;
}

ZTEST(zigbee_stack, test_trust_center_hands_the_joiner_the_network_key)
{
	static const uint8_t link[16] = "ZigBeeAlliance09";
	uint8_t ktk[16], tk[ZB_MAC_MAX_FRAME];
	int txs;
	struct zb_mac_hdr mh;
	struct zb_nwk_hdr nh;
	struct zb_sec_hdr sh;
	uint16_t child;
	size_t plen, nl;

	child = join_device();
	txs = join_tx0;
	zb_key_transport_key(link, ktk);

	/* The frame after the association response: NWK unsecured, the APS command secured. */
	memcpy(tk, mr.tx_log[txs + 1].f, mr.tx_log[txs + 1].n);
	zassert_ok(zb_mac_parse(tk, mr.tx_log[txs + 1].n, &mh));
	zassert_equal(mh.dst_short, child);
	zassert_true(mh.ar, "acknowledged by the joiner");
	nl = mr.tx_log[txs + 1].n - mh.len;
	zassert_ok(zb_nwk_parse(&tk[mh.len], nl, &nh));
	zassert_false(nh.security, "the joiner has no network key yet");
	zassert_equal(nh.dst, child);
	zassert_equal(nh.src, 0x0000);
	zassert_equal(tk[mh.len + nh.len], 0x21, "APS command frame with security");
	zassert_ok(zb_sec_parse(&tk[mh.len + nh.len], nl - nh.len, 2, &sh));
	zassert_equal(sh.key_id, ZB_KEYID_TRANSPORT);
	zassert_mem_equal(sh.src, EUI, 8);
	zassert_ok(zb_sec_open(&tk[mh.len + nh.len], nl - nh.len, 2, ktk, &sh, &plen));
	zassert_equal(plen, 35);
	{
		const uint8_t *p = &tk[mh.len + nh.len + 2 + sh.len];

		zassert_equal(p[0], 0x05, "Transport-Key");
		zassert_equal(p[1], 0x01, "standard network key");
		zassert_mem_equal(&p[2], KEY, 16);
		zassert_equal(p[18], 0, "key sequence number");
		zassert_mem_equal(&p[19], JOINER, 8);
		zassert_mem_equal(&p[27], EUI, 8);
	}
	zassert_equal(sreq(MT_UTIL, 0x00, NULL, 0).data[13], 1);
}

ZTEST(zigbee_stack, test_data_to_and_from_a_joined_device)
{
	static const uint8_t ep1[] = {1, 0x04, 0x01, 0x00, 0x04, 0, 0, 0, 0};
	uint8_t d[64], aps[32];
	struct txd t;
	struct mt_frame ev;
	uint16_t child;
	int m, txs;

	child = join_device();
	zassert_equal(sreq(MT_AF, 0x00, ep1, sizeof(ep1)).data[0], 0);

	/* The device announces itself (broadcast, endpoint 0, profile 0, cluster 0x0013). */
	{
		struct zb_aps_hdr ah = {.type = ZB_APS_DATA, .delivery = ZB_APS_BROADCAST, .dst_ep = 0,
					.cluster = 0x0013, .counter = 50};
		size_t o = zb_aps_build(&ah, aps);

		aps[o++] = 0x07; /* ZDO TSN */
		mt_put_u16(&aps[o], child);
		memcpy(&aps[o + 2], JOINER, 8);
		aps[o + 10] = 0x8E;
		m = mark();
		dev_sends(child, ZB_NWK_DATA, ZB_BCAST_RX_ON_IDLE, aps, o + 11, 100, KEY);
	}
	zassert_true(wait_areq(m, MT_ZDO, 0xC1, 1000, &ev), "no ZDO_END_DEVICE_ANNCE_IND");
	zassert_equal(mt_get_u16(&ev.data[0]), child, "srcaddr");
	zassert_equal(mt_get_u16(&ev.data[2]), child, "nwkaddr");
	zassert_mem_equal(&ev.data[4], JOINER, 8);
	zassert_equal(ev.data[12], 0x8E);

	/* The host sends an On/Off "Off" command with an APS acknowledgement requested. */
	memset(d, 0, sizeof(d));
	d[0] = 2;
	mt_put_u16(&d[1], child);
	d[9] = 1;                    /* destination endpoint */
	d[12] = 1;                   /* source endpoint */
	mt_put_u16(&d[13], 0x0006);  /* On/Off cluster */
	d[15] = 0x21;                /* transaction */
	d[16] = 0x10;                /* APS acknowledgement */
	d[17] = 30;
	mt_put_u16(&d[18], 3);
	d[20] = 0x01;                /* ZCL: cluster specific, client to server */
	d[21] = 0x5A;                /* ZCL sequence */
	d[22] = 0x00;                /* Off */
	txs = mr.tx_count;
	m = mark();
	zassert_equal(sreq(MT_AF, 0x02, d, 23).data[0], 0);
	wait_tx_total(txs + 1);
	zassert_true(open_tx(txs, KEY, &t));
	zassert_equal(t.mac_dst, child);
	zassert_true(t.ar);
	zassert_equal(t.nh.dst, child);
	zassert_true(t.nh.security);
	zassert_equal(t.sh.key_id, ZB_KEYID_NETWORK);
	zassert_mem_equal(t.sh.src, EUI, 8);
	{
		struct zb_aps_hdr ah;

		zassert_ok(zb_aps_parse(t.pl, t.plen, &ah));
		zassert_equal(ah.type, ZB_APS_DATA);
		zassert_true(ah.ack_req);
		zassert_equal(ah.dst_ep, 1);
		zassert_equal(ah.src_ep, 1);
		zassert_equal(ah.cluster, 0x0006);
		zassert_equal(ah.profile, 0x0104, "the source endpoint's profile");
		zassert_equal(t.plen - ah.len, 3);
		zassert_equal(t.pl[ah.len], 0x01);
		zassert_equal(t.pl[ah.len + 2], 0x00);

		/* No confirmation until the device acknowledges at the APS layer. */
		k_sleep(K_MSEC(100));
		zassert_true(find(m, MT_AREQ, MT_AF, 0x80, NULL) < 0);
		{
			struct zb_aps_hdr ack = {.type = ZB_APS_ACK, .dst_ep = 1, .src_ep = 1,
						 .cluster = 0x0006, .profile = 0x0104,
						 .counter = ah.counter};
			size_t o = zb_aps_build(&ack, aps);

			dev_sends(child, ZB_NWK_DATA, 0x0000, aps, o, 101, KEY);
		}
	}
	zassert_true(wait_areq(m, MT_AF, 0x80, 1000, &ev), "no AF_DATA_CONFIRM");
	zassert_equal(ev.data[0], 0x00, "status success");
	zassert_equal(ev.data[1], 1, "endpoint");
	zassert_equal(ev.data[2], 0x21, "transaction");

	/* A send the device never acknowledges is confirmed as APS no-ack. */
	d[15] = 0x22;
	m = mark();
	zassert_equal(sreq(MT_AF, 0x02, d, 23).data[0], 0);
	k_sleep(K_SECONDS(5));
	zassert_true(find(m, MT_AREQ, MT_AF, 0x80, &ev) >= 0);
	zassert_equal(ev.data[0], 0xB7);
	zassert_equal(ev.data[2], 0x22);

	/* A send the MAC cannot deliver is confirmed as MAC no-ack. */
	mr.script[0] = mr.script[1] = mr.script[2] = mr.script[3] = -ENOMSG;
	mr.script_i = 0;
	mr.script_n = 4;
	d[15] = 0x23;
	m = mark();
	zassert_equal(sreq(MT_AF, 0x02, d, 23).data[0], 0);
	zassert_true(find(m, MT_AREQ, MT_AF, 0x80, &ev) >= 0);
	zassert_equal(ev.data[0], 0xE9);

	/* A report from the device: acknowledged at the APS layer and delivered to the host. */
	{
		struct zb_aps_hdr ah = {.type = ZB_APS_DATA, .delivery = ZB_APS_UNICAST, .ack_req = true,
					.dst_ep = 1, .src_ep = 1, .cluster = 0x0006,
					.profile = 0x0104, .counter = 77};
		size_t o = zb_aps_build(&ah, aps);

		aps[o++] = 0x18;
		aps[o++] = 0x33;
		aps[o++] = 0x0A; /* ZCL report attributes */
		txs = mr.tx_count;
		m = mark();
		dev_sends(child, ZB_NWK_DATA, 0x0000, aps, o, 102, KEY);
		(void)wait_data_tx(txs, KEY, &t);
		{
			struct zb_aps_hdr ack;

			zassert_ok(zb_aps_parse(t.pl, t.plen, &ack));
			zassert_equal(ack.type, ZB_APS_ACK);
			zassert_equal(ack.counter, 77);
			zassert_equal(ack.dst_ep, 1);
			zassert_equal(ack.cluster, 0x0006);
		}
		zassert_true(wait_areq(m, MT_AF, 0x81, 1000, &ev), "no AF_INCOMING_MSG");
		zassert_equal(mt_get_u16(&ev.data[2]), 0x0006, "cluster");
		zassert_equal(mt_get_u16(&ev.data[4]), child, "source");
		zassert_equal(ev.data[6], 1, "source endpoint");
		zassert_equal(ev.data[7], 1, "destination endpoint");
		zassert_equal(ev.data[8], 0, "not a broadcast");
		zassert_equal(ev.data[16], 3);
		zassert_equal(ev.data[17], 0x18);
		zassert_equal(ev.data[19], 0x0A);

		/* The same APS counter again (a retransmission): acknowledged, delivered once. */
		m = mark();
		dev_sends(child, ZB_NWK_DATA, 0x0000, aps, o, 103, KEY);
		k_sleep(K_MSEC(200));
		zassert_true(find(m, MT_AREQ, MT_AF, 0x81, NULL) < 0, "duplicate delivered");

		/* A replayed NWK frame counter is dropped before anything else. */
		txs = mr.tx_count;
		dev_sends(child, ZB_NWK_DATA, 0x0000, aps, o, 103, KEY);
		k_sleep(K_MSEC(200));
		zassert_equal(mr.tx_count, txs, "no acknowledgement for a replay");
	}
}

ZTEST(zigbee_stack, test_frames_that_do_not_belong_are_ignored)
{
	static const uint8_t wrong_key[16] = {1, 2, 3};
	uint8_t aps[32];
	uint16_t child;
	int m, txs;
	struct zb_aps_hdr ah = {.type = ZB_APS_DATA, .delivery = ZB_APS_UNICAST, .ack_req = true,
				.dst_ep = 1, .src_ep = 1, .cluster = 0x0006, .profile = 0x0104,
				.counter = 5};
	size_t o;

	child = join_device();
	o = zb_aps_build(&ah, aps);
	aps[o++] = 0x18;
	m = mark();
	txs = mr.tx_count;
	/* Wrong key, a stranger's address, and an unsecured NWK frame. */
	dev_sends(child, ZB_NWK_DATA, 0x0000, aps, o + 1, 200, wrong_key);
	dev_sends(child + 1, ZB_NWK_DATA, 0x0000, aps, o + 1, 201, KEY);
	{
		uint8_t f[ZB_MAC_MAX_FRAME], mf[ZB_MAC_MAX_FRAME];
		struct zb_nwk_hdr nh = {.type = ZB_NWK_DATA, .proto_ver = 2, .dst = 0, .src = child,
					.radius = 30};
		size_t hl = zb_nwk_build(&nh, f);

		memcpy(&f[hl], aps, o + 1);
		zb_mac_rx(&st.mac, mf, zb_mac_build_data(mf, 9, mr.pan, 0, child, true, f, hl + o + 1), 200,
			  -50);
	}
	k_sleep(K_MSEC(300));
	zassert_equal(mr.tx_count, txs);
	zassert_true(find(m, MT_AREQ, MT_AF, 0x81, NULL) < 0);
}

ZTEST(zigbee_stack, test_link_status_and_rejoin)
{
	uint16_t child;
	uint8_t pl[1] = {ZB_NWKC_REJOIN_REQ};
	struct txd t;
	int txs, found = -1;

	child = join_device();

	/* A rejoin request from a known device: the same address, success, secured. */
	txs = mr.tx_count;
	pl[0] = ZB_NWKC_REJOIN_REQ;
	{
		uint8_t req[2] = {ZB_NWKC_REJOIN_REQ, 0x8E};

		dev_sends(child, ZB_NWK_COMMAND, 0x0000, req, sizeof(req), 300, KEY);
	}
	wait_tx_total(txs + 1);
	zassert_true(open_tx(txs, KEY, &t));
	zassert_equal(t.nh.type, ZB_NWK_COMMAND);
	zassert_equal(t.pl[0], ZB_NWKC_REJOIN_RSP);
	zassert_equal(mt_get_u16(&t.pl[1]), child);
	zassert_equal(t.pl[3], 0);

	/* The coordinator announces its neighbours every 15 s. */
	txs = mr.tx_count;
	k_sleep(K_SECONDS(40));
	for (int i = txs; i < mr.tx_count && found < 0; i++) {
		if (open_tx(i, KEY, &t) && t.nh.type == ZB_NWK_COMMAND && t.pl[0] == ZB_NWKC_LINK_STATUS) {
			found = i;
		}
	}
	zassert_true(found >= 0, "no link status in %d frames", mr.tx_count - txs);
	zassert_true(open_tx(found, KEY, &t));
	zassert_equal(t.nh.dst, ZB_BCAST_ROUTERS);
	zassert_equal(t.nh.radius, 1);
	zassert_equal(t.mac_dst, 0xFFFF);
	zassert_false(t.ar);
	zassert_equal(t.pl[1] & 0x1F, 1, "one neighbour");
	zassert_true((t.pl[1] & 0x60) == 0x60, "first and last frame");
	zassert_equal(mt_get_u16(&t.pl[2]), child);
	zassert_equal(t.pl[4], 0x11);
}

ZTEST(zigbee_stack, test_counters_are_written_ahead_and_survive_a_reset)
{
	uint8_t sm[ZB_SECMAT_LEN], b[4];
	uint32_t first, after;
	struct txd t;
	uint16_t child;
	struct mt_frame ev;
	int txs, m;

	child = join_device();
	/* The first secured NWK frame raises the stored limit above the counters in use. */
	txs = mr.tx_count;
	{
		uint8_t req[2] = {ZB_NWKC_REJOIN_REQ, 0x8E};

		dev_sends(child, ZB_NWK_COMMAND, 0x0000, req, sizeof(req), 399, KEY);
	}
	wait_tx_total(txs + 1);
	ex_read(ZB_NVEX_NWK_SEC_MATERIAL, 0, sm);
	first = mt_get_u32(sm);
	zassert_true(first >= 1024U, "the stored value is ahead of the counters in use");

	/* After a reset the counters continue from the stored value, never from behind. */
	m = mark();
	areq(MT_SYS, 0x00, (uint8_t[]){1}, 1);
	zassert_true(wait_areq(m, MT_SYS, 0x80, 1000, &ev));
	txs = mr.tx_count;
	(void)b;
	{
		uint8_t req[2] = {ZB_NWKC_REJOIN_REQ, 0x8E};

		dev_sends(child, ZB_NWK_COMMAND, 0x0000, req, sizeof(req), 400, KEY);
	}
	wait_tx_total(txs + 1);
	zassert_true(open_tx(txs, KEY, &t));
	after = t.sh.counter;
	zassert_true(after >= first, "counter %u went back below the stored %u", after, first);
}
