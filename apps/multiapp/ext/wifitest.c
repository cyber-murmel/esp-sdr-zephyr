/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A small app on the wifi module, through the host's net_mgmt as any Wi-Fi app: wifi scan, wifi
 * connect, wifi status, wifi disconnect. "net ping" in the host's shell then reaches the network.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/shell/shell.h>

#define SCAN_MAX  32
#define SCAN_WAIT K_SECONDS(20)
/* Joining, and DHCP's initial random delay of up to 10 s. */
#define JOIN_WAIT K_SECONDS(40)

static struct net_mgmt_event_callback wifi_cb;
static struct wifi_scan_result results[SCAN_MAX];
static volatile int result_count;
static volatile int result_dropped;
static volatile int connect_status;
static K_SEM_DEFINE(scan_done, 0, 1);
static K_SEM_DEFINE(connected, 0, 1);

static void wifi_event(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
	ARG_UNUSED(iface);

	if (event == NET_EVENT_WIFI_SCAN_RESULT) {
		if (result_count < SCAN_MAX) {
			memcpy(&results[result_count], cb->info, sizeof(results[0]));
			result_count++;
		} else {
			result_dropped++;
		}
	} else if (event == NET_EVENT_WIFI_SCAN_DONE) {
		k_sem_give(&scan_done);
	} else if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
		connect_status = ((const struct wifi_status *)cb->info)->status;
		k_sem_give(&connected);
	}
}

static struct net_if *wifi_iface(const struct shell *sh)
{
	struct net_if *iface = net_if_get_first_wifi();

	if (iface == NULL) {
		shell_error(sh, "no Wi-Fi interface");
	}
	return iface;
}

static int cmd_scan(const struct shell *sh, size_t argc, char **argv)
{
	struct net_if *iface = wifi_iface(sh);
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (iface == NULL) {
		return -ENODEV;
	}
	result_count = 0;
	result_dropped = 0;
	k_sem_reset(&scan_done);
	ret = net_mgmt(NET_REQUEST_WIFI_SCAN, iface, NULL, 0);
	if (ret != 0) {
		shell_error(sh, "scan failed (%d)", ret);
		return ret;
	}
	if (k_sem_take(&scan_done, SCAN_WAIT) != 0) {
		shell_error(sh, "no end of scan");
		return -ETIMEDOUT;
	}
	shell_print(sh, "%d access points%s", result_count, result_dropped != 0 ? ", more dropped" : "");
	for (int i = 0; i < result_count; i++) {
		const struct wifi_scan_result *r = &results[i];

		shell_print(sh, "%2d: chan %3u rssi %4d %-16s %.*s", i, r->channel, r->rssi,
			    wifi_security_txt(r->security), r->ssid_length, r->ssid);
	}
	return 0;
}

/* wifi connect <ssid> [psk] */
static int cmd_connect(const struct shell *sh, size_t argc, char **argv)
{
	struct net_if *iface = wifi_iface(sh);
	struct wifi_connect_req_params p = {
		.ssid = (const uint8_t *)argv[1],
		.ssid_length = strlen(argv[1]),
		.channel = WIFI_CHANNEL_ANY,
		.band = WIFI_FREQ_BAND_2_4_GHZ,
		.security = argc > 2 ? WIFI_SECURITY_TYPE_PSK : WIFI_SECURITY_TYPE_NONE,
		.mfp = WIFI_MFP_OPTIONAL,
		.timeout = SYS_FOREVER_MS,
	};
	int ret;

	if (iface == NULL) {
		return -ENODEV;
	}
	if (argc > 2) {
		p.psk = (const uint8_t *)argv[2];
		p.psk_length = strlen(argv[2]);
	}
	k_sem_reset(&connected);
	ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &p, sizeof(p));
	if (ret != 0) {
		shell_error(sh, "connect failed (%d)", ret);
		return ret;
	}
	if (k_sem_take(&connected, JOIN_WAIT) != 0) {
		shell_error(sh, "no result from the access point");
		return -ETIMEDOUT;
	}
	if (connect_status != 0) {
		shell_error(sh, "not connected (status %d)", connect_status);
		return -EIO;
	}
	/* With CONFIG_WIFI_STA_AUTO_DHCPV4 the result comes once DHCP has an address. */
	shell_print(sh, "connected");
	return 0;
}

static int cmd_disconnect(const struct shell *sh, size_t argc, char **argv)
{
	struct net_if *iface = wifi_iface(sh);

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (iface == NULL) {
		return -ENODEV;
	}
	return net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	struct net_if *iface = wifi_iface(sh);
	struct wifi_iface_status st = {0};
	const struct in_addr *addr;
	char text[NET_IPV4_ADDR_LEN] = "none";
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (iface == NULL) {
		return -ENODEV;
	}
	ret = net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &st, sizeof(st));
	if (ret != 0) {
		shell_error(sh, "status failed (%d)", ret);
		return ret;
	}
	addr = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
	if (addr != NULL) {
		(void)net_addr_ntop(AF_INET, addr, text, sizeof(text));
	}
	shell_print(sh, "state %s, ssid %.*s, chan %d, rssi %d, address %s, interface %s",
		    wifi_state_txt(st.state), st.ssid_len, st.ssid, st.channel, st.rssi, text,
		    net_if_is_up(iface) ? "up" : "down");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(wifi_cmds,
	SHELL_CMD(scan, NULL, "List access points", cmd_scan),
	SHELL_CMD_ARG(connect, NULL, "Join <ssid> [psk] and get an address", cmd_connect, 2, 1),
	SHELL_CMD(status, NULL, "State and address", cmd_status),
	SHELL_CMD(disconnect, NULL, "Leave", cmd_disconnect),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(wifi, &wifi_cmds, "The Wi-Fi module", NULL);

int main(void)
{
	net_mgmt_init_event_callback(&wifi_cb, wifi_event,
				     NET_EVENT_WIFI_SCAN_RESULT | NET_EVENT_WIFI_SCAN_DONE |
				     NET_EVENT_WIFI_CONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_cb);
	return 0;
}
