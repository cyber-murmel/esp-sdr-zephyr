/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The packet records of records.h: the LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR header before a
 * Bluetooth LE packet, the LINKTYPE_IEEE802_15_4_TAP header before an 802.15.4 frame, the
 * burst record of pcap_proto.h, and the metadata comment. Little endian byte streams built with
 * byte stores; the comment with integer arithmetic. Plain C, no Zephyr.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "records.h"

/* The burst record is the packed header as it lies in memory on the little-endian targets. */
_Static_assert(sizeof(struct pcap_burst_hdr) == 52U, "pcap_burst_hdr layout");

static void put_u16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* ---- Bluetooth LE ---- */

int rec_ble(uint8_t *buf, size_t cap, const struct dec_ble_pkt *p, int rf_channel)
{
	size_t size;
	uint16_t flags;

	if (p == NULL || p->len > sizeof(p->air) || rf_channel < 0 ||
	    rf_channel > REC_BLE_RF_CHANNEL_MAX) {
		return -EINVAL;
	}
	size = REC_BLE_HDR + p->len;
	if (size > cap) {
		return -ENOSPC;
	}
	if (buf == NULL) {
		return -EINVAL;
	}

	flags = REC_BLE_F_DEWHITENED | REC_BLE_F_REF_AA_VALID | REC_BLE_F_OFFENSES_VALID;
	if (p->crc_checked) {
		flags |= REC_BLE_F_CRC_CHECKED;
	}
	if (p->crc_ok) {
		flags |= REC_BLE_F_CRC_VALID;
	}
	if (p->phy == DEC_BLE_PHY_2M) {
		flags |= REC_BLE_PHY_2M << REC_BLE_F_PHY_SHIFT;
	}
	buf[0] = (uint8_t)rf_channel;
	/* Signal and noise power, dBm: unknown, flagged invalid. */
	buf[1] = 0U;
	buf[2] = 0U;
	buf[3] = p->aa_errors;
	put_u32(buf + 4, p->aa);
	put_u16(buf + 8, flags);
	memcpy(buf + REC_BLE_HDR, p->air, p->len);
	return (int)size;
}

/* ---- IEEE 802.15.4 ---- */

/* One TLV: @p len value bytes from @p val, zero padded to 4. Returns the next position. */
static uint8_t *tlv_put(uint8_t *p, uint16_t type, const void *val, size_t len)
{
	size_t padded = (len + 3U) & ~(size_t)3U;

	put_u16(p, type);
	put_u16(p + 2, (uint16_t)len);
	memcpy(p + 4, val, len);
	memset(p + 4 + len, 0, padded - len);
	return p + 4 + padded;
}

static uint8_t *tlv_u8(uint8_t *p, uint16_t type, uint8_t v)
{
	return tlv_put(p, type, &v, 1U);
}

static uint8_t *tlv_u32(uint8_t *p, uint16_t type, uint32_t v)
{
	uint8_t le[4];

	put_u32(le, v);
	return tlv_put(p, type, le, sizeof(le));
}

int rec_wpan(uint8_t *buf, size_t cap, const struct dec_wpan_pkt *p, uint64_t freq_hz)
{
	size_t hdr, size;
	uint8_t *q;
	uint32_t khz_bits;
	float khz;

	if (p == NULL || p->len > sizeof(p->psdu)) {
		return -EINVAL;
	}
	hdr = REC_TAP_HDR_MAX - (p->channel != 0U ? 0U : 8U);
	size = hdr + p->len;
	if (size > cap) {
		return -ENOSPC;
	}
	if (buf == NULL) {
		return -EINVAL;
	}

	buf[0] = REC_TAP_VERSION;
	buf[1] = 0U;
	put_u16(buf + 2, (uint16_t)hdr);
	q = buf + 4;
	q = tlv_u8(q, REC_TAP_TLV_FCS_TYPE, REC_TAP_FCS_CRC16);
	q = tlv_u32(q, REC_TAP_TLV_BIT_RATE, REC_TAP_BIT_RATE_OQPSK);
	if (p->channel != 0U) {
		uint8_t chan[3];

		/* u16 channel number, u8 channel page 0. */
		put_u16(chan, p->channel);
		chan[2] = 0U;
		q = tlv_put(q, REC_TAP_TLV_CHANNEL_ASSIGNMENT, chan, sizeof(chan));
	}
	q = tlv_u8(q, REC_TAP_TLV_LQI, p->lqi);
	/* kHz as a float: the whole kilohertz exact, the remainder rounded by the addition. */
	khz = (float)(freq_hz / 1000U) + (float)(freq_hz % 1000U) / 1000.0f;
	memcpy(&khz_bits, &khz, sizeof(khz_bits));
	q = tlv_u32(q, REC_TAP_TLV_CHANNEL_FREQUENCY, khz_bits);
	memcpy(q, p->psdu, p->len);
	return (int)size;
}

/* ---- the burst record ---- */

