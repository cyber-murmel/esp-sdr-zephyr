/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * apps/stream_eth: the data packet against libvrt's encoding of the same packet, the time of a
 * packet from the sample index (past 2^32 samples), packet count and loss flags through the
 * staging cells, the MTU split, IPv4 and IPv6 headers and checksums.
 */
#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <vrt/vrt_init.h>
#include <vrt/vrt_read.h>
#include <vrt/vrt_types.h>
#include <vrt/vrt_util.h>
#include <vrt/vrt_words.h>
#include <vrt/vrt_write.h>

#include "ctlproto.h"
#include "netframe.h"
#include "vrt_cells.h"
#include "vrt_pkt.h"

ZTEST_SUITE(vrt_eth, NULL, NULL, NULL, NULL, NULL);

#define STREAM_ID 0x5d5d0001U

/* libvrt's packet for the same samples, as apps/stream makes its data packets. */
static int32_t libvrt_packet(uint8_t *out, size_t out_max, const int8_t *iq, size_t pairs,
			     uint8_t count, uint32_t sec, uint64_t ps, bool agc, bool over, bool loss)
{
	static uint32_t body[512], buf[512];
	struct vrt_packet p;
	int32_t w;

	zassert_true(pairs % 2U == 0U);
	for (size_t k = 0; k < pairs / 2U; k++) {
		body[k] = ((uint32_t)(uint8_t)iq[4 * k] << 24) | ((uint32_t)(uint8_t)iq[4 * k + 1] << 16) |
			  ((uint32_t)(uint8_t)iq[4 * k + 2] << 8) | (uint8_t)iq[4 * k + 3];
	}
	vrt_init_packet(&p);
	p.header.packet_type = VRT_PT_IF_DATA_WITH_STREAM_ID;
	p.header.packet_count = count & 0xfU;
	p.header.tsi = VRT_TSI_OTHER;
	p.header.tsf = VRT_TSF_REAL_TIME;
	p.fields.stream_id = STREAM_ID;
	p.fields.integer_seconds_timestamp = sec;
	p.fields.fractional_seconds_timestamp = ps;
	p.header.has.trailer = true;
	p.body = body;
	p.words_body = (int32_t)(pairs / 2U);
	p.trailer.has.valid_data = true;
	p.trailer.valid_data = true;
	p.trailer.has.agc_or_mgc = true;
	p.trailer.agc_or_mgc = agc;
	p.trailer.has.over_range = true;
	p.trailer.over_range = over;
	p.trailer.has.sample_loss = true;
	p.trailer.sample_loss = loss;
	p.header.packet_size = (uint16_t)vrt_words_packet(&p);
	w = vrt_write_packet(&p, buf, ARRAY_SIZE(buf), true);
	zassert_true(w > 0, "libvrt: %d", w);
	vrt_to_big_endian(buf, w);
	zassert_true((size_t)w * 4U <= out_max);
	memcpy(out, buf, (size_t)w * 4U);
	return w * 4;
}

static void ours(uint8_t *out, const int8_t *iq, size_t pairs, uint8_t count, uint32_t sec,
		 uint64_t ps, bool agc, bool over, bool loss)
{
	size_t bytes = pairs * 2U;

	vrt_data_header(out, STREAM_ID, count, vrt_packet_words(bytes), sec, ps);
	memcpy(out + VRT_DATA_HDR_BYTES, iq, bytes);
	vrt_put_trailer(out + VRT_DATA_HDR_BYTES + bytes, vrt_trailer_word(agc, over, loss));
}

ZTEST(vrt_eth, test_bytes_equal_libvrt)
{
	static int8_t iq[1440];
	uint8_t a[1500], b[1500];
	uint32_t seed = 12345;

	for (size_t k = 0; k < sizeof(iq); k++) {
		seed = seed * 1103515245U + 12345U;
		iq[k] = (int8_t)(seed >> 16);
	}
	iq[0] = -128;
	iq[1] = 127;
	for (unsigned int flags = 0; flags < 8U; flags++) {
		for (uint8_t count = 0; count < 20U; count += 3U) {
			static const size_t sizes[] = {2, 4, 680, 720, 710};

			for (size_t s = 0; s < ARRAY_SIZE(sizes); s++) {
				uint32_t sec = 1000U + count * 77U;
				uint64_t ps = 999999999999ULL - count * 12345ULL;
				int32_t n = libvrt_packet(a, sizeof(a), iq, sizes[s], count, sec, ps,
							  flags & 1U, flags & 2U, flags & 4U);

				ours(b, iq, sizes[s], count, sec, ps, flags & 1U, flags & 2U,
				     flags & 4U);
				zassert_equal(n, VRT_DATA_HDR_BYTES + sizes[s] * 2U + 4U);
				zassert_mem_equal(a, b, (size_t)n, "flags %u count %u pairs %zu",
						  flags, count, sizes[s]);
			}
		}
	}
}

ZTEST(vrt_eth, test_clock_past_2_32_samples)
{
	static const uint32_t rates[] = {40000000U, 20000000U, 16000000U, 3200000U, 640000U,
					 30000000U, 12345678U};
	struct vrt_clock c;

	for (size_t r = 0; r < ARRAY_SIZE(rates); r++) {
		uint64_t start = (1ULL << 32) - 1000U + 17U * r;

		vrt_clock_seek(&c, rates[r], start);
		for (int k = 0; k < 20000; k++) {
			unsigned __int128 num = (unsigned __int128)(c.index % rates[r]) *
						1000000000000ULL;

			zassert_equal(c.sec, (uint32_t)(c.index / rates[r]));
			zassert_equal(vrt_clock_ps(&c), (uint64_t)(num / rates[r]),
				      "rate %u index %llu", rates[r], (unsigned long long)c.index);
			zassert_true(vrt_clock_ps(&c) < 1000000000000ULL);
			vrt_clock_advance(&c, 680U);
		}
		/* the whole run kept the exact index */
		zassert_equal(c.index, start + 20000ULL * 680U);
		zassert_equal(c.sec, (uint32_t)(c.index / rates[r]));
	}
	/* seconds count on past 2^32 samples: 2^32 + 5 at 40 MS/s is 107 s and a bit */
	vrt_clock_seek(&c, 40000000U, (1ULL << 32) + 5U);
	zassert_equal(c.sec, 107U);
	zassert_equal(vrt_clock_ps(&c), (((1ULL << 32) + 5U) % 40000000ULL) * 25000ULL);
}

