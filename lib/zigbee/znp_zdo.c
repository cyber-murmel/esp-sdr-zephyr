/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * MT handlers of the Zigbee stack: APP_CNF (BDB commissioning), ZDO (the coordinator's own
 * descriptors, scans, permit join), AF (endpoint registry) and DEBUG. Layouts follow
 * zigpy-znp and zigbee-herdsman (tmp/zstack/phase2/zigpy_sequence.md, herdsman.md).
 */
#include <errno.h>
#include <string.h>

#include <zigbee/zb_stack.h>

enum {
	ZDO_NWK_ADDR_REQ = 0x00,
	ZDO_IEEE_ADDR_REQ = 0x01,
	ZDO_NODE_DESC_REQ = 0x02,
	ZDO_POWER_DESC_REQ = 0x03,
	ZDO_SIMPLE_DESC_REQ = 0x04,
	ZDO_ACTIVE_EP_REQ = 0x05,
	ZDO_MATCH_DESC_REQ = 0x06,
	ZDO_NWK_DISCOVERY_REQ = 0x26,
	ZDO_MGMT_LQI_REQ = 0x31,
	ZDO_MGMT_RTG_REQ = 0x32,
	ZDO_MGMT_BIND_REQ = 0x33,
	ZDO_MGMT_LEAVE_REQ = 0x34,
	ZDO_MGMT_PERMIT_JOIN_REQ = 0x36,
	ZDO_MGMT_NWK_UPDATE_REQ = 0x37,
	ZDO_MSG_CB_REGISTER = 0x3E,
	ZDO_STARTUP_FROM_APP = 0x40,
	ZDO_EXT_FIND_GROUP = 0x4A,
	ZDO_EXT_ADD_GROUP = 0x4B,
	ZDO_EXT_ROUTE_DISC = 0x45,
	ZDO_EXT_ROUTE_CHK = 0x46,
	ZDO_EXT_NWK_INFO = 0x50,

	ZDO_NODE_DESC_RSP = 0x82,
	ZDO_SIMPLE_DESC_RSP = 0x84,
	ZDO_ACTIVE_EP_RSP = 0x85,
	ZDO_MGMT_PERMIT_JOIN_RSP = 0xB6,
	ZDO_MGMT_NWK_UPDATE_NOTIFY = 0xB8,
	ZDO_STATE_CHANGE_IND = 0xC0,
	ZDO_BEACON_NOTIFY_IND = 0xC5,
	ZDO_NWK_DISCOVERY_CNF = 0xC7,
	ZDO_PERMIT_JOIN_IND = 0xCB,
	ZDO_TC_DEV_IND = 0xCA,
	ZDO_END_DEVICE_ANNCE_IND = 0xC1,
	ZDO_MSG_CB_INCOMING = 0xFF,
};

enum {
	AF_REGISTER = 0x00,
	AF_DATA_REQUEST = 0x01,
	AF_DATA_REQUEST_EXT = 0x02,
	AF_DATA_CONFIRM = 0x80,
	AF_INCOMING_MSG = 0x81,
};

enum {
	APP_CNF_BDB_START_COMMISSIONING = 0x05,
	APP_CNF_BDB_SET_CHANNEL = 0x08,
	APP_CNF_BDB_COMMISSIONING_NOTIFICATION = 0x80,
};

/* ZDO cluster ids of the requests the coordinator answers about itself. */
enum {
	ZCL_NWK_ADDR_REQ = 0x0000,
	ZCL_IEEE_ADDR_REQ = 0x0001,
	ZCL_NODE_DESC_REQ = 0x0002,
	ZCL_POWER_DESC_REQ = 0x0003,
	ZCL_SIMPLE_DESC_REQ = 0x0004,
	ZCL_ACTIVE_EP_REQ = 0x0005,
	ZCL_MATCH_DESC_REQ = 0x0006,
	ZCL_MGMT_LQI_REQ = 0x0031,
	ZCL_MGMT_RTG_REQ = 0x0032,
	ZCL_MGMT_BIND_REQ = 0x0033,
	ZCL_MGMT_LEAVE_REQ = 0x0034,
	ZCL_MGMT_PERMIT_JOIN_REQ = 0x0036,
	ZCL_MGMT_NWK_UPDATE_REQ = 0x0038,
	ZCL_DEVICE_ANNCE = 0x0013,
	ZCL_RSP_BIT = 0x8000,
	ZCL_NWK_ADDR_RSP = 0x8000,
	ZCL_IEEE_ADDR_RSP = 0x8001,
};

/* Scan duration values of Mgmt_NWK_Update_req that are no scan. */
#define NWK_UPDATE_CHANNEL_CHANGE 0xFE
#define NWK_UPDATE_SET_MASK       0xFF
#define NWK_UPDATE_SCAN_MAX       5

/* AF/ZDO status bytes. */
#define STATUS_OK            0x00
#define STATUS_FAIL          0x01
#define STATUS_INVALID_PARAM 0x02
#define STATUS_BUFFER_FULL   0x11
#define ZDO_INVALID_EP       0x82
#define ZDO_NOT_SUPPORTED    0x84
#define AF_NWK_NO_ROUTE      0xCD
#define ROUTING_NO_ENTRY     4

#define NWK_COORDINATOR 0x0000

/* AF_DATA_REQUEST_EXT address modes and options. */
#define AF_ADDR_NWK            0x02
#define AF_ADDR_BROADCAST      0x0F
#define AF_OPTION_ACK_REQUEST  0x10
#define NWK_BROADCAST_ROUTERS 0xFFFC

