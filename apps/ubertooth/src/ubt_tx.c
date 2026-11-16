/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Packet builders of the Ubertooth transmit modes, see ubt_tx.h. The whitening
 * and CRC use the vendored upstream tables.
 */

#include "ubt_tx.h"

#include <errno.h>
#include <string.h>

#include "upstream/bluetooth_le.h"
#include "upstream/ubertooth_interface.h"

/* struct generic_tx_packet as the request carries it. */
#define GENERIC_HDR    8U
#define GENERIC_LEN_AT 6U

#define PRBS9_BITS 9U
#define PRBS9_FULL 0x1ffU

unsigned int ubt_tx_adv_index(uint16_t channel_mhz)
{
	return btle_channel_index(channel_mhz);
}

int ubt_tx_adv_pdu(uint8_t *pdu, const uint8_t mac[6], const uint8_t *adv, unsigned int adv_len,
		   bool txadd)
{
	if (adv_len > UBT_TX_ADV_DATA_MAX || (adv_len != 0U && adv == NULL)) {
		return -EMSGSIZE;
	}
	pdu[0] = txadd ? 0x40U : 0x00U;
	pdu[1] = (uint8_t)(6U + adv_len);
	for (unsigned int i = 0; i < 6U; i++) {
		pdu[2U + i] = mac[5U - i];
	}
	if (adv_len != 0U) {
		memcpy(pdu + 8, adv, adv_len);
	}
	return (int)(8U + adv_len);
}

int ubt_tx_build_le(struct ubt_tx_pkt *p, uint32_t aa, uint32_t crc_init_rev, const uint8_t *pdu,
		    unsigned int pdu_len, uint16_t channel_mhz)
{
	uint8_t buf[UBT_TX_ADV_PDU_MAX + 3U];
	uint32_t crc;
	unsigned int idx = whitening_index[btle_channel_index(channel_mhz)];
	unsigned int n = 0;

	if (pdu_len < 2U || pdu_len > UBT_TX_ADV_PDU_MAX) {
		return -EMSGSIZE;
	}
	memcpy(buf, pdu, pdu_len);
	crc = btle_calc_crc(crc_init_rev, buf, (int)pdu_len);
	buf[pdu_len] = (uint8_t)crc;
	buf[pdu_len + 1U] = (uint8_t)(crc >> 8);
	buf[pdu_len + 2U] = (uint8_t)(crc >> 16);

	memset(p, 0, sizeof(*p));
	/* Preamble: alternating, ending opposite to the first access address bit. */
	p->bits[n++] = (aa & 1U) != 0U ? 0x55U : 0xaaU;
	for (unsigned int i = 0; i < 4U; i++) {
		p->bits[n++] = (uint8_t)(aa >> (8U * i));
	}
	/* Whiten PDU and CRC: the LFSR bit goes into the byte's low bit first, as it is sent. */
	for (unsigned int i = 0; i < pdu_len + 3U; i++) {
		uint8_t in = buf[i], out = 0;

		for (unsigned int j = 0; j < 8U; j++) {
			unsigned int bit = (in & 1U) ^ whitening[idx];

			idx = (idx + 1U) % sizeof(whitening);
			in >>= 1;
			out |= (uint8_t)(bit << j);
		}
		p->bits[n++] = out;
	}
	p->nbits = (uint16_t)(8U * n);
	p->msb_first = false;
	p->channel_mhz = channel_mhz;
	p->dev_hz = ubt_tx_mdmctrl_dev(UBT_TX_MDMCTRL_BLE);
	return p->nbits;
}

int ubt_tx_build_adv(struct ubt_tx_pkt *p, const uint8_t mac[6], const uint8_t *adv,
		     unsigned int adv_len, uint16_t channel_mhz, uint32_t crc_init_rev)
{
	uint8_t pdu[UBT_TX_ADV_PDU_MAX];
	int len = ubt_tx_adv_pdu(pdu, mac, adv, adv_len, false);

	if (len < 0) {
		return len;
	}
	return ubt_tx_build_le(p, UBT_TX_ADV_AA, crc_init_rev, pdu, (unsigned int)len, channel_mhz);
}

