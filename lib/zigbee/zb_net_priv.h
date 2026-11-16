/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What zb_net.c shares with zb_nwk.c.
 */
#ifndef ZB_NET_PRIV_H_
#define ZB_NET_PRIV_H_

#include <zigbee/zb_net.h>

struct znp_nv_key zbn_exkey(uint8_t item, uint16_t sub);
struct zb_child *zbn_child_by_ext(struct zb_net *n, const uint8_t ext[8]);
struct zb_child *zbn_child_by_nwk(struct zb_net *n, uint16_t nwk);
void zbn_child_persist(struct zb_net *n, int idx);

/* zb_nwk.c */
void zbn_trace(struct zb_net *n, const char *fmt, ...) __printf_like(2, 3);
void zbn_security_load(struct zb_net *n);
void zbn_rx(struct zb_net *n, const struct zb_mac_hdr *mh, const uint8_t *payload, size_t len,
	    uint8_t lqi);
void zbn_tick(struct zb_net *n);
/* The response to an association was delivered: give the joiner the network key. */
void zbn_tc_join(struct zb_net *n, int idx);

#endif /* ZB_NET_PRIV_H_ */
