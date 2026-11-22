/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Buffers of the receive pool the stream holds, one configuration for the board (the credit depth, the
 * hook's cap) and the host (the burst): plain C, no Zephyr.
 *
 * The board holds up to TX_HOLD buffers, in the ring and the thread's list together. What is in them at
 * the worst moment: the packets the credit lets the host send beyond the open window, which wait for the
 * next flip (TX_DEPTH_PKTS, the credit depth in packets), a burst the host puts on the wire back to back
 * (TX_BURST, which the board takes before it has placed the first of them), and two: one packet split by
 * the window's end and one being placed. So TX_DEPTH_PKTS = TX_HOLD - TX_BURST - 2, and the credit depth
 * the board reports (TXAK) is that many packets of the format. The pool has CONFIG_NET_BUF_RX_COUNT
 * buffers, the MAC takes CONFIG_DWMAC_NB_RX_DESCS of them, two stay for the stack (tx_stream.c checks).
 * More buffers (a bigger pool) make TX_HOLD larger and with it the depth. The pool is 43 buffers of 1664 B in TCM
 * group 3 (prj.conf; the stacks of the threads with slack are in PSRAM), so TX_HOLD = 43 - 16 - 2 = 25, a depth of
 * 19 packets (13338 words iq8, 6669 w32) where 16 buffers in a pool of 34 gave 10 (7020).
 */

#ifndef TX_HOLD_H_
#define TX_HOLD_H_

/* buffers the stream holds at most (tx_stream_put() refuses one more) */
#define TX_HOLD 25U
/* packets the host sends back to back at most (vrt_tx: TXP_BURST) */
#define TX_BURST 4U
/* packets of credit depth: the packets beyond the open window, as the TXAK's depth says in words */
#define TX_DEPTH_PKTS (TX_HOLD - TX_BURST - 2U)
/* the credit depth's buffers: depth = (TX_BUFFERS - 2) packets (txs_credit.h) */
#define TX_BUFFERS (TX_DEPTH_PKTS + 2U)

_Static_assert(TX_DEPTH_PKTS >= 2U, "the hold must leave room for a depth");
_Static_assert(TX_DEPTH_PKTS + TX_BURST + 2U <= TX_HOLD, "depth, burst and two must fit the hold");
/* the pool: the MAC's 16 descriptors, the hold and two for the stack are the 43 receive buffers of prj.conf
 * (tx_stream.c checks the pool against its config, this one the numbers the host's tools share) */
_Static_assert(TX_HOLD + 16U + 2U == 43U, "TX_HOLD, the MAC's descriptors and the stack's two are the pool of prj.conf");

#endif
