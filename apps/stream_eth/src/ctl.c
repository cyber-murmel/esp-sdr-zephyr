/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Commands and the subscriber, see ctlproto.h for the subscription messages ("VRTS", "VRTC"
 * and cookie, "VRTK", "VRTB", "VRTT", "VRTX") and README.rst. Settings come as VITA 49.2 command
 * packets to the stream ID (frequency in Hz, gain stage 1 as the PHY's gain index or -1 for
 * automatic, sample rate, 8 bit payload format), acknowledged as in apps/stream, and only from the
 * live subscriber. Any other host may ask for the state, ten answers a second in all.
 *
 * NO AUTHENTICATION: the cookie shows that a host can receive at its address, nothing more. For a
 * LAN; do not expose the port. Datagrams are taken from unicast hosts on a connected prefix or
 * link-local, to a unicast address of the interface (CONFIG_APP_VRT_ALLOW_OFFLINK lifts the first).
 *
 * The datagrams come through net_context rather than sockets: the Ethernet header of each says
 * where the host's MAC is, and nothing then has to be resolved.
 */

#include <errno.h>
#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_context.h>
#include <zephyr/net/net_pkt.h>

#include <vrt/vrt_init.h>
#include <vrt/vrt_read.h>
#include <vrt/vrt_types.h>
#include <vrt/vrt_util.h>
#include <vrt/vrt_words.h>
#include <vrt/vrt_write.h>

#include <zephyr/random/random.h>

#include "ctlproto.h"
#include "stream_eth.h"

LOG_MODULE_DECLARE(stream_eth, LOG_LEVEL_INF);

#define MSG_MAX   256
#define ACK_WORDS 48

struct ctlmsg {
	struct peer p;
	uint16_t len;
	uint8_t data[MSG_MAX];
};
K_MSGQ_DEFINE(ctl_q, sizeof(struct ctlmsg), 8, 4);

static uint32_t rx_words[MSG_MAX / 4] __aligned(4);
static uint32_t ack_buf[ACK_WORDS];
static struct vrt_packet pkt, ack;
static struct vrt_command cmd, ack_cmd;
static struct {
	uint32_t commands, subscribes, bad, errors, dropped, rejected;
} stats;
static struct ctl_proto proto;
static bool ack_owed;
static struct peer ack_owed_peer;
static K_MUTEX_DEFINE(proto_lock);
static struct net_if *ctl_iface;

/* A datagram worth an answer: a unicast host on the link, to an address of ours. */
static bool addr_local_ok(const struct peer *p)
{
	struct net_if *i = NULL;

	if (p->v6) {
		return net_if_ipv6_addr_lookup((struct net_in6_addr *)p->local, &i) != NULL && i == ctl_iface;
	}
	return net_if_ipv4_addr_lookup((struct net_in_addr *)p->local, &i) != NULL && i == ctl_iface;
}

static bool peer_valid(const struct peer *p)
{
	static const uint8_t zero[16];
	bool all_zero_mac = true;

	for (int k = 0; k < 6; k++) {
		all_zero_mac &= p->mac[k] == 0U;
	}
	if ((p->mac[0] & 1U) != 0U || all_zero_mac || p->port == 0U) {
		return false;
	}
	if (p->v6) {
		struct net_in6_addr *a = (struct net_in6_addr *)p->addr;

		if (net_ipv6_is_addr_mcast(a) || net_ipv6_is_addr_unspecified(a)) {
			return false;
		}
		if (!IS_ENABLED(CONFIG_APP_VRT_ALLOW_OFFLINK) && !net_ipv6_is_ll_addr(a) &&
		    net_if_ipv6_prefix_lookup(ctl_iface, a, 64) == NULL) {
			return false;
		}
	} else {
		struct net_in_addr *a = (struct net_in_addr *)p->addr;

		if (net_ipv4_is_addr_mcast(a) || net_ipv4_is_addr_bcast(ctl_iface, a) ||
		    net_ipv4_is_addr_unspecified(a)) {
			return false;
		}
		if (!IS_ENABLED(CONFIG_APP_VRT_ALLOW_OFFLINK) && !net_ipv4_is_ll_addr(a) &&
		    !net_if_ipv4_addr_mask_cmp(ctl_iface, a)) {
			return false;
		}
	}
	(void)zero;
	return addr_local_ok(p);
}

