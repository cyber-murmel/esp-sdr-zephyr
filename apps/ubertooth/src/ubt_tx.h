/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Packet builders of the Ubertooth transmit modes, portable and without a
 * radio: BTLE_SLAVE advertising, TX_GENERIC_PACKET and TX_TEST. Each builds
 * what the CC2400 packet engine put on the air in upstream firmware (bluetooth_rxtx.c
 * le_transmit(), tx_generic(), cc2400_txtest()), as a bit array for gfsk_mod.
 *
 * Bit order. A struct ubt_tx_pkt holds the transmitted bits in air order, packed:
 *  - msb_first = false (Bluetooth LE): bit i is bit (i % 8) of bits[i / 8], so the
 *    array is the over-the-air byte sequence, every byte least significant bit first,
 *    as the LE specification defines it: preamble, access address (little endian),
 *    PDU, CRC (low byte first). PDU and CRC are whitened.
 *  - msb_first = true (CC2400 FIFO order): bit i is bit (7 - i % 8) of bits[i / 8]:
 *    the sync word's bit 31 first, then every FIFO byte from its bit 7.
 * struct gfsk_burst takes the same flag.
 *
 * Upstream facts reproduced here (firmware/bluetooth_rxtx, master of 2026-03):
 *  - ubertooth-btle -s<mac> -A<37|38|39>: the host sets the channel (2402, 2426, 2480
 *    MHz), the advertising data {02 01 05} and sends BTLE_SLAVE with the six address
 *    bytes as typed, "22:44:66:88:aa:cc" being mac[0] = 0x22 .. mac[5] = 0xcc. The
 *    firmware sends AdvA reversed (mac[5] first), so 22:44:66:88:aa:cc goes out as
 *    cc aa 88 66 44 22 and is displayed by a sniffer as 22:44:66:88:aa:cc.
 *  - The firmware sends one ADV_IND on the one channel it was given, every 100 ms, until the
 *    mode changes. It does not hop. PDU header byte 0 is 0x00 (ADV_IND, TxAdd 0 =
 *    public address, RxAdd 0), byte 1 is 6 + adv data length. CRCInit is 0x555555, the
 *    firmware keeps it bit reversed (0xaaaaaa) for btle_calc_crc(). Access address
 *    0x8e89bed6.
 *  - Upstream builds the packet in a 32 byte stack buffer, so adv data over 21 bytes
 *    overruns it there. A legacy ADV_IND carries up to 31 bytes of data (PDU length 37):
 *    that is the limit here, longer is refused (-EMSGSIZE).
 *  - TX_GENERIC_PACKET: GRMDM 0x0f61 = 32 preamble bytes 01010101 (first bit 0), 32 bit
 *    sync word SYNCH:SYNCL (bit 31 first), the data, no CRC; MDMCTRL 0x0057 =
 *    339.8 kHz deviation; Gaussian filter on. The CC2400 FIFO holds 32 bytes: a longer
 *    payload is cut at 32 (upstream writes it all; what the chip does with the excess is not
 *    documented, dropped is assumed).
 *    The request copies 7 + length bytes of the struct generic_tx_packet (a quirk: the
 *    last data byte of a full struct is lost), see ubt_tx_generic_load().
 *  - TX_TEST: GRMDM 0x0df1 with MDMTST0 0x334b (TX_PRNG): the modulator is fed by the
 *    chip's pseudo random generator, deviation 0x29 (BR, 160 kHz) or 0x40 (LE, 250 kHz).
 *    Here: a PRBS9 (x^9 + x^5 + 1) pattern of 400 bits made balanced, so that a loop
 *    bank of 400 symbols ends at the phase it began with.
 *  - TX frequency: FSDIV = channel, carrier at channel MHz (receive uses channel - 1).
 */

#ifndef UBT_TX_H_
#define UBT_TX_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gfsk_mod.h"
#include "upstream/ubertooth_interface.h"

/* Largest packet in bytes: 32 preamble, 4 sync and 32 data bytes of TX_GENERIC. */
#define UBT_TX_MAX_BYTES 72U

#define UBT_TX_ADV_AA           0x8e89bed6U
#define UBT_TX_ADV_CRC_INIT     0x555555U
/* CRCInit bit reversed, as btle_calc_crc() wants it (upstream le.crc_init_reversed). */
#define UBT_TX_ADV_CRC_INIT_REV 0xaaaaaaU
#define UBT_TX_ADV_DATA_MAX     31U
#define UBT_TX_ADV_PDU_MAX      39U

#define UBT_TX_GENERIC_PREAMBLE_BYTES 32U
#define UBT_TX_GENERIC_FIFO           32U
#define UBT_TX_GENERIC_DATA_MAX       50U
/* GRMDM 0x0f61 and MDMCTRL 0x0057 as tx_generic() writes them. */
#define UBT_TX_GENERIC_MDMCTRL        0x0057U

/* MDMCTRL of TX_TEST and of the BLE and BR modes. */
#define UBT_TX_MDMCTRL_BR  0x0029U
#define UBT_TX_MDMCTRL_BLE 0x0040U

