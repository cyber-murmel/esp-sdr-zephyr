/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Gapless I/Q from the diag16 ring as VITA 49.2 over UDP on the CoreBoard's Ethernet. See
 * ../README.rst.
 */

#include <zephyr/app_version.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>

#include "stream_eth.h"

LOG_MODULE_REGISTER(stream_eth, LOG_LEVEL_INF);

void shell_spin_start(void);
void shell_app_ready(void);

/* The ring consumer: 4096 bytes as the main thread had for it (esp_sdr_init() and the ring), the
 * control thread 4096 too. Check both with "kernel stacks" on the board.
 */
static K_THREAD_STACK_DEFINE(radio_stack, 4096);
static struct k_thread radio_thread;
static K_THREAD_STACK_DEFINE(ctl_stack, 4096);
static struct k_thread ctl_thread;

static void ctl_entry(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	ctl_run();
}

static void radio_entry(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	if (radio_init() != 0) {
		return;
	}
	k_thread_create(&ctl_thread, ctl_stack, K_THREAD_STACK_SIZEOF(ctl_stack), ctl_entry, NULL, NULL,
			NULL, K_PRIO_PREEMPT(6), 0, K_NO_WAIT);
	k_thread_name_set(&ctl_thread, "vrt_ctl");
	/* the shell's commands wait for this: the radio and the control thread are there */
	shell_app_ready();
	radio_run();
}

static struct net_mgmt_event_callback addr_cb;

static void addr_event(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
	ARG_UNUSED(cb);
	if (event == NET_EVENT_IPV4_ADDR_ADD) {
		char s[NET_IPV4_ADDR_LEN];

		for (int k = 0; k < NET_IF_MAX_IPV4_ADDR; k++) {
			struct net_if_addr *a = &iface->config.ip.ipv4->unicast[k].ipv4;

			if (a->is_used && net_addr_ntop(NET_AF_INET, &a->address.in_addr, s, sizeof(s))) {
				LOG_INF("IPv4 address %s", s);
			}
		}
	} else if (event == NET_EVENT_IPV6_ADDR_ADD) {
		char s[NET_IPV6_ADDR_LEN];

		for (int k = 0; k < NET_IF_MAX_IPV6_ADDR; k++) {
			struct net_if_addr *a = &iface->config.ip.ipv6->unicast[k];

			if (a->is_used && net_addr_ntop(NET_AF_INET6, &a->address.in6_addr, s, sizeof(s))) {
				LOG_INF("IPv6 address %s", s);
			}
		}
	}
}

int main(void)
{
	struct net_if *iface = net_if_get_first_by_type(&NET_L2_GET_NAME(ETHERNET));

	LOG_INF("stream_eth %s", APP_VERSION_STRING);
	if (iface == NULL) {
		LOG_ERR("no Ethernet interface");
		return 0;
	}
	net_mgmt_init_event_callback(&addr_cb, addr_event,
				     NET_EVENT_IPV4_ADDR_ADD | NET_EVENT_IPV6_ADDR_ADD);
	net_mgmt_add_event_callback(&addr_cb);
	shell_spin_start();
	if (txpath_init(iface) != 0 || ctl_init(iface) != 0) {
		return 0;
	}
	/*
	 * The radio gets a thread of its own, created stopped, pinned, then started: a running thread
	 * (this one) cannot be pinned (k_thread_cpu_pin() refuses with -EINVAL), and an unpinned ring
	 * consumer may take the sender's CPU. It also takes the big stack the radio's start needs, so
	 * that this one can be small. The control thread starts once the radio is up.
	 */
	k_thread_create(&radio_thread, radio_stack, K_THREAD_STACK_SIZEOF(radio_stack), radio_entry,
			NULL, NULL, NULL, K_PRIO_PREEMPT(0), 0, K_FOREVER);
	k_thread_name_set(&radio_thread, "vrt_radio");
#if defined(CONFIG_SMP) && defined(CONFIG_SCHED_CPU_MASK)
	if (CONFIG_APP_VRT_RX_CPU >= 0 && arch_num_cpus() > 1) {
		int ret = k_thread_cpu_pin(&radio_thread, CONFIG_APP_VRT_RX_CPU);

		if (ret != 0) {
			LOG_ERR("pinning the ring consumer to CPU %d: %d", CONFIG_APP_VRT_RX_CPU, ret);
		}
	}
#endif
	k_thread_start(&radio_thread);
	return 0;
}