int rec_burst(uint8_t *buf, size_t cap, const struct pcap_burst_hdr *h, const uint8_t *bits)
{
	size_t nbytes, size;

	if (h == NULL || h->nbits > PCAP_BURST_BITS_MAX || (bits == NULL && h->nbits != 0U)) {
		return -EINVAL;
	}
	nbytes = (h->nbits + 7U) / 8U;
	size = sizeof(*h) + nbytes;
	if (size > cap) {
		return -ENOSPC;
	}
	if (buf == NULL) {
		return -EINVAL;
	}

	memcpy(buf, h, sizeof(*h));
	buf[0] = PCAP_BURST_VERSION;
	buf[1] = (uint8_t)sizeof(*h);
	if (nbytes != 0U) {
		memcpy(buf + sizeof(*h), bits, nbytes);
	}
	return (int)size;
}

/* ---- the comment ---- */

/* The text being built: what fits is written, everything is counted. */
struct cur {
	char *s;
	size_t cap, len;
};

static void cur_putc(struct cur *c, char ch)
{
	/* The last byte stays for the NUL. */
	if (c->len + 1U < c->cap) {
		c->s[c->len] = ch;
	}
	c->len++;
}

static void cur_str(struct cur *c, const char *str)
{
	while (*str != '\0') {
		cur_putc(c, *str++);
	}
}

/* @p v in decimal, zero padded to at least @p digits. */
static void cur_uint(struct cur *c, uint64_t v, unsigned int digits)
{
	char tmp[24];
	unsigned int n = 0U;

	do {
		tmp[n++] = (char)('0' + (unsigned int)(v % 10U));
		v /= 10U;
	} while (v != 0U);
	while (n < digits && n < sizeof(tmp)) {
		tmp[n++] = '0';
	}
	while (n > 0U) {
		cur_putc(c, tmp[--n]);
	}
}

/* @p v / @p d rounded half up. */
static uint64_t round_div(uint64_t v, uint64_t d)
{
	return v / d + (v % d >= (d + 1U) / 2U ? 1U : 0U);
}

/* @p a in units of the last printed digit, with @p decimals digits after the point. */
static void cur_ufixed(struct cur *c, uint64_t a, unsigned int decimals)
{
	uint64_t scale = 1U;

	for (unsigned int i = 0U; i < decimals; i++) {
		scale *= 10U;
	}
	cur_uint(c, a / scale, 1U);
	if (decimals != 0U) {
		cur_putc(c, '.');
		cur_uint(c, a % scale, decimals);
	}
}

/*
 * @p v in units of @p lsb, rounded half away from zero, with @p decimals digits after the
 * point: v -421, lsb 1, decimals 1 is "-42.1"; a "+" before zero and positive values when
 * @p plus is set.
 */
static void cur_fixed(struct cur *c, int64_t v, uint64_t lsb, unsigned int decimals, int plus)
{
	uint64_t mag = v < 0 ? 0U - (uint64_t)v : (uint64_t)v;
	uint64_t a = round_div(mag, lsb);

	if (v < 0 && a != 0U) {
		cur_putc(c, '-');
	} else if (plus) {
		cur_putc(c, '+');
	}
	cur_ufixed(c, a, decimals);
}

int rec_comment(char *s, size_t cap, const struct rec_meta *m)
{
	struct cur c = {.s = s, .cap = cap, .len = 0U};

	if (m == NULL || (s == NULL && cap != 0U)) {
		return -EINVAL;
	}

	cur_str(&c, "esdr: ");
	cur_ufixed(&c, round_div(m->freq_hz, 1000U), 3U);
	cur_str(&c, " MHz, ch ");
	cur_uint(&c, m->channel, 1U);
	cur_putc(&c, '/');
	cur_uint(&c, m->channels, 1U);
	cur_str(&c, " at ");
	cur_ufixed(&c, round_div(m->rate_hz, 1000U), 3U);
	cur_str(&c, " MS/s");
	if (m->phy != NULL) {
		cur_str(&c, ", ");
		cur_str(&c, m->phy);
	}
	cur_str(&c, ", offset ");
	cur_fixed(&c, m->offset_hz, 100U, 1U, 1);
	cur_str(&c, " kHz, power ");
	cur_fixed(&c, m->power_dbfs_x10, 1U, 1U, 0);
	cur_str(&c, " dBFS, floor ");
	cur_fixed(&c, m->floor_dbfs_x10, 1U, 1U, 0);
	cur_str(&c, " dBFS, snr ");
	cur_fixed(&c, (int64_t)m->power_dbfs_x10 - m->floor_dbfs_x10, 1U, 1U, 0);
	cur_str(&c, " dB, gain ");
	if (m->gain == REC_GAIN_AUTO) {
		cur_str(&c, "auto");
	} else {
		cur_uint(&c, m->gain, 1U);
	}
	cur_str(&c, ", ");
	cur_uint(&c, m->duration_us, 1U);
	cur_str(&c, " us");

	if (c.len >= cap) {
		if (cap != 0U) {
			s[cap - 1U] = '\0';
		}
		return -ENOSPC;
	}
	s[c.len] = '\0';
	return (int)c.len;
}
