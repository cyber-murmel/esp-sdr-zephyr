/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Wi-Fi interface. A net_if must be in a host section, so the host defines one, and the wifi
 * module's device takes it over as the module loads. The network stack reaches the driver through
 * the interface's device on every call, so nothing needs to know. In every other boot the interface
 * stays down, on a device that does nothing.
 */

#include <zephyr/device.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>

#include <multiapp/multiapp.h>

static void idle_iface_init(struct net_if *iface)
{
	net_if_flag_set(iface, NET_IF_NO_AUTO_START);
}

static const struct net_wifi_mgmt_offload idle_api = {
	.wifi_iface.iface_api.init = idle_iface_init,
};

NET_DEVICE_INIT(multiapp_wifi, "multiapp_wifi", NULL, NULL, NULL, NULL,
		CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &idle_api, ETHERNET_L2,
		NET_L2_GET_CTX_TYPE(ETHERNET_L2), NET_ETH_MTU);

/* The module's name for it (ext/wifi_prelude.h). */
struct net_if *const multiapp_wifi_iface = NET_IF_GET(multiapp_wifi, 0);

int multiapp_wifi_bind(const struct device *dev)
{
	struct net_if *iface = multiapp_wifi_iface;
	const struct net_if_api *api;

	if (!device_is_ready(dev)) {
		return -ENODEV;
	}
	api = dev->api;
	iface->if_dev->dev = dev;
	api->init(iface);
	net_if_flag_clear(iface, NET_IF_NO_AUTO_START);
	return net_if_up(iface);
}
