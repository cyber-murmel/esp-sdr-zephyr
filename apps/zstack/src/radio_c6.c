/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The ESP32-C6 native 802.15.4 radio as a zb_radio: Zephyr's ieee802154_radio_api in raw mode
 * plus the few HAL calls the driver does not expose. Details and gotchas:
 * tmp/zstack/phase2/radio.md (changes reach the hardware at the next start/tx/ed; the driver
 * has no CSMA backoff or retries; promiscuous mode stops the hardware ACKs).
 */
#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/net/ieee802154_radio.h>
#include <zephyr/net/net_pkt.h>

#include <esp_ieee802154.h>

#include "zstack.h"

#define FCS_LEN 2U
/* Slack on top of the scan time before an energy detect is given up. */
#define ED_SLACK_MS 500

static const struct device *const radio = DEVICE_DT_GET(DT_CHOSEN(zephyr_ieee802154));
static const struct ieee802154_radio_api *api;
static struct zb_mac *rx_mac;

static K_SEM_DEFINE(ed_done, 0, 1);
static int16_t ed_result;

NET_BUF_POOL_DEFINE(tx_pool, 1, ZB_MAC_MAX_FRAME, 0, NULL);

/*
 * Raw mode hands every frame here, on the driver's own thread (1 KiB stack): copy it out and
 * return. The last two bytes of the packet are RSSI and LQI where the FCS was.
 */
int net_recv_data(struct net_if *iface, struct net_pkt *pkt)
{
	static uint8_t buf[ZB_MAC_MAX_FRAME + FCS_LEN];
	size_t len = net_pkt_get_len(pkt);
	uint8_t lqi = net_pkt_ieee802154_lqi(pkt);
	int8_t rssi = (int8_t)net_pkt_ieee802154_rssi_dbm(pkt);

	ARG_UNUSED(iface);
	net_pkt_cursor_init(pkt);
	if (len > FCS_LEN && len <= sizeof(buf) && net_pkt_read(pkt, buf, len) == 0 &&
	    rx_mac != NULL) {
		zb_mac_rx(rx_mac, buf, (uint8_t)(len - FCS_LEN), lqi, rssi);
	}
	net_pkt_unref(pkt);
	return 0;
}

enum net_verdict ieee802154_handle_ack(struct net_if *iface, struct net_pkt *pkt)
{
	ARG_UNUSED(iface);
	ARG_UNUSED(pkt);
	return NET_OK;
}

static int c6_set_channel(void *ctx, uint8_t ch)
{
	ARG_UNUSED(ctx);
	return api->set_channel(radio, ch);
}

static int c6_set_ids(void *ctx, uint16_t pan, uint16_t short_addr, const uint8_t ext[8])
{
	struct ieee802154_filter f;
	uint8_t ieee[8];
	int r;

	ARG_UNUSED(ctx);
	memcpy(ieee, ext, sizeof(ieee));
	f.pan_id = pan;
	r = api->filter(radio, true, IEEE802154_FILTER_TYPE_PAN_ID, &f);
	f.short_addr = short_addr;
	r |= api->filter(radio, true, IEEE802154_FILTER_TYPE_SHORT_ADDR, &f);
	f.ieee_addr = ieee;
	r |= api->filter(radio, true, IEEE802154_FILTER_TYPE_IEEE_ADDR, &f);
	return r != 0 ? -EIO : 0;
}

static int c6_set_mode(void *ctx, bool coordinator, bool promiscuous)
{
	struct ieee802154_config cfg = {.promiscuous = promiscuous};
	int r;

	ARG_UNUSED(ctx);
	r = api->configure(radio, IEEE802154_CONFIG_PROMISCUOUS, &cfg);
	r |= esp_ieee802154_set_coordinator(coordinator);
	return r != 0 ? -EIO : 0;
}

static int c6_set_txpower(void *ctx, int8_t dbm)
{
	ARG_UNUSED(ctx);
	return api->set_txpower(radio, dbm);
}

static int c6_start(void *ctx)
{
	ARG_UNUSED(ctx);
	return api->start(radio);
}

static int c6_tx(void *ctx, const uint8_t *f, size_t n, bool csma)
{
	struct net_pkt *pkt;
	struct net_buf *buf;
	int r;

	ARG_UNUSED(ctx);
	/* Drivers take the frame as one buffer; the packet is only a handle for this one. */
	buf = net_buf_alloc(&tx_pool, K_MSEC(100));
	if (buf == NULL) {
		return -ENOMEM;
	}
	net_buf_add_mem(buf, f, n);
	pkt = net_pkt_alloc(K_MSEC(100));
	if (pkt == NULL) {
		net_buf_unref(buf);
		return -ENOMEM;
	}
	r = api->tx(radio, csma ? IEEE802154_TX_MODE_CSMA_CA : IEEE802154_TX_MODE_DIRECT, pkt, buf);
	net_pkt_unref(pkt);
	net_buf_unref(buf);
	return r;
}

/* Runs on the system workqueue; in raw mode @p dev is NULL. */
static void ed_cb(const struct device *dev, int16_t max_ed)
{
	ARG_UNUSED(dev);
	ed_result = max_ed;
	k_sem_give(&ed_done);
}

static int c6_ed(void *ctx, uint16_t ms, int8_t *dbm)
{
	int r;

	ARG_UNUSED(ctx);
	k_sem_reset(&ed_done);
	r = api->ed_scan(radio, ms, ed_cb);
	if (r != 0) {
		return r;
	}
	if (k_sem_take(&ed_done, K_MSEC(ms + ED_SLACK_MS)) != 0) {
		return -ETIMEDOUT;
	}
	*dbm = (int8_t)ed_result;
	return 0;
}

static const struct zb_radio_ops c6_ops = {
	.set_channel = c6_set_channel,
	.set_ids = c6_set_ids,
	.set_mode = c6_set_mode,
	.set_txpower = c6_set_txpower,
	.start = c6_start,
	.tx = c6_tx,
	.ed = c6_ed,
};

const struct zb_radio_ops *radio_c6_init(struct zb_mac *mac)
{
	if (!device_is_ready(radio)) {
		return NULL;
	}
	api = radio->api;
	rx_mac = mac;
	return &c6_ops;
}