ZTEST(vrt_eth, test_mtu_split)
{
	static const unsigned int mtus[] = {1492, 1500, 1280, 576, 9000};

	zassert_equal(vrt_payload_bytes(1492, false), 1440U);
	zassert_equal(vrt_payload_bytes(1492, true), 1420U);
	zassert_equal(vrt_payload_bytes(1500, false), 1448U);
	zassert_equal(vrt_payload_bytes(1500, true), 1428U);
	zassert_equal(vrt_payload_bytes(20, false), 0U);
	for (size_t k = 0; k < ARRAY_SIZE(mtus); k++) {
		for (int v6 = 0; v6 < 2; v6++) {
			size_t pl = vrt_payload_bytes(mtus[k], v6);
			size_t ip = (v6 ? 40U : 20U) + 8U + VRT_DATA_HDR_BYTES + pl + VRT_TRAILER_BYTES;

			zassert_true(ip <= mtus[k], "mtu %u v6 %d: %zu", mtus[k], v6, ip);
			zassert_true(ip + 4U > mtus[k], "mtu %u v6 %d not full: %zu", mtus[k], v6, ip);
			zassert_equal(pl % 4U, 0U);
			zassert_equal(vrt_packet_words(pl) * 4U, VRT_DATA_HDR_BYTES + pl + 4U);
		}
	}
}

/* ---- cells: packets that span nodes, packet count, loss ---- */

#define NCELLS 16
#define STRIDE 1472
static uint8_t cell_mem[NCELLS][STRIDE];
static struct vrt_cell_meta cell_meta[NCELLS];
static struct vrt_cells cells;
static uint8_t node[4080];

static void fill_node(uint64_t first_sample)
{
	/* the value of byte k is the low byte of its position in the stream */
	for (size_t k = 0; k < sizeof(node); k++) {
		node[k] = (uint8_t)(first_sample * 2U + k);
	}
}

ZTEST(vrt_eth, test_cells_span_nodes_and_flag_loss)
{
	uint32_t rd = 0;
	uint64_t index = 1000;
	const uint32_t pl = 1440;

	vrt_cells_init(&cells, &cell_mem[0][0], cell_meta, NCELLS, STRIDE);
	vrt_cells_start(&cells, pl, 40000000U, 2412000000ULL, -1, 0, false, false);
	/* 4080 B nodes, 1440 B packets: 3 nodes make 8.5 packets */
	for (int n = 0; n < 3; n++) {
		fill_node(index);
		zassert_equal(vrt_cells_push(&cells, node, sizeof(node), index), 0);
		vrt_cells_commit(&cells);
		index += 2040U;
	}
	zassert_equal(vrt_cells_avail(&cells, rd), 8U);
	for (uint32_t k = 0; k < 8U; k++, rd++) {
		const struct vrt_cell_meta *m = vrt_cell_meta(&cells, rd);
		const uint8_t *p = vrt_cell_ptr(&cells, rd);

		zassert_equal(m->index, 1000U + k * (pl / 2U), "cell %u index", k);
		zassert_false(m->loss);
		zassert_equal(m->bytes, pl);
		zassert_equal(m->rate, 40000000U);
		/* a packet spanning two nodes holds the bytes in order, none of the 16 B holes */
		for (uint32_t j = 0; j < pl; j += 97U) {
			zassert_equal(p[j], (uint8_t)(1000U * 2U + k * pl + j), "cell %u byte %u", k, j);
		}
		vrt_cells_done(&cells, rd);
	}
	/* 8 whole cells and half a cell of the 9th are in; a node the ring skipped: the half
	 * cell goes with it and the next packet carries the loss
	 */
	index += 2040U * 5U;
	fill_node(index);
	zassert_equal(vrt_cells_push(&cells, node, sizeof(node), index), 0);
	vrt_cells_commit(&cells);
	zassert_equal(vrt_cells_avail(&cells, rd), 2U);
	zassert_true(vrt_cell_meta(&cells, rd)->loss);
	zassert_equal(vrt_cell_meta(&cells, rd)->index, index);
	zassert_false(vrt_cell_meta(&cells, rd + 1U)->loss);
	zassert_equal(vrt_cell_meta(&cells, rd + 1U)->index, index + pl / 2U);
}

ZTEST(vrt_eth, test_cells_full_drops_whole_nodes_and_aborts)
{
	uint64_t index = 0;
	uint32_t rd = 0, dropped = 0;

	vrt_cells_init(&cells, &cell_mem[0][0], cell_meta, NCELLS, STRIDE);
	vrt_cells_start(&cells, 1440, 20000000U, 2412000000ULL, -1, 0, false, false);
	/* nothing is sent: the cells fill after 8 * 1440 B, then whole nodes drop */
	for (int n = 0; n < 10; n++) {
		fill_node(index);
		if (vrt_cells_push(&cells, node, sizeof(node), index) == 0) {
			vrt_cells_commit(&cells);
		} else {
			dropped++;
		}
		index += 2040U;
	}
	zassert_true(dropped > 0U);
	zassert_equal(cells.dropped_nodes, dropped);
	zassert_true(vrt_cells_avail(&cells, rd) <= NCELLS);
	/* the sender frees all; the next node starts a packet that says samples were lost */
	for (; rd < cells.wr; rd++) {
		vrt_cells_done(&cells, rd);
	}
	fill_node(index);
	zassert_equal(vrt_cells_push(&cells, node, sizeof(node), index), 0);
	vrt_cells_commit(&cells);
	zassert_true(vrt_cell_meta(&cells, rd)->loss);
	zassert_equal(vrt_cell_meta(&cells, rd)->index, index);
	rd = cells.wr;
	for (uint32_t k = 0; k < rd; k++) {
		vrt_cells_done(&cells, k);
	}
	/* a node the ring wrote over while it was copied: abort, and the gap shows on the next */
	index += 2040U;
	fill_node(index);
	zassert_equal(vrt_cells_push(&cells, node, sizeof(node), index), 0);
	vrt_cells_abort(&cells);
	zassert_equal(vrt_cells_avail(&cells, rd), 0U);
	index += 2040U;
	fill_node(index);
	zassert_equal(vrt_cells_push(&cells, node, sizeof(node), index), 0);
	vrt_cells_commit(&cells);
	zassert_equal(vrt_cells_avail(&cells, rd), 2U);
	zassert_true(vrt_cell_meta(&cells, rd)->loss);
	zassert_equal(vrt_cell_meta(&cells, rd)->index, index);
}