/* Called by the RX thread: copy out what is needed, give the packet back. */
static void recv_cb(struct net_context *ctx, struct net_pkt *p, union net_ip_header *ip,
		    union net_proto_header *proto_hdr, int status, void *user)
{
	struct ctlmsg m;
	struct net_linkaddr *ll;
	size_t len;

	ARG_UNUSED(ctx);
	ARG_UNUSED(user);
	if (p == NULL) {
		return;
	}
	if (status != 0) {
		goto out;
	}
	memset(&m.p, 0, sizeof(m.p));
	m.p.v6 = net_pkt_family(p) == NET_AF_INET6;
	if (m.p.v6) {
		memcpy(m.p.addr, ip->ipv6->src, 16);
		memcpy(m.p.local, ip->ipv6->dst, 16);
	} else {
		memcpy(m.p.addr, ip->ipv4->src, 4);
		memcpy(m.p.local, ip->ipv4->dst, 4);
	}
	m.p.port = net_ntohs(proto_hdr->udp->src_port);
	ll = net_pkt_lladdr_src(p);
	if (ll->len != 6U) {
		goto out;
	}
	memcpy(m.p.mac, ll->addr, 6);
	if (!peer_valid(&m.p)) {
		stats.rejected++;
		goto out;
	}
	/* what is not worth queueing is dropped here: a source over its budget, a runt */
	{
		bool ok;

		k_mutex_lock(&proto_lock, K_FOREVER);
		ok = ctl_proto_admit(&proto, &m.p, k_uptime_get());
		k_mutex_unlock(&proto_lock);
		if (!ok || net_pkt_remaining_data(p) < 4U) {
			stats.dropped++;
			goto out;
		}
	}
	len = net_pkt_remaining_data(p);
	if (len == 0U || len > MSG_MAX || net_pkt_read(p, m.data, len) != 0) {
		stats.dropped++;
		goto out;
	}
	m.len = (uint16_t)len;
	/* a multicast or broadcast destination is nobody's address to answer from */
	if (k_msgq_put(&ctl_q, &m, K_NO_WAIT) != 0) {
		stats.dropped++;
	}
out:
	net_pkt_unref(p);
}

static int open_ctx(net_sa_family_t family)
{
	struct net_context *ctx;
	int ret = net_context_get(family, NET_SOCK_DGRAM, NET_IPPROTO_UDP, &ctx);

	if (ret != 0) {
		return ret;
	}
	if (family == NET_AF_INET6) {
		struct net_sockaddr_in6 a = {.sin6_family = NET_AF_INET6,
					     .sin6_port = net_htons(CONFIG_APP_VRT_PORT)};

		ret = net_context_bind(ctx, (struct net_sockaddr *)&a, sizeof(a));
	} else {
		struct net_sockaddr_in a = {.sin_family = NET_AF_INET,
					    .sin_port = net_htons(CONFIG_APP_VRT_PORT)};

		ret = net_context_bind(ctx, (struct net_sockaddr *)&a, sizeof(a));
	}
	if (ret == 0) {
		ret = net_context_recv(ctx, recv_cb, K_NO_WAIT, NULL);
	}
	return ret;
}

int ctl_init(struct net_if *iface)
{
	uint8_t key[16];
	int ret;

	ctl_iface = iface;
	sys_rand_get(key, sizeof(key));
	ctl_proto_init(&proto, key, (uint32_t)net_if_get_by_iface(iface),
		       (int64_t)CONFIG_APP_VRT_SILENT_S * 1000);
	ret = open_ctx(NET_AF_INET);
	if (ret != 0) {
		LOG_ERR("IPv4 control port: %d", ret);
	}
	ret = open_ctx(NET_AF_INET6);
	if (ret != 0) {
		LOG_ERR("IPv6 control port: %d", ret);
	}
	LOG_INF("VITA 49.2 commands on UDP port %d, stream ID 0x%08x", CONFIG_APP_VRT_PORT,
		CONFIG_APP_VRT_STREAM_ID);
	return 0;
}

