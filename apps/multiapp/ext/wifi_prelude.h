/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * In front of every source of the wifi module, after include/multiapp/ext.h: what the driver
 * registers in the host's sections. Its interface is the host's (src/wifi_netif.c), so its net
 * device becomes a plain device of the module.
 */

#ifndef MULTIAPP_WIFI_PRELUDE_H_
#define MULTIAPP_WIFI_PRELUDE_H_

#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>

#undef NET_DEVICE_DT_INST_DEFINE
#define NET_DEVICE_DT_INST_DEFINE(inst, init_fn, pm, data, config, prio, api, l2, l2_ctx_type,    \
				  mtu)                                                             \
	DEVICE_DT_INST_DEFINE(inst, init_fn, pm, data, config, POST_KERNEL, prio, api)

/*
 * The driver names its interface through these. Left alone, they name a zeroed net_if of its own:
 * it links and scans, and the first use of the interface crashes.
 */
extern struct net_if *const multiapp_wifi_iface;
#undef NET_IF_DT_INST_DECLARE
#define NET_IF_DT_INST_DECLARE(inst, sfx)
#undef NET_IF_DT_INST_GET
#define NET_IF_DT_INST_GET(inst, sfx) multiapp_wifi_iface
#undef NET_IF_DECLARE
#define NET_IF_DECLARE(dev_id, sfx) BUILD_ASSERT(0, "the Wi-Fi interface is the host's")

/* The host's list of static handlers cannot take one: a callback, with the same effect. */
#undef NET_MGMT_REGISTER_EVENT_HANDLER
#define NET_MGMT_REGISTER_EVENT_HANDLER(_name, _event_mask, _func, _user_data)                    \
	static struct net_mgmt_event_callback UTIL_CAT(_name, _cb);                                \
	static void UTIL_CAT(_name, _call)(struct net_mgmt_event_callback *cb, uint64_t event,     \
					   struct net_if *iface)                                    \
	{                                                                                          \
		_func(event, iface, (void *)cb->info, cb->info_length, _user_data);                \
	}                                                                                          \
	static void __attribute__((constructor)) UTIL_CAT(multiapp_init_, _name)(void)             \
	{                                                                                          \
		net_mgmt_init_event_callback(&UTIL_CAT(_name, _cb), UTIL_CAT(_name, _call),        \
					     _event_mask);                                          \
		net_mgmt_add_event_callback(&UTIL_CAT(_name, _cb));                                \
	}                                                                                          \
	static void __attribute__((destructor)) UTIL_CAT(multiapp_exit_, _name)(void)             \
	{                                                                                          \
		net_mgmt_del_event_callback(&UTIL_CAT(_name, _cb));                                \
	}

#endif /* MULTIAPP_WIFI_PRELUDE_H_ */
