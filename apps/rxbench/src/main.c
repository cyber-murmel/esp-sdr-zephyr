/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * UDP to port 4993 straight from the MAC's DMA ring, measured by a consumer thread. See
 * ../README.rst.
 */

#include <zephyr/app_version.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_if.h>

#include "rxb.h"
#include "rxb_proto.h"

LOG_MODULE_REGISTER(rxbench, LOG_LEVEL_INF);

void rxb_shell_init(const struct device *eth, struct net_if *iface);

int main(void)
{
	struct net_if *iface = net_if_get_first_by_type(&NET_L2_GET_NAME(ETHERNET));
	const struct device *eth;

	LOG_INF("rxbench %s", APP_VERSION_STRING);
	if (iface == NULL) {
		LOG_ERR("no Ethernet interface");
		return 0;
	}
	eth = net_if_get_device(iface);
	if (rxb_init(eth, iface) != 0) {
		return 0;
	}
	rxb_shell_init(eth, iface);
	LOG_INF("\"rxb start\" takes UDP to port %u", RXB_PORT);
	return 0;
}