static void log_peer(const char *what, const struct peer *p)
{
	char s[NET_INET6_ADDRSTRLEN];

	(void)net_addr_ntop(p->v6 ? NET_AF_INET6 : NET_AF_INET, p->addr, s, sizeof(s));
	LOG_INF("%s %s port %u", what, s, p->port);
}

/* The protocol decided: apply it to the stream. */
static void apply_event(const struct ctl_out *o, const struct peer *p)
{
	struct peer sp;

	switch (o->evt) {
	case CTL_EVT_SUBSCRIBED:
	case CTL_EVT_TAKEOVER:
		stats.subscribes++;
		sp = *p;
		sp.nocsum = IS_ENABLED(CONFIG_APP_VRT_V6_ZERO_CSUM) && p->v6 && o->nocsum;
		if (txpath_subscribe(&sp)) {
			log_peer(o->evt == CTL_EVT_TAKEOVER ? "subscriber replaced by" : "subscriber", p);
		}
		radio_enable(true);
		/* the packets fill the MTU of this kind of address: a different size restarts the ring */
		if (cells.pl != 0U && (txpath_payload_bytes() != cells.pl ||
				       cells.sum_on != (txpath_v6() && !txpath_zero_csum()))) {
			radio_replan();
		}
		break;
	case CTL_EVT_RENEWED:
		/* the cookie covers the MAC and the addresses, so only the flag can differ */
		sp = *p;
		sp.nocsum = IS_ENABLED(CONFIG_APP_VRT_V6_ZERO_CSUM) && p->v6 && o->nocsum;
		(void)txpath_subscribe(&sp);
		break;
	case CTL_EVT_STOPPED:
		log_peer("stop from", p);
		txpath_unsubscribe();
		radio_enable(false);
		break;
	default:
		break;
	}
}

/* The shell's stop: the stream ends and nobody may subscribe until ctl_unblock(). */
void ctl_block(void)
{
	k_mutex_lock(&proto_lock, K_FOREVER);
	proto.blocked = true;
	proto.live = false;
	txpath_unsubscribe();
	radio_enable(false);
	k_mutex_unlock(&proto_lock);
}

void ctl_unblock(void)
{
	k_mutex_lock(&proto_lock, K_FOREVER);
	proto.blocked = false;
	k_mutex_unlock(&proto_lock);
}

void ctl_resume(uint32_t gen)
{
	k_mutex_lock(&proto_lock, K_FOREVER);
	if (proto.live && !proto.blocked && txpath_sub_gen() == gen) {
		radio_enable(true);
	}
	k_mutex_unlock(&proto_lock);
}

bool ctl_blocked(void)
{
	return proto.blocked;
}

