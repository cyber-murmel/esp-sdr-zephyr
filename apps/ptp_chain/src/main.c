/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * PTP chain: the offset between two DWMAC clocks from a ping-pong of raw frames. See ../README.rst.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_if.h>

#include "chain.h"

LOG_MODULE_REGISTER(ptp_chain, LOG_LEVEL_INF);

int main(void)
{
	struct net_if *iface = net_if_get_first_by_type(&NET_L2_GET_NAME(ETHERNET));
	int ret;

	if (iface == NULL) {
		LOG_ERR("no Ethernet interface");
		return 0;
	}
	if (!net_if_is_up(iface)) {
		net_if_up(iface);
	}
	ret = chain_init(net_if_get_device(iface), iface);
	if (ret != 0) {
		LOG_ERR("rx hook: %d", ret);
		return 0;
	}
	chain_stats_init();
	if (IS_ENABLED(CONFIG_APP_CHAIN_PPS)) {
		ret = chain_pps_init();
		if (ret != 0) {
			LOG_ERR("pps: %d", ret);
		}
	}
	LOG_INF("ptp_chain ready: 'chain peer <mac>' and 'chain start' on one board");
	return 0;
}
