/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Zigbee stack behind the ZNP: MAC, network layer, endpoint registry and the MT handlers
 * for APP_CNF, ZDO, AF and DEBUG. The app supplies a radio backend and the stack of the MAC
 * thread; everything else is here.
 */
#ifndef ZIGBEE_ZB_STACK_H_
#define ZIGBEE_ZB_STACK_H_

#include <zigbee/zb_net.h>
#include <zigbee/znp.h>

#define ZB_EP_MAX      16
#define ZB_EP_CLUSTERS 12
#define ZB_GROUP_MAX   8
#define ZB_GROUP_NAME  16

struct zb_ep {
	bool used;
	uint8_t ep;
	uint16_t profile;
	uint16_t device;
	uint8_t version;
	uint8_t nin, nout;
	uint16_t in[ZB_EP_CLUSTERS];
	uint16_t out[ZB_EP_CLUSTERS];
};

struct zb_group {
	bool used;
	uint8_t ep;
	uint16_t id;
	uint8_t namelen;
	char name[ZB_GROUP_NAME];
};

struct zb_stack {
	struct znp *znp;
	struct znp_nv *nv;
	struct zb_mac mac;
	struct zb_net net;
	struct zb_ep eps[ZB_EP_MAX];
	struct zb_group groups[ZB_GROUP_MAX];
	uint8_t default_eui[8];
	/* The host asked for ZDO messages as raw callbacks (zigpy-znp) rather than as AREQs. */
	bool msg_cb;
	uint8_t zdo_tsn;
	/* 0 off; 1 stack events; 2 also every frame (DEBUG_MSG AREQs to the host). */
	uint8_t trace_level;
};

/*
 * Set up the stack, register its MT handlers with @p z and boot the network layer from NV
 * (a formed network starts at once). Starts the MAC thread on @p mac_stack.
 */
int zb_stack_init(struct zb_stack *s, struct znp *z, struct znp_nv *nv,
		  const struct zb_radio_ops *radio, void *rctx, const uint8_t default_eui[8],
		  k_thread_stack_t *mac_stack, size_t mac_stack_size, int mac_prio);

/* SYS_RESET_REQ: restart the stack from NV (the transport stays up), then SYS_RESET_IND. */
void zb_stack_reset(struct zb_stack *s, uint8_t type);

/* Transmit power in dBm for SYS_SET_TX_POWER and SYS_STACK_TUNE; returns what is in force. */
int8_t zb_stack_set_txpower(struct zb_stack *s, int8_t dbm);

/* A diagnostic line to the host (MT DEBUG_MSG) when tracing is on. */
void zb_stack_trace(struct zb_stack *s, uint8_t level, const char *fmt, ...) __printf_like(3, 4);

/* MT handlers (registered by zb_stack_init). */
int znp_appcnf_handler(struct znp *z, const struct mt_frame *req, struct mt_frame *rsp);
int znp_zdo_handler(struct znp *z, const struct mt_frame *req, struct mt_frame *rsp);
int znp_af_handler(struct znp *z, const struct mt_frame *req, struct mt_frame *rsp);
int znp_debug_handler(struct znp *z, const struct mt_frame *req, struct mt_frame *rsp);

/* Net layer events to the host (set up by zb_stack_init). */
extern const struct zb_net_cb zb_stack_net_cb;

#endif /* ZIGBEE_ZB_STACK_H_ */
