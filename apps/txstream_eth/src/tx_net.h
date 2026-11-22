/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The network side of the transmit stream (doc/dev/txs.md, "Network protocol"): the receive hook
 * that takes the sample packets of UDP port 4994, the control port 4992 on the stack (the cookie
 * protocol of apps/stream_eth plus TXGO, TXEN and TXPG), and the thread that sends TXST and TXPO.
 */

#ifndef TX_NET_H_
#define TX_NET_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/net/net_if.h>
#include <zephyr/shell/shell.h>

/** UDP port of the control messages and the source port of the status. */
#define TX_CTL_PORT 4992U

/** Open the control port, start the control and status threads, take the receive hook. */
int tx_net_init(const struct device *eth, struct net_if *iface);

/** `tx arm <minutes>`: the subscriber there is now (-ENOENT without one) may ask for RF for that long; prints
 * its address, port and MAC. `tx disarm`. */
int tx_net_arm(const struct shell *sh, uint32_t minutes);
void tx_net_disarm(void);
/** Seconds of the arming left, 0: not armed. */
uint32_t tx_net_armed_s(void);

/** `tx net`. */
void tx_net_print(const struct shell *sh);

#endif