/* ---- the packet count and loss flags through the whole chain ---- */

ZTEST(vrt_eth, test_count_wraps_and_time_follows_index)
{
	struct vrt_clock c;
	uint8_t buf[1500];
	uint8_t count = 0;
	uint32_t rate = 40000000U;
	uint64_t index = (1ULL << 32) - 2U * 720U;

	vrt_clock_seek(&c, rate, index);
	for (int k = 0; k < 40; k++) {
		uint32_t w;
		uint64_t want_ps = (index % rate) * 25000ULL;

		vrt_data_header(buf, STREAM_ID, count++, 365, c.sec, vrt_clock_ps(&c));
		zassert_equal(((uint32_t)buf[0] << 24 | buf[1] << 16 | buf[2] << 8 | buf[3]) >> 16 & 0xfU,
			      (uint32_t)k & 0xfU);
		w = (uint32_t)buf[8] << 24 | buf[9] << 16 | buf[10] << 8 | buf[11];
		zassert_equal(w, (uint32_t)(index / rate));
		zassert_equal(vrt_clock_ps(&c), want_ps);
		vrt_clock_advance(&c, 720U);
		index += 720U;
	}
	zassert_equal(count, 40U);
}

ZTEST(vrt_eth, test_clipped)
{
	uint32_t w[16] = {0};
	uint8_t *b = (uint8_t *)w;

	zassert_false(vrt_clipped(b, sizeof(w), 1));
	memset(b, 0x55, sizeof(w));
	zassert_false(vrt_clipped(b, sizeof(w), 1));
	b[37] = 0x7f;
	zassert_true(vrt_clipped(b, sizeof(w), 1));
	b[37] = 0x80;
	zassert_true(vrt_clipped(b, sizeof(w), 1));
	b[37] = 0x81;
	zassert_false(vrt_clipped(b, sizeof(w), 1));
	b[36] = 0x7e;
	b[39] = 0xff;
	zassert_false(vrt_clipped(b, sizeof(w), 1));
	b[60] = 0x80;
	zassert_true(vrt_clipped(b, sizeof(w), 1));
	/* every fourth word: word 15 is not looked at, word 0 is */
	zassert_false(vrt_clipped(b, sizeof(w), 4));
	b[0] = 0x7f;
	zassert_true(vrt_clipped(b, sizeof(w), 4));
}

/* ---- frames ---- */

/* The textbook checksum of the UDP datagram with its pseudo header, byte by byte. */
static uint16_t ref_udp_csum(const uint8_t *src, const uint8_t *dst, const uint8_t *udp, size_t len)
{
	uint32_t s = 0;

	for (int k = 0; k < 16; k += 2) {
		s += (uint32_t)(src[k] << 8 | src[k + 1]) + (uint32_t)(dst[k] << 8 | dst[k + 1]);
	}
	s += (uint32_t)len + 17U;
	for (size_t k = 0; k + 1U < len; k += 2U) {
		s += (uint32_t)(udp[k] << 8 | udp[k + 1U]);
	}
	if (len % 2U != 0U) {
		s += (uint32_t)udp[len - 1U] << 8;
	}
	while (s >> 16) {
		s = (s & 0xffffU) + (s >> 16);
	}
	return (uint16_t)s;
}

static uint16_t sum_ok(const uint8_t *p, size_t len)
{
	uint32_t s = 0;

	for (size_t k = 0; k < len; k += 2U) {
		s += (uint32_t)(p[k] << 8 | p[k + 1U]);
	}
	while (s >> 16) {
		s = (s & 0xffffU) + (s >> 16);
	}
	return (uint16_t)s;
}