#define UBT_TX_TEST_SYMBOLS 400U

/* One transmission as the modulator needs it. */
struct ubt_tx_pkt {
	uint8_t bits[UBT_TX_MAX_BYTES];
	uint16_t nbits;
	bool msb_first;
	/* Unmodulated carrier of nbits symbols: no bits (TX_TEST carrier). */
	bool carrier;
	/* Loop bank: the pattern repeats (TX_TEST). */
	bool loop;
	/* Carrier frequency in MHz (FSDIV). */
	uint16_t channel_mhz;
	/* Peak deviation in Hz (CC2400 MDMCTRL[6:0] steps of 3906.25 Hz). */
	int32_t dev_hz;
	/* tx_generic(): the request's pa_level (written to FREND). */
	uint8_t pa_level;
};

/* The ADV_IND PDU, not yet whitened: 2 header bytes, AdvA (the mac reversed), adv data. Returns its
 * length. */
int ubt_tx_adv_pdu(uint8_t *pdu, const uint8_t mac[6], const uint8_t *adv, unsigned int adv_len,
		   bool txadd);

/*
 * Preamble, access address, PDU and CRC for @p channel_mhz (whitened by its channel
 * index). @p crc_init_rev is CRCInit bit reversed. Returns the bits, or
 * a negative errno (-EINVAL, -EMSGSIZE for a PDU over UBT_TX_ADV_PDU_MAX).
 */
int ubt_tx_build_le(struct ubt_tx_pkt *p, uint32_t aa, uint32_t crc_init_rev, const uint8_t *pdu,
		    unsigned int pdu_len, uint16_t channel_mhz);

/*
 * The BTLE_SLAVE packet: ADV_IND from @p mac (as the host sent it) and the
 * LE_SET_ADV_DATA bytes, access address 0x8e89bed6, CRCInit @p crc_init_rev
 * (UBT_TX_ADV_CRC_INIT_REV unless a connection followed earlier changed it),
 * deviation 250 kHz. The glue sends it every 100 ms.
 */
int ubt_tx_build_adv(struct ubt_tx_pkt *p, const uint8_t mac[6], const uint8_t *adv,
		     unsigned int adv_len, uint16_t channel_mhz, uint32_t crc_init_rev);

/* The BLE advertising channel index (37, 38, 39) of 2402, 2426, 2480 MHz as upstream's whitening
 * picks it. */
unsigned int ubt_tx_adv_index(uint16_t channel_mhz);

/* TX_GENERIC_PACKET request (generic_tx_packet), fields as the firmware keeps them. */
struct ubt_tx_generic {
	uint16_t synch, syncl, channel;
	uint8_t length, pa_level;
	uint8_t data[UBT_TX_GENERIC_DATA_MAX];
};

/*
 * Parse the request payload like upstream's memcpy(&tx_pkt, data, 7 + data[6]) into the
 * struct generic_tx_packet layout (little endian u16 synch, syncl, channel at 0, 2, 4,
 * length at 6, pa_level at 7, data from 8): only the first 7 + length bytes of the
 * request are copied and the rest of @p g keeps its old contents, as upstream's
 * static tx_pkt does. @return Bytes copied.
 */
size_t ubt_tx_generic_load(struct ubt_tx_generic *g, const uint8_t *req, size_t req_len);

/* The CC2400 bit stream of tx_generic(): 32 preamble bytes, sync word, data. */
int ubt_tx_build_generic(struct ubt_tx_pkt *p, const struct ubt_tx_generic *g);

/* TX_TEST patterns. */
enum ubt_tx_test_kind {
	UBT_TX_TEST_PRBS,
	UBT_TX_TEST_CARRIER,
};

/*
 * TX_TEST for @p modulation (enum modulations: BR 160 kHz, LE 250 kHz; others -EINVAL).
 * PRBS: 400 bits of PRBS9 from @p seed (9 bits, 0 is taken as 0x1ff), balanced (200 ones),
 * air order LSB first, a loop. Carrier: 400 symbols of unmodulated carrier.
 */
int ubt_tx_build_test(struct ubt_tx_pkt *p, uint8_t modulation, uint16_t channel_mhz,
		      enum ubt_tx_test_kind kind, uint16_t seed);

/* Fill a modulator burst description (lead/trail 4 symbols, 120 sample ramps, centre at the end)
 * from a packet. */
void ubt_tx_burst(const struct ubt_tx_pkt *p, struct gfsk_burst *b);

/* MDMCTRL[6:0] deviation in Hz; MOD_OFFSET[12:7] carrier offset in Hz (15.625 kHz steps). */
static inline int32_t ubt_tx_mdmctrl_dev(uint16_t mdmctrl)
{
	return gfsk_dev_mdmctrl(mdmctrl);
}

static inline int32_t ubt_tx_mdmctrl_offset(uint16_t mdmctrl)
{
	return (int32_t)((mdmctrl >> 7) & 0x3fU) * 15625;
}

#endif /* UBT_TX_H_ */