size_t ubt_tx_generic_load(struct ubt_tx_generic *g, const uint8_t *req, size_t req_len)
{
	uint8_t img[GENERIC_HDR + UBT_TX_GENERIC_DATA_MAX];
	size_t n;

	if (req_len <= GENERIC_LEN_AT) {
		return 0;
	}
	/* The image of the struct as it is now, then the request over its start. */
	img[0] = (uint8_t)g->synch;
	img[1] = (uint8_t)(g->synch >> 8);
	img[2] = (uint8_t)g->syncl;
	img[3] = (uint8_t)(g->syncl >> 8);
	img[4] = (uint8_t)g->channel;
	img[5] = (uint8_t)(g->channel >> 8);
	img[6] = g->length;
	img[7] = g->pa_level;
	memcpy(img + GENERIC_HDR, g->data, UBT_TX_GENERIC_DATA_MAX);
	n = 7U + req[GENERIC_LEN_AT];
	if (n > req_len) {
		n = req_len;
	}
	if (n > sizeof(img)) {
		n = sizeof(img);
	}
	memcpy(img, req, n);
	g->synch = (uint16_t)(img[0] | (img[1] << 8));
	g->syncl = (uint16_t)(img[2] | (img[3] << 8));
	g->channel = (uint16_t)(img[4] | (img[5] << 8));
	g->length = img[6];
	g->pa_level = img[7];
	memcpy(g->data, img + GENERIC_HDR, UBT_TX_GENERIC_DATA_MAX);
	return n;
}

int ubt_tx_build_generic(struct ubt_tx_pkt *p, const struct ubt_tx_generic *g)
{
	unsigned int len = g->length < UBT_TX_GENERIC_FIFO ? g->length : UBT_TX_GENERIC_FIFO;
	unsigned int n = 0;

	memset(p, 0, sizeof(*p));
	/* PRE_BYTES 6 = 32 bytes of 01010101, sent from the most significant bit. */
	for (unsigned int i = 0; i < UBT_TX_GENERIC_PREAMBLE_BYTES; i++) {
		p->bits[n++] = 0x55U;
	}
	p->bits[n++] = (uint8_t)(g->synch >> 8);
	p->bits[n++] = (uint8_t)g->synch;
	p->bits[n++] = (uint8_t)(g->syncl >> 8);
	p->bits[n++] = (uint8_t)g->syncl;
	memcpy(p->bits + n, g->data, len);
	n += len;
	p->nbits = (uint16_t)(8U * n);
	p->msb_first = true;
	p->channel_mhz = g->channel;
	p->dev_hz = ubt_tx_mdmctrl_dev(UBT_TX_GENERIC_MDMCTRL);
	p->pa_level = g->pa_level;
	return p->nbits;
}

int ubt_tx_build_test(struct ubt_tx_pkt *p, uint8_t modulation, uint16_t channel_mhz,
		      enum ubt_tx_test_kind kind, uint16_t seed)
{
	uint16_t mdmctrl;
	uint32_t state = (seed & PRBS9_FULL) != 0U ? seed & PRBS9_FULL : PRBS9_FULL;
	unsigned int ones = 0;

	if (modulation == MOD_BT_BASIC_RATE) {
		mdmctrl = UBT_TX_MDMCTRL_BR;
	} else if (modulation == MOD_BT_LOW_ENERGY) {
		mdmctrl = UBT_TX_MDMCTRL_BLE;
	} else {
		return -EINVAL;
	}
	memset(p, 0, sizeof(*p));
	p->nbits = UBT_TX_TEST_SYMBOLS;
	p->msb_first = false;
	p->channel_mhz = channel_mhz;
	p->dev_hz = ubt_tx_mdmctrl_dev(mdmctrl);
	if (kind == UBT_TX_TEST_CARRIER) {
		p->carrier = true;
		return p->nbits;
	}
	p->loop = true;
	for (unsigned int i = 0; i < UBT_TX_TEST_SYMBOLS; i++) {
		unsigned int bit = state & 1U;

		state = (state >> 1) | ((((state >> 0) ^ (state >> 4)) & 1U) << (PRBS9_BITS - 1U));
		p->bits[i / 8U] |= (uint8_t)(bit << (i % 8U));
		ones += bit;
	}
	/* Balance: flip the surplus bits, walking the pattern with a step coprime to its length. */
	for (unsigned int j = 0, at = 13; ones != UBT_TX_TEST_SYMBOLS / 2U;
	     j++, at = (at + 7U) % UBT_TX_TEST_SYMBOLS) {
		unsigned int bit = (p->bits[at / 8U] >> (at % 8U)) & 1U;

		if ((ones > UBT_TX_TEST_SYMBOLS / 2U) == (bit != 0U)) {
			p->bits[at / 8U] ^= (uint8_t)(1U << (at % 8U));
			ones = bit != 0U ? ones - 1U : ones + 1U;
		}
		if (j > 4U * UBT_TX_TEST_SYMBOLS) {
			break;
		}
	}
	return p->nbits;
}

void ubt_tx_burst(const struct ubt_tx_pkt *p, struct gfsk_burst *b)
{
	memset(b, 0, sizeof(*b));
	b->kind = p->carrier ? GFSK_CARRIER : (p->loop ? GFSK_LOOP : GFSK_BURST);
	b->bits = p->bits;
	b->nbits = p->nbits;
	b->msb_first = p->msb_first;
	b->end = GFSK_END_CENTRE;
	b->lead = 4;
	b->trail = 4;
	b->ramp = 120;
}