ZTEST(vrt_eth, test_ipv4_frame)
{
	static const uint8_t dmac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
	static const uint8_t smac[6] = {0x1c, 0x29, 0x04, 0xd1, 0x9c, 0xea};
	static const uint8_t sa[4] = {192, 168, 1, 50}, da[4] = {192, 168, 1, 7};
	static uint8_t body[1444] __aligned(4);
	uint8_t hdr[96] __aligned(4);
	struct nf_tmpl t;
	size_t pl = vrt_payload_bytes(1492, false);
	uint16_t id0;

	for (size_t k = 0; k < sizeof(body); k++) {
		body[k] = (uint8_t)(k * 7U);
	}
	nf_tmpl_init(&t, false, dmac, smac, sa, da, 4992, 51234);
	zassert_equal(t.len, 42U);
	for (int frame = 0; frame < 3; frame++) {
		memcpy(hdr, t.b, t.len);
		vrt_data_header(hdr + t.len, STREAM_ID, (uint8_t)frame, vrt_packet_words(pl), 5, 7);
		nf_seal(&t, hdr, VRT_DATA_HDR_BYTES, body, pl + VRT_TRAILER_BYTES);
		zassert_mem_equal(hdr, dmac, 6);
		zassert_mem_equal(hdr + 6, smac, 6);
		zassert_equal(hdr[12], 0x08);
		zassert_equal(hdr[13], 0x00);
		zassert_equal(hdr[14], 0x45);
		/* the whole IP packet is the MTU, and a header sums to 0xffff */
		zassert_equal(hdr[16] << 8 | hdr[17], 1492);
		zassert_equal(sum_ok(hdr + 14, 20), 0xffff, "IPv4 header checksum");
		zassert_equal(hdr[20] & 0x40, 0x40, "don't fragment");
		zassert_equal(hdr[23], 17);
		zassert_mem_equal(hdr + 26, sa, 4);
		zassert_mem_equal(hdr + 30, da, 4);
		zassert_equal(hdr[34] << 8 | hdr[35], 4992);
		zassert_equal(hdr[36] << 8 | hdr[37], 51234);
		zassert_equal(hdr[38] << 8 | hdr[39], 8 + 20 + pl + 4);
		zassert_equal(hdr[40] | hdr[41], 0, "UDP checksum 0 over IPv4");
		if (frame == 0) {
			id0 = (uint16_t)(hdr[18] << 8 | hdr[19]);
		} else {
			zassert_equal((uint16_t)(hdr[18] << 8 | hdr[19]), (uint16_t)(id0 + frame));
		}
	}
}

ZTEST(vrt_eth, test_ipv6_frame_checksum)
{
	static const uint8_t dmac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
	static const uint8_t smac[6] = {0x1c, 0x29, 0x04, 0xd1, 0x9c, 0xea};
	static const uint8_t sa[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x1e, 0x29, 0x04, 0xff,
				       0xfe, 0xd1, 0x9c, 0xea};
	static const uint8_t da[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x6d, 0x75, 0xaa, 0xbb,
				       0xcc, 0xdd, 0xee, 0xff};
	static const size_t mtus[] = {1492, 1500, 1280};
	static uint8_t body[1500] __aligned(4);

	for (size_t m = 0; m < ARRAY_SIZE(mtus); m++) {
		uint8_t frame[1600] __aligned(4);
		struct nf_tmpl t;
		size_t pl = vrt_payload_bytes(mtus[m], true);
		size_t udp_len = 8 + 20 + pl + 4;
		uint16_t csum;

		for (size_t k = 0; k < sizeof(body); k++) {
			body[k] = (uint8_t)(k * 13U + m);
		}
		nf_tmpl_init(&t, true, dmac, smac, sa, da, 4992, 40000);
		zassert_equal(t.len, 62U);
		memcpy(frame, t.b, t.len);
		vrt_data_header(frame + t.len, STREAM_ID, 3, vrt_packet_words(pl), 9, 99);
		nf_seal(&t, frame, VRT_DATA_HDR_BYTES, body, pl + 4U);
		zassert_equal(frame[12], 0x86);
		zassert_equal(frame[13], 0xdd);
		zassert_equal(frame[14] >> 4, 6);
		zassert_equal(frame[18] << 8 | frame[19], (int)udp_len, "payload length");
		zassert_equal(frame[20], 17);
		/* the whole IP packet is the MTU */
		zassert_true(40U + udp_len <= mtus[m]);
		zassert_mem_equal(frame + 22, sa, 16);
		zassert_mem_equal(frame + 38, da, 16);
		/* the datagram as the receiver sees it: UDP header, VRT header, samples, trailer */
		memcpy(frame + t.len + VRT_DATA_HDR_BYTES, body, pl + 4U);
		csum = (uint16_t)(frame[60] << 8 | frame[61]);
		zassert_not_equal(csum, 0, "IPv6 needs the checksum");
		/* with the checksum in place the sum of datagram and pseudo header is all ones */
		zassert_equal(ref_udp_csum(sa, da, frame + 54, udp_len), 0xffff, "mtu %zu", mtus[m]);
	}
}

/* ---- the subscription protocol ---- */

