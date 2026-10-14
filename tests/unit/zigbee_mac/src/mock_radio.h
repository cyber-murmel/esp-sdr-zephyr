/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A mock 802.15.4 radio for the Zigbee unit tests: logs what the MAC transmits, returns
 * scripted TX results and energy values, and lets nearby networks answer beacon requests.
 */
#ifndef MOCK_RADIO_H_
#define MOCK_RADIO_H_

#include <errno.h>
#include <string.h>

#include <zigbee/zb_mac.h>

#define MAX_TX 32

struct mock_net {
	uint8_t channel;
	uint16_t pan;
	bool permit;
};

struct mock_radio {
	uint8_t chan;
	uint16_t pan, short_addr;
	uint8_t ext[8];
	bool coordinator, promisc;
	int starts;
	int8_t txpower;
	int txpower_calls;

	struct {
		uint8_t f[ZB_MAC_MAX_FRAME];
		size_t n;
		bool csma;
		uint8_t chan;
		bool promisc;
	} tx_log[MAX_TX];
	int tx_count;
	struct k_sem tx_sem;

	/* Results the next tx() calls return before it starts succeeding (consumed in order). */
	int script[8];
	int script_n, script_i;

	int8_t ed_dbm[27];
	int ed_calls;

	/* Nearby networks that answer beacon requests on their channel. */
	struct mock_net nets[4];
	int nnets;
	struct zb_mac *mac;
};

static struct mock_radio mr;

static int m_set_channel(void *c, uint8_t ch)
{
	mr.chan = ch;
	return 0;
}

static int m_set_ids(void *c, uint16_t pan, uint16_t s, const uint8_t ext[8])
{
	mr.pan = pan;
	mr.short_addr = s;
	memcpy(mr.ext, ext, 8);
	return 0;
}

static int m_set_mode(void *c, bool coord, bool promisc)
{
	mr.coordinator = coord;
	mr.promisc = promisc;
	return 0;
}

static int m_set_txpower(void *c, int8_t dbm)
{
	mr.txpower = dbm;
	mr.txpower_calls++;
	return 0;
}

static int m_start(void *c)
{
	mr.starts++;
	return 0;
}

static int m_tx(void *c, const uint8_t *f, size_t n, bool csma)
{
	int i = mr.tx_count;

	if (i < MAX_TX) {
		memcpy(mr.tx_log[i].f, f, n);
		mr.tx_log[i].n = n;
		mr.tx_log[i].csma = csma;
		mr.tx_log[i].chan = mr.chan;
		mr.tx_log[i].promisc = mr.promisc;
		mr.tx_count++;
	}
	k_sem_give(&mr.tx_sem);

	/* A beacon request makes the networks on this channel beacon. */
	struct zb_mac_hdr h;

	if (zb_mac_parse(f, n, &h) == 0 && h.type == ZB_FT_CMD && f[h.len] == ZB_CMD_BEACON_REQ) {
		for (int k = 0; k < mr.nnets; k++) {
			if (mr.nets[k].channel == mr.chan) {
				struct zb_beacon_info b = {
					.pan = mr.nets[k].pan, .src_short = 0x0000, .permit = mr.nets[k].permit,
					.pan_coord = true, .stack_profile = 2, .proto_ver = 2, .router_cap = true,
					.ed_cap = true, .upd = 3,
				};
				uint8_t bf[ZB_MAC_MAX_FRAME];
				size_t bn;

				memset(b.epid, 0xA0 + k, 8);
				bn = zb_mac_build_beacon(bf, 77, &b);
				zb_mac_rx(mr.mac, bf, bn, 200 - 10 * k, -60);
			}
		}
	}

	if (mr.script_i < mr.script_n) {
		return mr.script[mr.script_i++];
	}
	return 0;
}

static int m_ed(void *c, uint16_t ms, int8_t *dbm)
{
	mr.ed_calls++;
	*dbm = mr.ed_dbm[mr.chan];
	return 0;
}

static const struct zb_radio_ops mock_ops = {
	.set_channel = m_set_channel,
	.set_ids = m_set_ids,
	.set_mode = m_set_mode,
	.set_txpower = m_set_txpower,
	.start = m_start,
	.tx = m_tx,
	.ed = m_ed,
};

#endif /* MOCK_RADIO_H_ */
