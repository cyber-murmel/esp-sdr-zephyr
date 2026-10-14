/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ubertooth One device protocol as the unmodified host tools expect it: the
 * vendor request numbers, packet layouts and enums are upstream's interface
 * header (upstream/ubertooth_interface.h, unchanged); this adds the device
 * modes and limits of the upstream firmware that the interface header omits.
 */

#ifndef UBT_PROTO_H_
#define UBT_PROTO_H_

#include <stdint.h>

#include "upstream/ubertooth_interface.h"

_Static_assert(sizeof(usb_pkt_rx) == 64, "usb_pkt_rx is one full-speed bulk packet");

/* Device operating modes (upstream firmware/common/ubertooth.h). */
enum ubt_mode_id {
	MODE_IDLE = 0,
	MODE_RX_SYMBOLS = 1,
	MODE_TX_SYMBOLS = 2,
	MODE_TX_TEST = 3,
	MODE_SPECAN = 4,
	MODE_RANGE_TEST = 5,
	MODE_REPEATER = 6,
	MODE_LED_SPECAN = 7,
	MODE_BT_FOLLOW = 8,
	MODE_BT_FOLLOW_LE = 9,
	MODE_BT_PROMISC_LE = 10,
	MODE_RESET = 11,
	MODE_BT_SLAVE_LE = 12,
	MODE_EGO = 13,
	MODE_AFH = 14,
	MODE_RX_GENERIC = 15,
	MODE_TX_GENERIC = 16,
	MODE_XMAS = 17,
};

/* SET_CHANNEL (MHz) limits of the CC2400 firmware; above MAX_FREQ it means "sweep" (host sends 9999). */
#define UBT_MIN_FREQ 2268U
#define UBT_MAX_FREQ 2794U
/* SPECAN accepts requests inside this window (wValue, wIndex). */
#define UBT_SPECAN_MIN 2049U
#define UBT_SPECAN_MAX 3072U

/* Bluetooth clock: CLKN ticks at 3.2 kHz, CLK100NS counts 100 ns units inside 2^20 ticks. */
#define UBT_CLK100NS_PER_CLKN 3125U
#define UBT_CLK100NS_WRAP     3276800000ULL

/* KEEP_ALIVE after this many CLKN ticks without a packet (about 125 s). */
#define UBT_KEEP_ALIVE_TICKS 400000U

/* GET_BOARD_ID: 1 = Ubertooth One (host/libubertooth/src/ubertooth.h board_ids). */
#define UBT_BOARD_ID 1U

/* CC2400 RSSI register byte: dBm + 54 (squelch thresholds and RSSI bytes in the packets). */
#define UBT_RSSI_OFFSET 54

#endif /* UBT_PROTO_H_ */