#define DEVICE_TYPE_COORDINATOR 1

static struct zb_stack *stk(struct znp *z)
{
	return z->stack;
}

static int status_rsp(struct mt_frame *rsp, uint8_t status)
{
	rsp->data[0] = status;
	rsp->len = 1;
	return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* Descriptors of the coordinator itself */

#define NODE_DESC_LEN 13

/* Logical type coordinator, 2.4 GHz, FFD on mains with receiver on, TI manufacturer code. */
static void node_desc(uint8_t d[NODE_DESC_LEN])
{
	static const uint8_t desc[NODE_DESC_LEN] = {
		0x00,       /* logical type 0 (coordinator), no complex/user descriptor */
		0x40,       /* APS flags 0, frequency band: 2.4 GHz */
		0x8F,       /* MAC capability: alt coordinator, FFD, mains, rx on idle, allocate */
		0x35, 0x11, /* manufacturer code 0x1135 */
		0x52,       /* maximum buffer size */
		0x52, 0x00, /* maximum incoming transfer size */
		0x00, 0x2C, /* server mask: stack compliance revision 22 */
		0x52, 0x00, /* maximum outgoing transfer size */
		0x00,       /* descriptor capability */
	};

	memcpy(d, desc, sizeof(desc));
}

/* Active endpoint list body: status, nwk, count, endpoints. Returns the length. */
static size_t body_active_ep(struct zb_stack *s, uint8_t *out)
{
	uint8_t n = 0;

	out[0] = STATUS_OK;
	mt_put_u16(&out[1], NWK_COORDINATOR);
	for (int i = 0; i < ZB_EP_MAX; i++) {
		if (s->eps[i].used) {
			out[4 + n++] = s->eps[i].ep;
		}
	}
	out[3] = n;
	return 4U + n;
}

static size_t body_node_desc(uint8_t *out)
{
	out[0] = STATUS_OK;
	mt_put_u16(&out[1], NWK_COORDINATOR);
	node_desc(&out[3]);
	return 3U + NODE_DESC_LEN;
}

static size_t body_simple_desc(struct zb_stack *s, uint8_t ep, uint8_t *out)
{
	const struct zb_ep *e = NULL;
	size_t o;

	for (int i = 0; i < ZB_EP_MAX; i++) {
		if (s->eps[i].used && s->eps[i].ep == ep) {
			e = &s->eps[i];
		}
	}
	mt_put_u16(&out[1], NWK_COORDINATOR);
	if (e == NULL) {
		out[0] = ZDO_INVALID_EP;
		out[3] = 0;
		return 4;
	}
	out[0] = STATUS_OK;
	o = 4;
	out[o++] = e->ep;
	mt_put_u16(&out[o], e->profile);
	mt_put_u16(&out[o + 2], e->device);
	o += 4;
	out[o++] = e->version & 0x0F;
	out[o++] = e->nin;
	for (int i = 0; i < e->nin; i++, o += 2) {
		mt_put_u16(&out[o], e->in[i]);
	}
	out[o++] = e->nout;
	for (int i = 0; i < e->nout; i++, o += 2) {
		mt_put_u16(&out[o], e->out[i]);
	}
	out[3] = (uint8_t)(o - 4);
	return o;
}

static size_t body_energy(int status, uint32_t mask, const uint8_t *ed, size_t n, uint8_t *out)
{
	out[0] = status == 0 ? STATUS_OK : STATUS_FAIL;
	mt_put_u32(&out[1], mask);
	mt_put_u16(&out[5], 0); /* total transmissions */
	mt_put_u16(&out[7], 0); /* transmission failures */
	out[9] = (uint8_t)n;
	memcpy(&out[10], ed, n);
	return 10U + n;
}

/* A ZDO message as the host's message callback sees it (@p cluster as on air). */
static void msg_cb_incoming(struct zb_stack *s, uint16_t src, bool bcast, uint16_t cluster,
			    uint8_t tsn, const uint8_t *body, size_t len, bool after)
{
	uint8_t d[9 + 100];

	if (len > 100U) {
		return;
	}
	mt_put_u16(&d[0], src);
	d[2] = bcast ? 1 : 0;
	mt_put_u16(&d[3], cluster);
	d[5] = 0; /* security use */
	d[6] = tsn;
	mt_put_u16(&d[7], NWK_COORDINATOR);
	memcpy(&d[9], body, len);
	if (after) {
		znp_send_areq_after(s->znp, MT_ZDO, ZDO_MSG_CB_INCOMING, d, (uint8_t)(9U + len));
	} else {
		znp_send_areq(s->znp, MT_ZDO, ZDO_MSG_CB_INCOMING, d, (uint8_t)(9U + len));
	}
}

/* ------------------------------------------------------------------------------------------ */
/* Events from the net layer, to the host */

static void ev_state(void *ctx, uint8_t st)
{
	struct zb_stack *s = ctx;

	znp_send_areq(s->znp, MT_ZDO, ZDO_STATE_CHANGE_IND, &st, 1);
}

static void ev_bdb(void *ctx, uint8_t status, uint8_t mode, uint8_t remaining)
{
	struct zb_stack *s = ctx;
	const uint8_t d[] = {status, mode, remaining};

	znp_send_areq(s->znp, MT_APP_CNF, APP_CNF_BDB_COMMISSIONING_NOTIFICATION, d, sizeof(d));
}

static void ev_beacons(void *ctx, const struct zb_beacon_info *b, size_t n)
{
	struct zb_stack *s = ctx;
	uint8_t d[1 + 8 * 21];
	uint8_t *p = &d[1];

	d[0] = (uint8_t)n;
	for (size_t i = 0; i < n; i++, p += 21) {
		mt_put_u16(&p[0], b[i].src_short);
		mt_put_u16(&p[2], b[i].pan);
		p[4] = b[i].channel;
		p[5] = b[i].permit ? 1 : 0;
		p[6] = b[i].router_cap ? 1 : 0;
		p[7] = b[i].ed_cap ? 1 : 0;
		p[8] = b[i].proto_ver;
		p[9] = b[i].stack_profile;
		p[10] = b[i].lqi;
		p[11] = b[i].depth;
		p[12] = b[i].upd;
		memcpy(&p[13], b[i].epid, 8);
	}
	znp_send_areq(s->znp, MT_ZDO, ZDO_BEACON_NOTIFY_IND, d, (uint8_t)(1U + 21U * n));
}

static void ev_discovery_done(void *ctx, uint8_t status)
{
	struct zb_stack *s = ctx;

	znp_send_areq(s->znp, MT_ZDO, ZDO_NWK_DISCOVERY_CNF, &status, 1);
}

/* The tag of an energy scan: a ZDO response through the message callback, or the MT AREQ. */
#define ESCAN_TAG_LOOPBACK BIT(8)

static void ev_energy_done(void *ctx, int status, uint32_t mask, const uint8_t *ed, size_t n,
			   uint32_t tag)
{
	struct zb_stack *s = ctx;
	uint8_t body[10 + 16];
	size_t len = body_energy(status, mask, ed, n, body);

	if ((tag & ESCAN_TAG_LOOPBACK) != 0U) {
		msg_cb_incoming(s, NWK_COORDINATOR, false, ZCL_MGMT_NWK_UPDATE_REQ | ZCL_RSP_BIT,
				(uint8_t)tag, body, len, false);
	} else {
		uint8_t d[2 + 10 + 16];

		mt_put_u16(d, NWK_COORDINATOR);
		memcpy(&d[2], body, len);
		znp_send_areq(s->znp, MT_ZDO, ZDO_MGMT_NWK_UPDATE_NOTIFY, d, (uint8_t)(2U + len));
	}
}

static void ev_permit(void *ctx, uint8_t duration)
{
	struct zb_stack *s = ctx;

	znp_send_areq(s->znp, MT_ZDO, ZDO_PERMIT_JOIN_IND, &duration, 1);
}

static void ev_tc_join(void *ctx, uint16_t nwk, const uint8_t ext[8], uint16_t parent)
{
	struct zb_stack *s = ctx;
	uint8_t d[12];

	mt_put_u16(d, nwk);
	memcpy(&d[2], ext, 8);
	mt_put_u16(&d[10], parent);
	znp_send_areq_after(s->znp, MT_ZDO, ZDO_TC_DEV_IND, d, sizeof(d));
}

static void ev_data_confirm(void *ctx, uint8_t status, uint32_t tag)
{
	struct zb_stack *s = ctx;
	const uint8_t d[] = {status, (uint8_t)(tag >> 8), (uint8_t)tag};

	znp_send_areq_after(s->znp, MT_AF, AF_DATA_CONFIRM, d, sizeof(d));
}

/* A ZDO frame from a device: responses and the device announcement, in the host's style. */
static void zdo_incoming(struct zb_stack *s, const struct zb_aps_rx *rx)
{
	const uint8_t *body = &rx->data[1];
	size_t len = rx->len - 1U;
	uint8_t tsn = rx->data[0];
	uint8_t d[2 + 100];

	if (rx->len < 1U) {
		return;
	}
	if (rx->cluster == ZCL_DEVICE_ANNCE && len >= 11U) {
		/* srcaddr, nwkaddr, IEEE address, capabilities */
		mt_put_u16(d, rx->src);
		memcpy(&d[2], body, 11);
		znp_send_areq_after(s->znp, MT_ZDO, ZDO_END_DEVICE_ANNCE_IND, d, 13);
	}
	if (s->msg_cb) {
		msg_cb_incoming(s, rx->src, rx->broadcast, rx->cluster, tsn, body, len, true);
		return;
	}
	if ((rx->cluster & ZCL_RSP_BIT) == 0U || len > 98U) {
		return;
	}
	if (rx->cluster == ZCL_NWK_ADDR_RSP || rx->cluster == ZCL_IEEE_ADDR_RSP) {
		/* No source address; Z-Stack puts the start index before the count. */
		memcpy(d, body, len);
		if (len >= 13U) {
			uint8_t t = d[11];

			d[11] = d[12];
			d[12] = t;
		} else if (len == 11U) {
			d[11] = 0;
			d[12] = 0;
			len = 13;
		}
		znp_send_areq_after(s->znp, MT_ZDO, (uint8_t)(0x80U + (rx->cluster & 0xFFU)), d, (uint8_t)len);
		return;
	}
	mt_put_u16(d, rx->src);
	memcpy(&d[2], body, len);
	znp_send_areq_after(s->znp, MT_ZDO, (uint8_t)(0x80U + (rx->cluster & 0xFFU)), d, (uint8_t)(2U + len));
}

/* An application frame: AF_INCOMING_MSG. */
static void af_incoming(struct zb_stack *s, const struct zb_aps_rx *rx)
{
	uint8_t d[17 + ZB_APS_MAX_PAYLOAD];

	mt_put_u16(&d[0], 0); /* group */
	mt_put_u16(&d[2], rx->cluster);
	mt_put_u16(&d[4], rx->src);
	d[6] = rx->src_ep;
	d[7] = rx->dst_ep;
	d[8] = rx->broadcast ? 1 : 0;
	d[9] = rx->lqi;
	d[10] = 0; /* security use */
	mt_put_u32(&d[11], (uint32_t)k_uptime_get());
	d[15] = rx->aps_counter;
	d[16] = rx->len;
	memcpy(&d[17], rx->data, rx->len);
	znp_send_areq_after(s->znp, MT_AF, AF_INCOMING_MSG, d, (uint8_t)(17U + rx->len));
}

static void ev_aps_rx(void *ctx, const struct zb_aps_rx *rx)
{
	struct zb_stack *s = ctx;

	if (rx->dst_ep == 0U && rx->profile == 0U) {
		zdo_incoming(s, rx);
	} else {
		af_incoming(s, rx);
	}
}

static void ev_trace(void *ctx, const char *msg)
{
	zb_stack_trace(ctx, 1, "%s", msg);
}

const struct zb_net_cb zb_stack_net_cb = {
	.trace = ev_trace,
	.tc_join = ev_tc_join,
	.aps_rx = ev_aps_rx,
	.data_confirm = ev_data_confirm,
	.state = ev_state,
	.bdb = ev_bdb,
	.beacons = ev_beacons,
	.discovery_done = ev_discovery_done,
	.energy_done = ev_energy_done,
	.permit = ev_permit,
};

/* ------------------------------------------------------------------------------------------ */
/* APP_CNF */

int znp_appcnf_handler(struct znp *z, const struct mt_frame *req, struct mt_frame *rsp)
{
	struct zb_stack *s = stk(z);

	switch (req->cmd) {
	case APP_CNF_BDB_SET_CHANNEL:
		if (req->len != 5U) {
			return MT_ERR_LENGTH;
		}
		zb_net_bdb_set_channel(&s->net, req->data[0] != 0U, mt_get_u32(&req->data[1]));
		return status_rsp(rsp, STATUS_OK);
	case APP_CNF_BDB_START_COMMISSIONING:
		if (req->len != 1U) {
			return MT_ERR_LENGTH;
		}
		return status_rsp(rsp, zb_net_bdb_start(&s->net, req->data[0]) == 0 ? STATUS_OK
										      : STATUS_FAIL);
	}
	return MT_ERR_COMMAND;
}

/* ------------------------------------------------------------------------------------------ */
/* DEBUG */

int znp_debug_handler(struct znp *z, const struct mt_frame *req, struct mt_frame *rsp)
{
	if (req->cmd == 0x00) { /* DEBUG_SET_THRESHOLD: component, threshold */
		if (req->len != 2U) {
			return MT_ERR_LENGTH;
		}
		stk(z)->trace_level = req->data[1];
		return status_rsp(rsp, STATUS_OK);
	}
	return MT_ERR_COMMAND;
}

/* ------------------------------------------------------------------------------------------ */
/* ZDO */

/* The ZDO request is meant for this device (or everyone). */
static bool for_me(uint16_t dst)
{
	return dst == NWK_COORDINATOR || dst == 0xFFFF || dst == 0xFFFD || dst == NWK_BROADCAST_ROUTERS;
}

static void permit_from_zdo(struct zb_stack *s, uint8_t duration)
{
	(void)zb_net_permit(&s->net, duration);
}

/* Both address responses carry this: status, IEEE address, short address of the coordinator. */
static size_t body_addr(struct zb_stack *s, bool mt, uint8_t reqtype, uint8_t start, uint8_t *out)
{
	size_t o = 0;
	uint8_t list[16], n = 0;

	out[o++] = STATUS_OK;
	memcpy(&out[o], zb_net_eui(&s->net), 8);
	o += 8;
	mt_put_u16(&out[o], NWK_COORDINATOR);
	o += 2;
	if (reqtype == 1U) {
		/* Extended request: the children, from the start index. */
		uint8_t k = 0;

		for (int i = 0; i < ZB_NET_MAX_CHILDREN && n < sizeof(list) / 2U; i++) {
			if (s->net.children[i].used && !s->net.children[i].tentative && k++ >= start) {
				mt_put_u16(&list[2U * n++], s->net.children[i].nwk);
			}
		}
	}
	if (mt) {
		/* Z-Stack's MT order: start index before the count. */
		out[o++] = start;
		out[o++] = n;
	} else if (reqtype == 1U) {
		out[o++] = n;
		out[o++] = start;
	}
	memcpy(&out[o], list, 2U * n);
	return o + 2U * n;
}

/*
 * The body of the response to a ZDO request about this device, @p p the request payload
 * (after the transaction number). Returns its length; 0 when no response is due (a broadcast
 * that does not concern this device).
 */
static size_t zdo_body(struct zb_stack *s, uint16_t cluster, const uint8_t *p, size_t len,
		       bool unicast, bool mt, uint8_t *out)
{
	switch (cluster) {
	case ZCL_NWK_ADDR_REQ: /* IEEE address, request type, start index */
		if (len < 10U || memcmp(p, zb_net_eui(&s->net), 8) != 0) {
			return 0;
		}
		return body_addr(s, mt, p[8], p[9], out);
	case ZCL_IEEE_ADDR_REQ: /* short address, request type, start index */
		if (len < 4U || mt_get_u16(p) != NWK_COORDINATOR) {
			return 0;
		}
		return body_addr(s, mt, p[2], p[3], out);
	case ZCL_NODE_DESC_REQ:
		return body_node_desc(out);
	case ZCL_POWER_DESC_REQ:
		out[0] = STATUS_OK;
		mt_put_u16(&out[1], NWK_COORDINATOR);
		out[3] = 0x10; /* receiver on when idle, mains available */
		out[4] = 0xC1; /* mains, full level */
		return 5;
	case ZCL_ACTIVE_EP_REQ:
		return body_active_ep(s, out);
	case ZCL_SIMPLE_DESC_REQ:
		return len >= 3U ? body_simple_desc(s, p[2], out) : 0U;
	case ZCL_MATCH_DESC_REQ: { /* short address, profile, input clusters, output clusters */
		uint16_t profile;
		uint8_t nin, nout, found = 0;
		const uint8_t *in, *outc;

		if (len < 6U) {
			return 0;
		}
		profile = mt_get_u16(&p[2]);
		nin = p[4];
		if (len < 5U + 2U * nin + 1U) {
			return 0;
		}
		in = &p[5];
		nout = p[5U + 2U * nin];
		if (len < 6U + 2U * nin + 2U * nout) {
			return 0;
		}
		outc = &p[6U + 2U * nin];
		out[0] = STATUS_OK;
		mt_put_u16(&out[1], NWK_COORDINATOR);
		for (int i = 0; i < ZB_EP_MAX && found < 16U; i++) {
			const struct zb_ep *e = &s->eps[i];
			bool hit = false;

			if (!e->used || (profile != 0xFFFFU && profile != e->profile)) {
				continue;
			}
			/* A match is a cluster the endpoint serves that the requester lists as input
			 * or one it uses that the requester lists as output.
			 */
			for (int a = 0; a < nin && !hit; a++) {
				for (int b = 0; b < e->nin && !hit; b++) {
					hit = mt_get_u16(&in[2 * a]) == e->in[b];
				}
			}
			for (int a = 0; a < nout && !hit; a++) {
				for (int b = 0; b < e->nout && !hit; b++) {
					hit = mt_get_u16(&outc[2 * a]) == e->out[b];
				}
			}
			if (hit) {
				out[4U + found++] = e->ep;
			}
		}
		out[3] = found;
		return found == 0U && !unicast ? 0U : 4U + found;
	}
	case ZCL_MGMT_LQI_REQ:
	case ZCL_MGMT_RTG_REQ:
	case ZCL_MGMT_BIND_REQ:
		/* Tables with no entries yet: status, total, start index, count. */
		if (len < 1U) {
			return 0;
		}
		out[0] = STATUS_OK;
		out[1] = 0;
		out[2] = p[0];
		out[3] = 0;
		return 4;
	default:
		/* A request this firmware does not serve: say so rather than leave the host waiting. */
		if (!unicast || (cluster & ZCL_RSP_BIT) != 0U) {
			return 0;
		}
		out[0] = ZDO_NOT_SUPPORTED;
		mt_put_u16(&out[1], NWK_COORDINATOR);
		return 3;
	}
}

/* The AREQ that carries a ZDO response when ZDO_DIRECT_CB is on (0x80 + the cluster's low byte). */
static void zdo_areq_rsp(struct zb_stack *s, uint16_t cluster, const uint8_t *body, size_t len)
{
	uint8_t d[2 + 64];
	size_t skip = cluster == ZCL_NWK_ADDR_REQ || cluster == ZCL_IEEE_ADDR_REQ ? 0U : 2U;

	/* Everything but the two address responses starts with the source address. */
	mt_put_u16(d, NWK_COORDINATOR);
	memcpy(&d[skip], body, len);
	znp_send_areq_after(s->znp, MT_ZDO, (uint8_t)(0x80U + (cluster & 0xFFU)), d, (uint8_t)(skip + len));
}

/* A ZDO request to another device (or a broadcast): [TSN][payload] as an APS frame to endpoint 0. */
static int zdo_send(struct zb_stack *s, uint16_t dst, uint16_t cluster, const uint8_t *payload,
		    size_t len)
{
	uint8_t d[ZB_APS_MAX_PAYLOAD];
	struct zb_aps_tx tx = {
		.dst = dst,
		.broadcast = dst >= NWK_BROADCAST_ROUTERS,
		.cluster = cluster,
		.data = d,
		.len = 1U + len,
	};

	if (len + 1U > sizeof(d)) {
		return -EMSGSIZE;
	}
	d[0] = s->zdo_tsn++;
	memcpy(&d[1], payload, len);
	return zb_net_send(&s->net, &tx);
}

static int send_status(struct mt_frame *rsp, int r)
{
	return status_rsp(rsp, r == 0 ? STATUS_OK : r == -EMSGSIZE ? STATUS_INVALID_PARAM : STATUS_FAIL);
}

/*
 * A ZDO request over MT: the payload is the destination, then the ZDO request without its TSN.
 * About this device it is answered here, otherwise it goes over the air.
 */
static int zdo_mt_request(struct zb_stack *s, uint16_t cluster, const uint8_t *d, size_t len,
			  struct mt_frame *rsp)
{
	/* Requests about a device: the short address of interest follows the destination. */
	bool about_device = cluster >= ZCL_NODE_DESC_REQ && cluster <= ZCL_MATCH_DESC_REQ;
	uint8_t body[64];
	uint16_t dst;
	size_t n;

	if (len < 2U) {
		return MT_ERR_LENGTH;
	}
	dst = mt_get_u16(d);
	if (for_me(dst) && (!about_device || len < 4U || mt_get_u16(&d[2]) == NWK_COORDINATOR)) {
		n = zdo_body(s, cluster, d + 2, len - 2U, dst == NWK_COORDINATOR, true, body);
		if (n > 0U) {
			zdo_areq_rsp(s, cluster, body, n);
		}
		return status_rsp(rsp, STATUS_OK);
	}
	return send_status(rsp, zdo_send(s, dst, cluster, d + 2, len - 2U));
}

int znp_zdo_handler(struct znp *z, const struct mt_frame *req, struct mt_frame *rsp)
{
	struct zb_stack *s = stk(z);
	const uint8_t *d = req->data;
	uint8_t body[64];
	size_t n;

	switch (req->cmd) {
	case ZDO_NODE_DESC_REQ:
	case ZDO_POWER_DESC_REQ:
	case ZDO_ACTIVE_EP_REQ:
		/* destination, then the short address of interest */
		if (req->len != 4U) {
			return MT_ERR_LENGTH;
		}
		return zdo_mt_request(s, req->cmd, d, req->len, rsp);
	case ZDO_SIMPLE_DESC_REQ:
		if (req->len != 5U) {
			return MT_ERR_LENGTH;
		}
		return zdo_mt_request(s, req->cmd, d, req->len, rsp);
	case ZDO_MATCH_DESC_REQ:
	case ZDO_MGMT_LQI_REQ:
	case ZDO_MGMT_RTG_REQ:
	case ZDO_MGMT_BIND_REQ:
	case ZDO_MGMT_LEAVE_REQ:
		if (req->len < 3U) {
			return MT_ERR_LENGTH;
		}
		return zdo_mt_request(s, req->cmd, d, req->len, rsp);
	case ZDO_NWK_ADDR_REQ:
	case ZDO_IEEE_ADDR_REQ:
		/* NWK_ADDR: IEEE address, request type, start index (broadcast to the network).
		 * IEEE_ADDR: the short address first, answered by that device.
		 */
		if (req->len < 4U) {
			return MT_ERR_LENGTH;
		}
		n = zdo_body(s, req->cmd, d, req->len, true, true, body);
		if (n > 0U) {
			zdo_areq_rsp(s, req->cmd, body, n);
			return status_rsp(rsp, STATUS_OK);
		}
		return send_status(rsp, zdo_send(s, req->cmd == ZDO_NWK_ADDR_REQ
							    ? ZB_BCAST_RX_ON_IDLE
							    : mt_get_u16(d),
						 req->cmd, d, req->len));
	case ZDO_NWK_DISCOVERY_REQ:
		if (req->len != 5U) {
			return MT_ERR_LENGTH;
		}
		if (d[4] > ZB_MAC_SCAN_EXP_MAX) {
			return status_rsp(rsp, STATUS_INVALID_PARAM);
		}
		return status_rsp(rsp, zb_net_discover(&s->net, mt_get_u32(d), d[4]) == 0 ? STATUS_OK
											  : STATUS_FAIL);
	case ZDO_MGMT_PERMIT_JOIN_REQ: {
		/* address mode, destination, duration, TC significance */
		uint16_t dst;

		if (req->len != 5U) {
			return MT_ERR_LENGTH;
		}
		dst = mt_get_u16(&d[1]);
		if (for_me(dst)) {
			permit_from_zdo(s, d[3]);
			body[0] = STATUS_OK;
			zdo_areq_rsp(s, ZCL_MGMT_PERMIT_JOIN_REQ, body, 1);
		}
		if (dst == NWK_COORDINATOR) {
			return status_rsp(rsp, STATUS_OK);
		}
		/* Other devices and the routers around: the request goes over the air too. */
		return send_status(rsp, zdo_send(s, dst, ZCL_MGMT_PERMIT_JOIN_REQ, &d[3], 2));
	}
	case ZDO_MGMT_NWK_UPDATE_REQ: {
		/* destination, address mode, channels, scan duration, scan count, manager */
		uint32_t mask;

		if (req->len != 11U) {
			return MT_ERR_LENGTH;
		}
		if (!for_me(mt_get_u16(d))) {
			return status_rsp(rsp, STATUS_FAIL);
		}
		mask = mt_get_u32(&d[3]);
		if (d[7] == NWK_UPDATE_CHANNEL_CHANGE) {
			if (__builtin_popcount(mask) != 1 || (mask & ~ZB_ALL_CHANNELS) != 0U) {
				return status_rsp(rsp, STATUS_INVALID_PARAM);
			}
			return status_rsp(rsp, zb_net_change_channel(&s->net, (uint8_t)(__builtin_ctz(mask)))
							       == 0 ? STATUS_OK : STATUS_FAIL);
		}
		if (d[7] == NWK_UPDATE_SET_MASK) {
			return status_rsp(rsp, zb_net_set_channel_mask(&s->net, mask, mt_get_u16(&d[9])) == 0
						       ? STATUS_OK
						       : STATUS_FAIL);
		}
		if (d[7] > NWK_UPDATE_SCAN_MAX) {
			return status_rsp(rsp, STATUS_INVALID_PARAM);
		}
		return status_rsp(rsp, zb_net_energy_scan(&s->net, mask, d[7], 0) == 0 ? STATUS_OK
										       : STATUS_FAIL);
	}
	case ZDO_MSG_CB_REGISTER:
		/* Every ZDO response is delivered (ZDO_DIRECT_CB), whatever is registered. */
		if (req->len != 2U) {
			return MT_ERR_LENGTH;
		}
		s->msg_cb = true;
		return status_rsp(rsp, STATUS_OK);
	case ZDO_STARTUP_FROM_APP:
		if (req->len != 2U) {
			return MT_ERR_LENGTH;
		}
		if (s->net.formed) {
			znp_send_areq_after(s->znp, MT_ZDO, ZDO_STATE_CHANGE_IND,
					    (uint8_t[]){ZB_DEV_COORD}, 1);
			return status_rsp(rsp, 0); /* restored network state */
		}
		/* No network: form one as BDB would; reported as new network state. */
		return status_rsp(rsp, zb_net_bdb_start(&s->net, ZB_BDB_FORMATION) == 0 ? 1 : 2);
	case ZDO_EXT_ROUTE_DISC:
		/* destination, options, radius: routes are found when the NWK layer exists */
		if (req->len != 4U) {
			return MT_ERR_LENGTH;
		}
		return status_rsp(rsp, STATUS_OK);
	case ZDO_EXT_ROUTE_CHK:
		/* destination, route status, options -> RoutingStatus */
		if (req->len != 4U) {
			return MT_ERR_LENGTH;
		}
		return status_rsp(rsp, ROUTING_NO_ENTRY);
	case ZDO_EXT_NWK_INFO:
		mt_put_u16(&rsp->data[0], s->net.formed ? NWK_COORDINATOR : 0xFFFE);
		rsp->data[2] = s->net.dev_state;
		mt_put_u16(&rsp->data[3], s->net.formed ? s->net.pan : 0xFFFF);
		mt_put_u16(&rsp->data[5], NWK_COORDINATOR); /* parent: itself */
		memcpy(&rsp->data[7], s->net.epid, 8);
		memcpy(&rsp->data[15], zb_net_eui(&s->net), 8);
		rsp->data[23] = s->net.formed ? s->net.channel : 0;
		rsp->len = 24;
		return 0;
	case ZDO_EXT_FIND_GROUP: {
		struct zb_group *g = NULL;

		if (req->len != 3U) {
			return MT_ERR_LENGTH;
		}
		for (int i = 0; i < ZB_GROUP_MAX; i++) {
			if (s->groups[i].used && s->groups[i].ep == d[0] &&
			    s->groups[i].id == mt_get_u16(&d[1])) {
				g = &s->groups[i];
			}
		}
		rsp->data[0] = g != NULL ? STATUS_OK : STATUS_FAIL;
		mt_put_u16(&rsp->data[1], mt_get_u16(&d[1]));
		rsp->data[3] = g != NULL ? g->namelen : 0;
		if (g != NULL) {
			memcpy(&rsp->data[4], g->name, g->namelen);
		}
		rsp->len = (uint8_t)(4U + rsp->data[3]);
		return 0;
	}
	case ZDO_EXT_ADD_GROUP: {
		if (req->len < 4U || d[3] > ZB_GROUP_NAME || req->len != 4U + d[3]) {
			return MT_ERR_LENGTH;
		}
		for (int i = 0; i < ZB_GROUP_MAX; i++) {
			if (!s->groups[i].used) {
				s->groups[i] = (struct zb_group){.used = true,
								 .ep = d[0],
								 .id = mt_get_u16(&d[1]),
								 .namelen = d[3]};
				memcpy(s->groups[i].name, &d[4], d[3]);
				return status_rsp(rsp, STATUS_OK);
			}
		}
		return status_rsp(rsp, STATUS_FAIL);
	}
	}
	return MT_ERR_COMMAND;
}

/* ------------------------------------------------------------------------------------------ */
/* AF */

static int af_register(struct zb_stack *s, const struct mt_frame *req, struct mt_frame *rsp)
{
	const uint8_t *d = req->data;
	struct zb_ep e = {.used = true};
	size_t o = 8;

	if (req->len < 9U) {
		return MT_ERR_LENGTH;
	}
	e.ep = d[0];
	e.profile = mt_get_u16(&d[1]);
	e.device = mt_get_u16(&d[3]);
	e.version = d[5];
	e.nin = d[7];
	if (e.nin > ZB_EP_CLUSTERS || req->len < o + 2U * e.nin + 1U) {
		return status_rsp(rsp, STATUS_INVALID_PARAM);
	}
	for (int i = 0; i < e.nin; i++, o += 2) {
		e.in[i] = mt_get_u16(&d[o]);
	}
	e.nout = d[o++];
	if (e.nout > ZB_EP_CLUSTERS || req->len < o + 2U * e.nout) {
		return status_rsp(rsp, STATUS_INVALID_PARAM);
	}
	for (int i = 0; i < e.nout; i++, o += 2) {
		e.out[i] = mt_get_u16(&d[o]);
	}
	/* Endpoint 0 is the ZDO and 0xFF the broadcast endpoint; 242 is Green Power. */
	if (e.ep == 0U || e.ep == 0xFFU) {
		return status_rsp(rsp, STATUS_INVALID_PARAM);
	}

	/* A second registration of an endpoint replaces the first. */
	for (int i = 0; i < ZB_EP_MAX; i++) {
		if (s->eps[i].used && s->eps[i].ep == e.ep) {
			s->eps[i] = e;
			return status_rsp(rsp, STATUS_OK);
		}
	}
	for (int i = 0; i < ZB_EP_MAX; i++) {
		if (!s->eps[i].used) {
			s->eps[i] = e;
			return status_rsp(rsp, STATUS_OK);
		}
	}
	return status_rsp(rsp, STATUS_FAIL);
}

/* Answer a ZDO request the host sent to this device, the way the stack's own ZDO would. */
static void local_zdo(struct zb_stack *s, uint16_t cluster, const uint8_t *data, size_t len,
		      bool unicast)
{
	uint8_t body[64];
	size_t n;
	uint8_t tsn;

	if (len < 1U) {
		return;
	}
	tsn = data[0];
	data++;
	len--;

	switch (cluster) {
	case ZCL_MGMT_PERMIT_JOIN_REQ:
		if (len >= 2U) {
			permit_from_zdo(s, data[0]);
			if (unicast) {
				body[0] = STATUS_OK;
				msg_cb_incoming(s, NWK_COORDINATOR, false, cluster | ZCL_RSP_BIT, tsn, body, 1, true);
			}
		}
		return;
	case ZCL_MGMT_NWK_UPDATE_REQ:
		/* channels, scan duration, scan count (+ more for 0xFE and 0xFF) */
		if (len < 6U) {
			return;
		}
		if (data[4] == NWK_UPDATE_CHANNEL_CHANGE) {
			uint32_t mask = mt_get_u32(data);

			if (__builtin_popcount(mask) == 1) {
				(void)zb_net_change_channel(&s->net, (uint8_t)__builtin_ctz(mask));
			}
		} else if (data[4] <= NWK_UPDATE_SCAN_MAX) {
			(void)zb_net_energy_scan(&s->net, mt_get_u32(data), data[4],
						 ESCAN_TAG_LOOPBACK | tsn);
		}
		return;
	default:
		n = zdo_body(s, cluster, data, len, unicast, false, body);
		if (n > 0U) {
			msg_cb_incoming(s, NWK_COORDINATOR, false, cluster | ZCL_RSP_BIT, tsn, body, n, true);
		}
		return;
	}
}

/*
 * Send what the host asked for: application data from a registered endpoint (its profile) or a
 * ZDO frame (endpoint 0). The outcome follows as AF_DATA_CONFIRM for application data.
 */
static int af_send(struct zb_stack *s, bool bcast, uint16_t dst, uint8_t dst_ep, uint8_t src_ep,
		   uint16_t cluster, uint8_t tsn, uint8_t options, const uint8_t *data, size_t len,
		   struct mt_frame *rsp)
{
	struct zb_aps_tx tx = {
		.dst = dst,
		.broadcast = bcast,
		.src_ep = src_ep,
		.dst_ep = dst_ep,
		.cluster = cluster,
		.ack_req = (options & AF_OPTION_ACK_REQUEST) != 0U,
		.confirm = dst_ep != 0U && !bcast,
		.tag = ((uint32_t)src_ep << 8) | tsn,
		.data = data,
		.len = len,
	};
	int r;

	if (src_ep != 0U) {
		bool found = false;

		for (int i = 0; i < ZB_EP_MAX && !found; i++) {
			if (s->eps[i].used && s->eps[i].ep == src_ep) {
				tx.profile = s->eps[i].profile;
				found = true;
			}
		}
		if (!found) {
			return status_rsp(rsp, STATUS_INVALID_PARAM);
		}
	}
	r = zb_net_send(&s->net, &tx);
	return status_rsp(rsp, r == 0 ? STATUS_OK
				       : r == -EMSGSIZE ? STATUS_INVALID_PARAM
							: r == -ENOMEM ? STATUS_BUFFER_FULL : STATUS_FAIL);
}

static int af_data_request_ext(struct zb_stack *s, const struct mt_frame *req, struct mt_frame *rsp)
{
	const uint8_t *d = req->data;
	uint16_t dst, cluster, dlen;
	uint8_t mode, dst_ep, src_ep, tsn;

	if (req->len < 20U) {
		return MT_ERR_LENGTH;
	}
	mode = d[0];
	dst = mt_get_u16(&d[1]);
	dst_ep = d[9];
	src_ep = d[12];
	cluster = mt_get_u16(&d[13]);
	tsn = d[15];
	dlen = mt_get_u16(&d[18]);
	if (req->len != 20U + dlen) {
		return MT_ERR_LENGTH;
	}
	if (mode != AF_ADDR_NWK && mode != AF_ADDR_BROADCAST) {
		return status_rsp(rsp, STATUS_FAIL); /* group and IEEE addressing: not supported */
	}

	if (dst_ep == 0U && for_me(dst)) {
		/* ZDO to this device (or a broadcast, which includes it). */
		local_zdo(s, cluster, &d[20], dlen, dst == NWK_COORDINATOR);
		if (dst == NWK_COORDINATOR) {
			return status_rsp(rsp, STATUS_OK);
		}
	} else if (dst == NWK_COORDINATOR) {
		return status_rsp(rsp, STATUS_FAIL); /* no application endpoints on this side */
	}
	return af_send(s, mode == AF_ADDR_BROADCAST, dst, dst_ep, src_ep, cluster, tsn, d[16], &d[20],
		       dlen, rsp);
}

int znp_af_handler(struct znp *z, const struct mt_frame *req, struct mt_frame *rsp)
{
	struct zb_stack *s = stk(z);

	switch (req->cmd) {
	case AF_REGISTER:
		return af_register(s, req, rsp);
	case AF_DATA_REQUEST_EXT:
		return af_data_request_ext(s, req, rsp);
	case AF_DATA_REQUEST: {
		/* dst, dst ep, src ep, cluster, transaction, options, radius, length, data */
		const uint8_t *d = req->data;

		if (req->len < 10U || req->len != 10U + d[9]) {
			return MT_ERR_LENGTH;
		}
		if (mt_get_u16(d) == NWK_COORDINATOR) {
			return status_rsp(rsp, STATUS_FAIL);
		}
		return af_send(s, false, mt_get_u16(d), d[2], d[3], mt_get_u16(&d[4]), d[6], d[7],
			       &d[10], d[9], rsp);
	}
	}
	return MT_ERR_COMMAND;
}
