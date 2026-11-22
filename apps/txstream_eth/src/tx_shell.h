/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TX_SHELL_H_
#define TX_SHELL_H_

#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/net/net_if.h>

#include "tx_stream.h"

void tx_shell_init(const struct device *eth, struct net_if *iface);
/** `tx init` succeeded and the library is ready (a run, RF or not, needs it). */
bool tx_shell_inited(void);
/** The kernel and the flip tuning the shell holds (`tx kern`, `tx cfg`), into a run's configuration. */
void tx_shell_cfg(struct tx_cfg *c);
/** The report at the end of a run, on the UART shell (from the stream's control thread). */
void tx_shell_done(const struct tx_stats *s);
/** Frames the receive hook was offered since boot (tx_net.c). */
uint32_t tx_hook_offered(void);

#endif