int ctl_build_context(uint32_t *buf, size_t words, uint32_t rate, uint64_t freq_hz, int gain,
		      uint8_t epoch, uint32_t sec, uint64_t ps, bool changed, uint8_t count)
{
	struct vrt_packet p;
	struct vrt_data_packet_payload_format *fmt = &p.if_context.data_packet_payload_format;
	int32_t w;

	vrt_init_packet(&p);
	p.header.packet_type = VRT_PT_IF_CONTEXT;
	p.header.packet_count = count & 0xfU;
	p.fields.stream_id = CONFIG_APP_VRT_STREAM_ID;
	p.header.tsi = VRT_TSI_OTHER;
	p.header.tsf = VRT_TSF_REAL_TIME;
	p.fields.integer_seconds_timestamp = sec;
	p.fields.fractional_seconds_timestamp = ps;
	/* CIF1 is a 49.2 feature: declare compliance and mark the packet not 49.0. */
	p.header.nd0 = true;
	p.if_context.has.v49_spec_compliance = true;
	p.if_context.v49_spec_compliance = VRT_V49_2;
	p.if_context.context_field_change_indicator = changed;
	p.if_context.has.rf_reference_frequency = true;
	p.if_context.rf_reference_frequency = (double)freq_hz;
	p.if_context.has.sample_rate = true;
	p.if_context.sample_rate = (double)rate;
	/* complex sampling: the band is one sample rate wide */
	p.if_context.has.bandwidth = true;
	p.if_context.bandwidth = (double)rate;
	p.if_context.has.gain = true;
	p.if_context.gain.stage1 = (float)gain;
	/* CIF0 bit 16: the settings epoch of the run whose data follows, in user bits 7..0 */
	p.if_context.has.state_and_event_indicators = true;
	p.if_context.state_and_event_indicators.user_defined = epoch;
	p.if_context.has.data_packet_payload_format = true;
	fmt->packing_method = VRT_PM_LINK_EFFICIENT;
	fmt->real_or_complex = VRT_ROC_COMPLEX_CARTESIAN;
	fmt->data_item_format = VRT_DIF_SIGNED_FIXED_POINT;
	fmt->item_packing_field_size = 7;
	fmt->data_item_size = 7;
	p.header.packet_size = (uint16_t)vrt_words_packet(&p);
	w = vrt_write_packet(&p, buf, words, true);
	if (w < 0) {
		return w;
	}
	vrt_to_big_endian(buf, w);
	return w;
}

static void send_ack(const struct peer *to, bool query, const struct vrt_ack_responses *errors,
		     bool executed)
{
	int32_t words;

	vrt_init_packet(&ack);
	vrt_init_command(&ack_cmd);
	ack.command = &ack_cmd;
	ack.header.packet_type = VRT_PT_COMMAND;
	ack.header.acknowledge = true;
	ack.fields.stream_id = pkt.fields.stream_id;
	ack_cmd.message_id = cmd.message_id;
	ack_cmd.cam.controllee_enable = cmd.cam.controllee_enable;
	ack_cmd.cam.controllee_format = cmd.cam.controllee_format;
	ack_cmd.controllee_id = cmd.controllee_id;
	memcpy(ack_cmd.controllee_uuid, cmd.controllee_uuid, sizeof(cmd.controllee_uuid));
	ack_cmd.cam.controller_enable = cmd.cam.controller_enable;
	ack_cmd.cam.controller_format = cmd.cam.controller_format;
	ack_cmd.controller_id = cmd.controller_id;
	memcpy(ack_cmd.controller_uuid, cmd.controller_uuid, sizeof(cmd.controller_uuid));

	if (query) {
		/* Query-state acknowledge: the settings as context fields. */
		struct radio_state rs;
		uint64_t center;
		struct vrt_data_packet_payload_format *f = &ack.if_context.data_packet_payload_format;

		radio_get(&rs);
		center = radio_center_hz(rs.freq_hz);
		ack_cmd.cam.query_state = true;
		ack.if_context.has.rf_reference_frequency = true;
		ack.if_context.rf_reference_frequency = (double)(center != 0U ? center : rs.freq_hz);
		ack.if_context.has.sample_rate = true;
		ack.if_context.sample_rate = (double)rs.rate_hz;
		ack.if_context.has.gain = true;
		ack.if_context.gain.stage1 = (float)rs.gain;
		/* the epoch of the settings as they are after this command */
		ack.if_context.has.state_and_event_indicators = true;
		ack.if_context.state_and_event_indicators.user_defined = rs.epoch;
		ack.if_context.has.data_packet_payload_format = true;
		f->packing_method = VRT_PM_LINK_EFFICIENT;
		f->real_or_complex = VRT_ROC_COMPLEX_CARTESIAN;
		f->data_item_format = VRT_DIF_SIGNED_FIXED_POINT;
		f->item_packing_field_size = 7;
		f->data_item_size = 7;
	} else {
		ack_cmd.cam.execution = true;
		ack_cmd.cam.scheduled_or_executed = executed;
		ack_cmd.cam.errors = cmd.cam.errors && errors != NULL;
		if (ack_cmd.cam.errors) {
			ack_cmd.errors = *errors;
		}
	}
	ack.header.packet_size = (uint16_t)vrt_words_packet(&ack);
	words = vrt_write_packet(&ack, ack_buf, ARRAY_SIZE(ack_buf), true);
	if (words < 0) {
		stats.errors++;
		return;
	}
	vrt_to_big_endian(ack_buf, words);
	if (txpath_send_ctl(to, ack_buf, (size_t)words * 4U) != 0) {
		stats.errors++;
	}
}

