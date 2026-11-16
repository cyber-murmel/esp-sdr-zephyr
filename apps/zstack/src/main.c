/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Z-Stack coordinator: a Zigbee network processor behind the Z-Stack 3.x ZNP serial protocol.
 */
#include <string.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zigbee/zb_stack.h>

#include "zstack.h"

LOG_MODULE_REGISTER(zstack, LOG_LEVEL_INF);

static struct znp_nv nv;
static struct znp znp;
static struct zb_stack stack;

K_THREAD_STACK_DEFINE(mac_stack, 4096);

/* EUI-64 from the factory MAC (FFFE in the middle), little endian as ZNP carries it. */
static void ext_addr(uint8_t out[8])
{
	uint8_t mac[8] = {0};
	ssize_t n = hwinfo_get_device_id(mac, sizeof(mac));
	uint8_t eui[8];

	if (n >= 6) {
		eui[0] = mac[0];
		eui[1] = mac[1];
		eui[2] = mac[2];
		eui[3] = 0xFF;
		eui[4] = 0xFE;
		eui[5] = mac[3];
		eui[6] = mac[4];
		eui[7] = mac[5];
	} else {
		memcpy(eui, mac, 8);
	}
	for (int i = 0; i < 8; i++) {
		out[i] = eui[7 - i];
	}
}

int main(void)
{
	struct znp_config cfg = {
		.tx = transport_tx,
		.nv = &nv,
	};
	const struct zb_radio_ops *radio;
	uint8_t eui[8];

	znp_nv_init(&nv, NULL);
	if (nv_store_init(&nv) != 0) {
		LOG_ERR("NV store unavailable, items will not persist");
	}
	ext_addr(eui);
	memcpy(cfg.ext_addr, eui, sizeof(eui));
	radio = radio_c6_init(&stack.mac);
	if (radio == NULL) {
		LOG_ERR("802.15.4 radio not ready");
		return -1;
	}
	znp_init(&znp, &cfg);
	zb_stack_init(&stack, &znp, &nv, radio, NULL, eui, mac_stack,
		      K_THREAD_STACK_SIZEOF(mac_stack), K_PRIO_PREEMPT(4));
	transport_start(&znp);
	znp_send_reset_ind(&znp, ZNP_RESET_POWER_UP);
	LOG_INF("ZNP up, IEEE %02x%02x%02x%02x%02x%02x%02x%02x, %s", eui[7], eui[6], eui[5], eui[4],
		eui[3], eui[2], eui[1], eui[0], stack.net.formed ? "network restored" : "no network");

	for (;;) {
		k_sleep(K_SECONDS(1));
		if (znp_nv_dirty(&nv)) {
			znp_nv_flush(&nv);
		}
	}
	return 0;
}
