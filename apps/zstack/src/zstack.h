/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZSTACK_H_
#define ZSTACK_H_

#include <zigbee/zb_mac.h>
#include <zigbee/zb_radio.h>
#include <zigbee/znp.h>

/* Host link (USB-Serial-JTAG). transport_tx() is the ZNP's tx callback. */
void transport_start(struct znp *z);
void transport_tx(void *user, const uint8_t *buf, size_t len);

/* The ESP32-C6 radio for the MAC; NULL if the device is missing. */
const struct zb_radio_ops *radio_c6_init(struct zb_mac *mac);

/* Flash backend of the NV store: mounts, loads stored items, then flush() persists. */
int nv_store_init(struct znp_nv *nv);

#endif /* ZSTACK_H_ */