static void handle_command(const struct peer *from, bool is_sub)
{
	struct vrt_ack_responses errors = {0};
	const struct vrt_if_context *c = &pkt.if_context;
	struct radio_state rs;
	uint64_t hz;
	int gain;
	uint32_t rate;
	bool any_error = false, executed = false;
	bool settings = c->has.rf_reference_frequency || c->has.gain || c->has.sample_rate ||
			c->has.data_packet_payload_format;

	radio_get(&rs);
	hz = rs.freq_hz;
	gain = rs.gain;
	rate = rs.rate_hz;
	stats.commands++;
	if (pkt.header.acknowledge) {
		return;
	}
	if (pkt.header.cancellation) {
		/* a cancellation does not stop the stream: VRTX with a cookie does */
		return;
	}
	if (pkt.fields.stream_id != CONFIG_APP_VRT_STREAM_ID) {
		stats.bad++;
		return;
	}
	if (!is_sub) {
		/* a host that is not the subscriber may only ask for the state */
		if (cmd.cam.query_state && !settings && !cmd.cam.execution &&
		    ctl_proto_query_ok(&proto, from, k_uptime_get())) {
			send_ack(from, true, NULL, false);
		} else {
			stats.rejected++;
		}
		return;
	}
	if (c->has.rf_reference_frequency) {
		double f = c->rf_reference_frequency;

		if (f < 0.0 || f != floor(f) || f > 6e9 || !radio_freq_ok((uint64_t)f)) {
			errors.has.rf_reference_frequency = true;
			errors.rf_reference_frequency = VRT_WEF_PARAMETER_OUT_OF_RANGE;
			any_error = true;
		} else {
			hz = (uint64_t)f;
		}
	}
	if (c->has.gain) {
		float g = c->gain.stage1;

		if (c->gain.stage2 != 0.0f || g != floorf(g) ||
		    (g != -1.0f && (g < 0.0f || g > (float)radio_gain_max()))) {
			errors.has.gain = true;
			errors.gain = VRT_WEF_PARAMETER_OUT_OF_RANGE;
			any_error = true;
		} else {
			gain = (int)g;
		}
	}
	if (c->has.data_packet_payload_format) {
		const struct vrt_data_packet_payload_format *f = &c->data_packet_payload_format;

		if (f->packing_method != VRT_PM_LINK_EFFICIENT ||
		    f->real_or_complex != VRT_ROC_COMPLEX_CARTESIAN ||
		    f->data_item_format != VRT_DIF_SIGNED_FIXED_POINT || f->data_item_size != 7 ||
		    f->item_packing_field_size != 7) {
			errors.has.data_packet_payload_format = true;
			errors.data_packet_payload_format = VRT_WEF_PARAMETER_OUT_OF_RANGE;
			any_error = true;
		}
	}
	if (c->has.sample_rate) {
		double r = c->sample_rate;

		if (r != floor(r) || r < 1.0 || r > 1e9 || !radio_rate_ok((uint32_t)r)) {
			errors.has.sample_rate = true;
			errors.sample_rate = VRT_WEF_PARAMETER_OUT_OF_RANGE;
			any_error = true;
		} else {
			rate = (uint32_t)r;
		}
	}
	if (cmd.cam.action_mode == VRT_AM_EXECUTE && (!any_error || cmd.cam.permit_errors)) {
		if (c->has.rf_reference_frequency && !errors.has.rf_reference_frequency &&
		    radio_set_freq(hz) != 0) {
			errors.has.rf_reference_frequency = true;
			errors.rf_reference_frequency = VRT_WEF_PARAMETER_OUT_OF_RANGE;
			any_error = true;
		}
		if (c->has.gain && !errors.has.gain && radio_set_gain(gain) != 0) {
			stats.errors++;
		}
		if (c->has.sample_rate && !errors.has.sample_rate && radio_set_rate(rate) != 0) {
			stats.errors++;
		}
		executed = true;
	}
	if (cmd.cam.execution && (!cmd.cam.nack_only || any_error)) {
		send_ack(from, false, any_error ? &errors : NULL, executed);
	}
	if (cmd.cam.query_state) {
		send_ack(from, true, NULL, false);
	}
}