static const uint8_t KEY[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

ZTEST(vrt_eth, test_siphash_reference_vector)
{
	uint8_t in[15];

	for (int k = 0; k < 15; k++) {
		in[k] = (uint8_t)k;
	}
	zassert_equal(siphash24(KEY, in, 15), 0xa129ca6149be45e5ULL);
}

static struct peer mkpeer(uint8_t last, uint16_t port, bool v6)
{
	struct peer p = {.v6 = v6, .port = port};

	p.addr[0] = v6 ? 0xfe : 192;
	p.addr[1] = v6 ? 0x80 : 168;
	p.addr[v6 ? 15 : 3] = last;
	return p;
}

static void dg(struct ctl_proto *c, const struct peer *p, const char *magic, const uint8_t *ck,
	       int64_t now, struct ctl_out *o)
{
	uint8_t d[12];

	memcpy(d, magic, 4);
	if (ck != NULL) {
		memcpy(d + 4, ck, 8);
	}
	ctl_proto_dgram(c, p, d, ck != NULL ? 12U : 4U, now, o);
}

static void cookie(struct ctl_proto *c, const struct peer *p, int64_t now, uint8_t out[8])
{
	ctl_proto_cookie(c, p, (uint32_t)(now / CTL_EPOCH_MS), out);
}

ZTEST(vrt_eth, test_ctl_subscribe_busy_takeover_stop_timeout)
{
	struct ctl_proto c;
	struct ctl_out o;
	struct peer a = mkpeer(5, 5000, false), b = mkpeer(6, 5001, false);
	uint8_t ca[8], cb[8];
	int64_t t = 100000;

	ctl_proto_init(&c, KEY, 2, 5000);
	cookie(&c, &a, t, ca);
	cookie(&c, &b, t, cb);
	/* a hello gets a cookie and no state */
	dg(&c, &a, "VRTS", NULL, t, &o);
	zassert_equal(o.act, CTL_REPLY);
	zassert_equal(o.reply_len, 12U);
	zassert_mem_equal(o.reply, "VRTC", 4);
	zassert_mem_equal(o.reply + 4, ca, 8);
	zassert_false(c.live);
	/* a cookie of another source is no cookie: a new one comes back, nothing is subscribed */
	dg(&c, &b, "VRTS", ca, t, &o);
	zassert_equal(o.act, CTL_REPLY);
	zassert_mem_equal(o.reply, "VRTC", 4);
	zassert_false(c.live);
	dg(&c, &a, "VRTS", ca, t, &o);
	zassert_equal(o.evt, CTL_EVT_SUBSCRIBED);
	zassert_mem_equal(o.reply, "VRTK", 4);
	zassert_true(c.live);
	/* a renewal: no answer */
	t += 1000;
	dg(&c, &a, "VRTS", ca, t, &o);
	zassert_equal(o.evt, CTL_EVT_RENEWED);
	zassert_equal(o.act, CTL_IGNORE);
	/* a second host is told it is busy and changes nothing */
	dg(&c, &b, "VRTS", cb, t, &o);
	zassert_mem_equal(o.reply, "VRTB", 4);
	zassert_true(ctl_peer_same(&c.sub, &a));
	/* a stop from the other host does nothing, a takeover with its cookie does */
	dg(&c, &b, "VRTX", cb, t, &o);
	zassert_equal(o.evt, CTL_EVT_NONE);
	zassert_true(c.live);
	dg(&c, &b, "VRTT", cb, t, &o);
	zassert_equal(o.evt, CTL_EVT_TAKEOVER);
	zassert_true(ctl_peer_same(&c.sub, &b));
	/* the stop of the subscriber */
	dg(&c, &b, "VRTX", cb, t, &o);
	zassert_equal(o.evt, CTL_EVT_STOPPED);
	zassert_false(c.live);
	/* silence: five seconds and the subscriber is gone, then anyone may */
	dg(&c, &a, "VRTS", ca, t, &o);
	zassert_true(c.live);
	zassert_false(ctl_proto_expire(&c, t + 5000));
	zassert_true(ctl_proto_expire(&c, t + 5001));
	zassert_false(ctl_proto_expire(&c, t + 6000));
	/* the same host, 6 s on (the same 30 s epoch here): anyone may subscribe now */
	dg(&c, &b, "VRTS", cb, t + 6000, &o);
	zassert_equal(o.evt, CTL_EVT_SUBSCRIBED);
	zassert_true(ctl_peer_same(&c.sub, &b));
}

ZTEST(vrt_eth, test_ctl_cookie_epochs_blocked_and_ipv6)
{
	struct ctl_proto c;
	struct ctl_out o;
	struct peer a = mkpeer(9, 4000, true);
	uint8_t ck[8];
	int64_t t = 3 * CTL_EPOCH_MS + 100;

	ctl_proto_init(&c, KEY, 2, 5000);
	cookie(&c, &a, t, ck);
	/* the cookie of this epoch is good in the next and not in the one after */
	dg(&c, &a, "VRTS", ck, t + CTL_EPOCH_MS, &o);
	zassert_equal(o.evt, CTL_EVT_SUBSCRIBED);
	c.live = false;
	dg(&c, &a, "VRTS", ck, t + 2 * CTL_EPOCH_MS, &o);
	zassert_equal(o.evt, CTL_EVT_NONE);
	zassert_mem_equal(o.reply, "VRTC", 4);
	/* the interface is part of it (the family is the first byte of the hash input; the test of
	 * the MAC below has one family only)
	 */
	{
		struct ctl_proto d;
		uint8_t other[8];

		ctl_proto_init(&d, KEY, 3, 5000);
		ctl_proto_cookie(&d, &a, 3, other);
		ctl_proto_cookie(&c, &a, 3, ck);
		zassert_true(memcmp(ck, other, 8) != 0);
	}
	/* stream stop on the shell: nobody subscribes until it is lifted */
	c.blocked = true;
	cookie(&c, &a, t, ck);
	dg(&c, &a, "VRTS", ck, t, &o);
	zassert_equal(o.act, CTL_IGNORE);
	zassert_false(c.live);
	c.blocked = false;
	dg(&c, &a, "VRTS", ck, t, &o);
	zassert_equal(o.evt, CTL_EVT_SUBSCRIBED);
}

ZTEST(vrt_eth, test_ctl_malformed_and_commands_and_limits)
{
	struct ctl_proto c;
	struct ctl_out o;
	struct peer a = mkpeer(5, 5000, false), b = mkpeer(6, 5001, false);
	uint8_t ck[8], junk[1200];
	uint8_t cmd[8] = {0x60, 0, 0, 2, 0x5d, 0x5d, 0, 1};
	int64_t t = 50000;

	ctl_proto_init(&c, KEY, 2, 5000);
	memset(junk, 'V', sizeof(junk));
	for (size_t n = 0; n < 20; n++) {
		ctl_proto_dgram(&c, &a, junk, n, t, &o);
		zassert_equal(o.act, CTL_IGNORE, "length %zu", n);
	}
	ctl_proto_dgram(&c, &a, junk, sizeof(junk), t, &o);
	zassert_equal(o.act, CTL_IGNORE);
	memcpy(junk, "VRTS", 4);
	ctl_proto_dgram(&c, &a, junk, 14, t, &o);
	zassert_equal(o.act, CTL_IGNORE);
	/* 13 bytes are a subscribe with the flag byte; a cookie that is wrong gets a new one */
	ctl_proto_dgram(&c, &a, junk, 13, t, &o);
	zassert_equal(o.act, CTL_REPLY);
	zassert_false(c.live);
	ctl_proto_dgram(&c, &a, junk, 1000, t, &o);
	zassert_equal(o.act, CTL_IGNORE);
	memcpy(junk, "VRTZ", 4);
	ctl_proto_dgram(&c, &a, junk, 12, t, &o);
	zassert_equal(o.act, CTL_IGNORE);
	/* a command packet comes back for the caller to parse, with the sender's standing */
	ctl_proto_dgram(&c, &a, cmd, sizeof(cmd), t, &o);
	zassert_equal(o.act, CTL_CMD);
	zassert_false(o.is_sub);
	cookie(&c, &a, t, ck);
	dg(&c, &a, "VRTS", ck, t, &o);
	ctl_proto_dgram(&c, &a, cmd, sizeof(cmd), t, &o);
	zassert_true(o.is_sub);
	ctl_proto_dgram(&c, &b, cmd, sizeof(cmd), t, &o);
	zassert_equal(o.act, CTL_CMD);
	zassert_false(o.is_sub, "another host's port is not the subscriber");
	ctl_proto_dgram(&c, &a, cmd, 6, t, &o);
	zassert_equal(o.act, CTL_IGNORE);
	/* queries: ten a second in all, then again */
	for (int k = 0; k < 10; k++) {
		struct peer q = mkpeer((uint8_t)(100 + k / 2), 7000, false);

		zassert_true(ctl_proto_query_ok(&c, &q, t), "answer %d", k);
	}
	zassert_false(ctl_proto_query_ok(&c, &b, t + 10), "ten a second in all");
	zassert_true(ctl_proto_query_ok(&c, &b, t + 1100));
	zassert_true(ctl_proto_query_ok(&c, &b, t + 1100));
	zassert_false(ctl_proto_query_ok(&c, &b, t + 1100), "two a second from one source");
}

static void live_subscriber(struct ctl_proto *c, const struct peer *a, int64_t t)
{
	struct ctl_out o;
	uint8_t ck[8];

	cookie(c, a, t, ck);
	dg(c, a, "VRTS", ck, t, &o);
	zassert_equal(o.evt, CTL_EVT_SUBSCRIBED);
}

ZTEST(vrt_eth, test_ctl_budgets_per_source_and_for_the_subscriber)
{
	struct ctl_proto c;
	struct ctl_out o;
	struct peer a = mkpeer(5, 5000, false), x = mkpeer(50, 6000, false), imp = a;
	int adm, answers, total;
	int64_t t = 90000;

	ctl_proto_init(&c, KEY, 2, 5000);
	live_subscriber(&c, &a, t);
	/* 20 datagrams a second from one source, and the next second again */
	adm = 0;
	for (int k = 0; k < 100; k++) {
		adm += ctl_proto_admit(&c, &x, t + 10);
	}
	zassert_equal(adm, 20, "20 a second to a source, got %d", adm);
	adm = 0;
	for (int k = 0; k < 100; k++) {
		adm += ctl_proto_admit(&c, &x, t + 1100);
	}
	zassert_equal(adm, 20);
	/* a host with the subscriber's IP, on another port and with another MAC, uses its own budget
	 * and not the subscriber's
	 */
	imp.port = 5001;
	imp.mac[5] = 7;
	for (int k = 0; k < 500; k++) {
		(void)ctl_proto_admit(&c, &imp, t + 2000);
	}
	adm = 0;
	for (int k = 0; k < 100; k++) {
		adm += ctl_proto_admit(&c, &a, t + 2000);
	}
	zassert_equal(adm, 60, "the subscriber has 60 a second of its own, got %d", adm);
	/* many sources: 300 a second in all, the subscriber outside them */
	total = 0;
	for (int s = 0; s < 100; s++) {
		struct peer f = mkpeer((uint8_t)(100 + s), 7000, false);

		for (int k = 0; k < 20; k++) {
			total += ctl_proto_admit(&c, &f, t + 4000);
		}
	}
	zassert_equal(total, 300, "300 a second in all, got %d", total);
	zassert_true(ctl_proto_admit(&c, &a, t + 4000), "the subscriber is admitted in the flood");
	/* cookie answers: 5 a second to the imposter, the subscriber's refresh is answered */
	answers = 0;
	for (int k = 0; k < 500; k++) {
		dg(&c, &imp, "VRTS", NULL, t + 6000, &o);
		answers += o.act == CTL_REPLY;
	}
	zassert_equal(answers, 5, "got %d", answers);
	dg(&c, &a, "VRTS", NULL, t + 6000, &o);
	zassert_equal(o.act, CTL_REPLY, "the subscriber's cookie refresh");
	/* and 100 a second in all to others, the subscriber's refresh not among them */
	answers = 0;
	for (int s = 0; s < 40; s++) {
		struct peer f = mkpeer((uint8_t)(100 + s), 7000, false);

		for (int k = 0; k < 5; k++) {
			dg(&c, &f, "VRTS", NULL, t + 8000, &o);
			answers += o.act == CTL_REPLY;
		}
	}
	zassert_equal(answers, 100, "100 cookie answers a second in all, got %d", answers);
	dg(&c, &a, "VRTS", NULL, t + 8000, &o);
	zassert_equal(o.act, CTL_REPLY, "the subscriber's refresh after the flood");
	/* queries: 2 a second to the imposter, the subscriber's counters are its own */
	zassert_true(ctl_proto_query_ok(&c, &imp, t + 10000));
	zassert_true(ctl_proto_query_ok(&c, &imp, t + 10000));
	zassert_false(ctl_proto_query_ok(&c, &imp, t + 10000));
	zassert_true(ctl_proto_query_ok(&c, &a, t + 10000), "the subscriber's own query counters");
}

/* ---- cells: payload change, loss, wrap of the counters, the checksum summed while copying ---- */

ZTEST(vrt_eth, test_cells_payload_change_wrap_and_fused_sum)
{
	static const uint32_t pls[] = {1440, 1420, 1448, 1428};
	uint32_t rd;
	uint64_t index = 5000;

	vrt_cells_init(&cells, &cell_mem[0][0], cell_meta, NCELLS, STRIDE);
	/* the counters are 32 bit and wrap: the cells are a power of two */
	atomic_set(&cells.ready, (atomic_val_t)0xfffffff0U);
	atomic_set(&cells.released, (atomic_val_t)0xfffffff0U);
	rd = 0xfffffff0U;
	for (size_t k = 0; k < ARRAY_SIZE(pls); k++) {
		uint32_t pl = pls[k];

		vrt_cells_start(&cells, pl, 40000000U, 2412000000ULL, 3, 0, true, k != 0);
		for (int n = 0; n < 5; n++) {
			fill_node(index);
			zassert_equal(vrt_cells_push(&cells, node, sizeof(node), index), 0);
			vrt_cells_commit(&cells);
			index += 2040U;
		}
		zassert_true(vrt_cells_avail(&cells, rd) >= 12U, "cells in %zu: %u", k,
			     vrt_cells_avail(&cells, rd));
		for (uint32_t j = 0; j < 12U; j++, rd++) {
			const struct vrt_cell_meta *m = vrt_cell_meta(&cells, rd);
			const uint8_t *p = vrt_cell_ptr(&cells, rd);

			zassert_equal(m->bytes, pl);
			zassert_equal(m->loss, k != 0 && j == 0, "loss of cell %u of run %zu", j, k);
			zassert_equal(m->freq_hz, 2412000000ULL);
			zassert_equal(m->gain, 3);
			zassert_true(m->sum_valid);
			/* the sum made while copying is the sum of the cell, bit for bit */
			zassert_equal(nf_fold(m->sum), nf_fold(nf_sum(p, pl, 0)), "sum of cell %u", j);
			vrt_cells_done(&cells, rd);
		}
		/* what is left of the run is dropped by the next start */
		rd = (uint32_t)atomic_get(&cells.ready);
		atomic_set(&cells.released, (atomic_val_t)rd);
	}
}

ZTEST(vrt_eth, test_ipv6_seal_with_known_sum_is_the_same)
{
	static const uint8_t dmac[6] = {2, 1, 2, 3, 4, 5}, smac[6] = {0x1c, 0x29, 4, 0xd1, 0x9c, 0xea};
	static const uint8_t sa[16] = {0xfd, 1, 2, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
	static const uint8_t da[16] = {0xfd, 1, 2, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};
	static uint8_t body[1500] __aligned(4);
	/* payloads in whole words (the stream makes no others), the largest per MTU and the smallest */
	static const size_t pls[] = {4, 8, 1420, 1428, 716};

	for (size_t k = 0; k < ARRAY_SIZE(pls); k++) {
		uint8_t f1[200] __aligned(4), f2[200] __aligned(4);
		struct nf_tmpl t1, t2;
		size_t pl = pls[k];

		for (size_t j = 0; j < sizeof(body); j++) {
			body[j] = (uint8_t)(j * 31U + k);
		}
		nf_tmpl_init(&t1, true, dmac, smac, sa, da, 4992, 5555);
		t2 = t1;
		memcpy(f1, t1.b, t1.len);
		memcpy(f2, t2.b, t2.len);
		vrt_data_header(f1 + t1.len, STREAM_ID, 1, vrt_packet_words(pl), 1, 2);
		memcpy(f2 + t2.len, f1 + t1.len, VRT_DATA_HDR_BYTES);
		vrt_put_trailer(body + pl, vrt_trailer_word(true, false, true));
		nf_seal(&t1, f1, VRT_DATA_HDR_BYTES, body, pl + 4U);
		nf_seal2(&t2, f2, VRT_DATA_HDR_BYTES, body, pl + 4U, nf_sum(body, pl, 0), pl);
		zassert_mem_equal(f1, f2, t1.len + VRT_DATA_HDR_BYTES, "pl %zu", pl);
	}
}

ZTEST(vrt_eth, test_ctl_zero_checksum_flag_and_ipv6_seal)
{
	struct ctl_proto c;
	struct ctl_out o;
	struct peer a = mkpeer(9, 4000, true);
	uint8_t d[13], ck[8];
	static const uint8_t mac[6] = {2, 1, 2, 3, 4, 5};
	uint8_t addr[16] = {0xfe, 0x80, [15] = 1};
	static uint8_t body[200] __aligned(4);
	uint8_t f[200] __aligned(4);
	struct nf_tmpl t;

	ctl_proto_init(&c, KEY, 2, 5000);
	cookie(&c, &a, 1000, ck);
	memcpy(d, "VRTS", 4);
	memcpy(d + 4, ck, 8);
	d[12] = 1;
	ctl_proto_dgram(&c, &a, d, 13, 1000, &o);
	zassert_equal(o.evt, CTL_EVT_SUBSCRIBED);
	zassert_true(o.nocsum);
	zassert_true(c.sub.nocsum);
	d[12] = 0;
	ctl_proto_dgram(&c, &a, d, 13, 1100, &o);
	zassert_false(o.nocsum);
	/* the frame: the checksum field stays 0, the lengths are right */
	nf_tmpl_init(&t, true, mac, mac, addr, addr, 4992, 40000);
	t.zero_csum = true;
	memcpy(f, t.b, t.len);
	nf_seal(&t, f, 0, body, 100);
	zassert_equal(f[60] | f[61], 0);
	zassert_equal(f[18] << 8 | f[19], 108);
}

ZTEST(vrt_eth, test_ctl_cookie_covers_the_mac_and_the_local_address)
{
	struct ctl_proto c;
	struct ctl_out o;
	struct peer a = mkpeer(5, 5000, false), a2 = a, a3 = a;
	uint8_t ck[8], ck2[8], ck3[8];
	int64_t t = 70000;

	a.mac[5] = 1;
	a2.mac[5] = 2;
	a3.mac[5] = 1;
	a3.local[3] = 9;
	ctl_proto_init(&c, KEY, 2, 5000);
	cookie(&c, &a, t, ck);
	cookie(&c, &a2, t, ck2);
	cookie(&c, &a3, t, ck3);
	zassert_true(memcmp(ck, ck2, 8) != 0, "two MACs on one IP have two cookies");
	zassert_true(memcmp(ck, ck3, 8) != 0, "so do two local addresses");
	dg(&c, &a, "VRTS", ck, t, &o);
	zassert_equal(o.evt, CTL_EVT_SUBSCRIBED);
	/* the same IP and port from another MAC, with the first one's cookie: no renewal, a new cookie,
	 * and the stream stays where it was
	 */
	dg(&c, &a2, "VRTS", ck, t + 100, &o);
	zassert_equal(o.evt, CTL_EVT_NONE);
	zassert_mem_equal(o.reply, "VRTC", 4);
	zassert_mem_equal(o.reply + 4, ck2, 8);
	zassert_mem_equal(c.sub.mac, a.mac, 6);
	/* and a stop from there does nothing */
	dg(&c, &a2, "VRTX", ck2, t + 100, &o);
	zassert_true(c.live);
	/* a renewal to another address of the board: the old cookie is no cookie for it, its own is a
	 * different stream (busy), and the stream stays
	 */
	dg(&c, &a3, "VRTS", ck, t + 200, &o);
	zassert_mem_equal(o.reply, "VRTC", 4);
	dg(&c, &a3, "VRTS", ck3, t + 200, &o);
	zassert_mem_equal(o.reply, "VRTB", 4);
	zassert_equal(c.sub.local[3], a.local[3]);
}

/* ---- the settings epoch ---- */

ZTEST(vrt_eth, test_epoch_counts_changes_only)
{
	uint64_t f = 2412000000ULL;
	uint32_t r = 20000000U;
	int g = -1;
	uint8_t e = 0;

	/* a command that sets what is set changes nothing */
	zassert_false(vrt_set_u64(&f, 2412000000ULL, &e));
	zassert_false(vrt_set_u32(&r, 20000000U, &e));
	zassert_false(vrt_set_int(&g, -1, &e));
	zassert_equal(e, 0);
	/* A to B to A: two changes, two increments */
	zassert_true(vrt_set_u32(&r, 40000000U, &e));
	zassert_equal(e, 1);
	zassert_true(vrt_set_u32(&r, 20000000U, &e));
	zassert_equal(e, 2);
	zassert_true(vrt_set_u64(&f, 2440000000ULL, &e));
	zassert_true(vrt_set_int(&g, 30, &e));
	zassert_equal(e, 4);
	/* eight bits: it wraps */
	for (int k = 0; k < 252; k++) {
		(void)vrt_set_int(&g, k & 1 ? 1 : 2, &e);
	}
	zassert_equal(e, 0);
}

ZTEST(vrt_eth, test_every_run_has_its_epoch_and_number_in_its_cells)
{
	uint32_t rd = 0;
	uint64_t index = 100;
	static const uint8_t epochs[] = {1, 2, 1};   /* A, B, A: the epoch is not the settings, it counts */
	uint32_t runs[3];

	vrt_cells_init(&cells, &cell_mem[0][0], cell_meta, NCELLS, STRIDE);
	for (size_t k = 0; k < ARRAY_SIZE(epochs); k++) {
		vrt_cells_start(&cells, 1440, 20000000U, 2412000000ULL, -1, epochs[k], false, k != 0);
		fill_node(index);
		zassert_equal(vrt_cells_push(&cells, node, sizeof(node), index), 0);
		vrt_cells_commit(&cells);
		index += 2040U;
		zassert_true(vrt_cells_avail(&cells, rd) >= 2U);
		runs[k] = vrt_cell_meta(&cells, rd)->run;
		zassert_equal(vrt_cell_meta(&cells, rd)->epoch, epochs[k], "run %zu", k);
		/* all the run's cells say so: a context built from any of them has this run's epoch */
		zassert_equal(vrt_cell_meta(&cells, rd + 1U)->epoch, epochs[k]);
		zassert_equal(vrt_cell_meta(&cells, rd + 1U)->run, runs[k]);
		for (uint32_t j = rd; j < cells.wr; j++) {
			vrt_cells_done(&cells, j);
		}
		rd = cells.wr;
	}
	/* the same epoch twice is still two runs: a context is due before each */
	zassert_true(runs[0] != runs[2]);
	zassert_true(runs[0] != runs[1] && runs[1] != runs[2]);
}

/* The context as ctl_build_context() makes it, with the epoch in the state and event field (CIF0 bit
 * 16, user bits 7..0): the words, and libvrt reads it back
 */
ZTEST(vrt_eth, test_context_carries_the_epoch)
{
	struct vrt_packet p, r;
	uint32_t buf[64];
	int32_t w;

	for (int epoch = 0; epoch < 256; epoch += 51) {
		vrt_init_packet(&p);
		p.header.packet_type = VRT_PT_IF_CONTEXT;
		p.fields.stream_id = STREAM_ID;
		p.header.nd0 = true;
		p.if_context.has.v49_spec_compliance = true;
		p.if_context.v49_spec_compliance = VRT_V49_2;
		p.if_context.has.sample_rate = true;
		p.if_context.sample_rate = 8e6;
		p.if_context.has.state_and_event_indicators = true;
		p.if_context.state_and_event_indicators.user_defined = (uint8_t)epoch;
		p.header.packet_size = (uint16_t)vrt_words_packet(&p);
		w = vrt_write_packet(&p, buf, 64, true);
		zassert_true(w > 0);
		/* header, stream id, CIF0 (bit 16 set), CIF1, rate (2 words), the field, the CIF1 field */
		zassert_equal(buf[2] & BIT(16), BIT(16));
		zassert_equal(buf[2 + 1 + 1 + 2], (uint32_t)epoch, "the state and event word has only the epoch");
		vrt_init_packet(&r);
		zassert_equal(vrt_read_packet(buf, w, &r, true), w);
		zassert_true(r.if_context.has.state_and_event_indicators);
		zassert_equal(r.if_context.state_and_event_indicators.user_defined, epoch);
	}
}
