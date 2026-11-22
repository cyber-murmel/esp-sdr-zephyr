/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The transmit stream's app: the shell commands "tx ..." in tx_shell.c, the network side in
 * tx_net.c (control on UDP 4992, samples through a hook on the MAC's receive path from UDP 4994), the
 * stream itself in tx_stream.c, the network as in apps/rxbench.
 */

#include <zephyr/app_version.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/ethernet/eth_dwmac_fast.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_if.h>

#include "tx_boot.h"
#include "tx_net.h"
#include "tx_shell.h"
#include "tx_stream.h"

LOG_MODULE_REGISTER(txstream, LOG_LEVEL_INF);

int main(void)
{
	struct net_if *iface = net_if_get_first_by_type(&NET_L2_GET_NAME(ETHERNET));
	const struct device *eth;
	int ret;

	LOG_INF("txstream_eth %s", APP_VERSION_STRING);
	tx_mem_report();
	if (iface == NULL) {
		LOG_ERR("no Ethernet interface");
		tx_stream_ctl_run();
	}
	eth = net_if_get_device(iface);
	ret = tx_net_init(eth, iface);
	if (ret != 0) {
		LOG_ERR("network: %d", ret);
	}
	tx_shell_init(eth, iface);
	LOG_INF("\"tx boot\", then \"tx init\", then vrt_tx from the host (RF only after \"tx arm <minutes>\" with a subscriber)");
	/* this thread ends and starts the runs' chain and report (tx_stream.c) */
	tx_stream_ctl_run();
}