static void handle(const struct ctlmsg *m)
{
	struct ctl_out o;
	int32_t words, rv;
	bool sub;

	/* the decision and its effect under one lock: a stop on the shell cannot fall between them */
	k_mutex_lock(&proto_lock, K_FOREVER);
	ctl_proto_dgram(&proto, &m->p, m->data, m->len, k_uptime_get(), &o);
	apply_event(&o, &m->p);
	k_mutex_unlock(&proto_lock);
	if (o.act == CTL_REPLY) {
		/* a VRTK that could not go (no descriptor, no slot) is owed: the next renewal brings it */
		if (txpath_send_ctl(&m->p, o.reply, o.reply_len) != 0 && o.reply_len == 4U &&
		    o.reply[3] == 'K') {
			ack_owed = true;
			ack_owed_peer = m->p;
		}
		return;
	}
	if (o.evt == CTL_EVT_RENEWED && ack_owed && ctl_peer_same(&ack_owed_peer, &m->p) &&
	    txpath_send_ctl(&m->p, "VRTK", 4) == 0) {
		ack_owed = false;
	}
	if (o.act != CTL_CMD) {
		stats.bad += o.evt == CTL_EVT_NONE;
		return;
	}
	sub = o.is_sub;
	memcpy(rx_words, m->data, m->len);
	words = (int32_t)(m->len / 4U);
	vrt_from_big_endian(rx_words, words);
	vrt_init_packet(&pkt);
	vrt_init_command(&cmd);
	pkt.command = &cmd;
	rv = vrt_read_packet(rx_words, words, &pkt, true);
	if (rv < 0 || !vrt_is_command(&pkt.header)) {
		stats.bad++;
		return;
	}
	handle_command(&m->p, sub);
}

void ctl_run(void)
{
	struct ctlmsg m;

	for (;;) {
		bool gone;
		struct peer cur;

		if (k_msgq_get(&ctl_q, &m, K_MSEC(500)) == 0) {
			handle(&m);
		}
		k_mutex_lock(&proto_lock, K_FOREVER);
		gone = ctl_proto_expire(&proto, k_uptime_get());
		if (proto.live && !addr_local_ok(&proto.sub)) {
			/* the address it was sent to is gone (a new DHCP lease): the stream cannot go on */
			LOG_WRN("the address of the stream is gone: stopped");
			proto.live = false;
			gone = true;
		}
		if (gone && txpath_peer(&cur)) {
			LOG_INF("no renewal for %d s (or its address went): stream stopped",
				CONFIG_APP_VRT_SILENT_S);
			txpath_unsubscribe();
			radio_enable(false);
		}
		k_mutex_unlock(&proto_lock);
	}
}

void ctl_get_stats(struct ctl_stats *o)
{
	o->commands = stats.commands;
	o->subscribes = stats.subscribes;
	o->bad = stats.bad;
	o->errors = stats.errors;
	o->dropped = stats.dropped;
	o->rejected = stats.rejected;
}
