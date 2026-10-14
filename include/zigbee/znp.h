/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ZNP server: parses MT frames from a byte stream, dispatches SREQs to per-subsystem
 * handlers and sends their SRSPs, and carries the stack's AREQ callbacks to the host.
 * Speaks the Z-Stack 3.x dialect (transport revision 2) that zigpy-znp and zigbee2mqtt
 * detect as a CC2652 coordinator.
 */
#ifndef ZIGBEE_ZNP_H_
#define ZIGBEE_ZNP_H_

#include <zephyr/kernel.h>

#include <zigbee/mt.h>
#include <zigbee/znp_nv.h>

/* What SYS_VERSION and SYS_RESET_IND report (Z-Stack 3.30 on a coordinator). */
#define ZNP_TRANSPORT_REV 2
#define ZNP_PRODUCT_ID    1
#define ZNP_VERSION_MAJOR 2
#define ZNP_VERSION_MINOR 7
#define ZNP_VERSION_MAINT 1

enum znp_reset_reason {
	ZNP_RESET_POWER_UP = 0,
	ZNP_RESET_EXTERNAL = 1,
	ZNP_RESET_WATCHDOG = 2,
};

struct znp;

/*
 * A subsystem handler. @p rsp is pre-set with type SRSP and the request's subsystem and
 * command for an SREQ and has len 0; fill data and len. Return 0, or an enum mt_rpc_error
 * to answer with an RPC error instead. For an AREQ @p rsp is NULL.
 */
typedef int (*znp_handler_t)(struct znp *z, const struct mt_frame *req, struct mt_frame *rsp);

struct znp_config {
	/* Called with one whole wire frame; serialised by the ZNP's tx lock. */
	void (*tx)(void *user, const uint8_t *buf, size_t len);
	/* SYS_RESET_REQ: type 0 hard, 1 soft; the stack restarts and the app then calls
	 * znp_send_reset_ind(). NULL: the ZNP only sends the indication.
	 */
	void (*reset)(void *user, uint8_t type);
	/* SYS_SET_TX_POWER in dBm; returns the applied power. NULL: accepted, ignored. */
	int8_t (*set_tx_power)(void *user, int8_t dbm);
	void *user;
	struct znp_nv *nv;
	/* The device's IEEE address (EUI-64, little endian on the wire as stored). */
	uint8_t ext_addr[8];
};

#define ZNP_DEFER_MAX  4
#define ZNP_DEFER_DATA 128

struct znp_deferred {
	uint8_t sub, cmd, len;
	uint8_t data[ZNP_DEFER_DATA];
};

struct znp {
	struct znp_config cfg;
	/* The Zigbee stack behind this ZNP (struct zb_stack), NULL for the bare protocol. */
	void *stack;
	k_tid_t dispatching;
	uint8_t ndeferred;
	struct znp_deferred deferred[ZNP_DEFER_MAX];
	struct mt_parser parser;
	struct mt_frame rsp;
	/* SYS_PING capabilities: one bit per registered subsystem (MT_SYS bit 0 ... MT_UTIL
	 * bit 6, MT_ZGP bit 9, MT_APP_CNF bit 10). Hosts take a missing APP_CNF for Z-Stack 1.2.
	 */
	uint16_t caps;
	struct k_mutex tx_lock;
	znp_handler_t handler[MT_SUBSYS_COUNT];
	int8_t tx_power;
};

/* Sets up SYS and UTIL; other subsystems come in through znp_register(). */
void znp_init(struct znp *z, const struct znp_config *cfg);

void znp_register(struct znp *z, enum mt_subsys sub, znp_handler_t handler);

/* Feed bytes from the transport; complete frames are handled in this call. */
void znp_rx(struct znp *z, const uint8_t *buf, size_t n);

/* The byte stream paused mid-frame: forget the partial frame (the transport's idle timer). */
void znp_rx_idle(struct znp *z);

/* Send an AREQ (stack callback) to the host; any thread. */
void znp_send_areq(struct znp *z, enum mt_subsys sub, uint8_t cmd, const void *data, uint8_t len);

/*
 * Send an AREQ that belongs after the SRSP of the request being handled (a callback that the
 * request caused). From a handler it is held back until the SRSP is out; from any other thread
 * it goes at once.
 */
void znp_send_areq_after(struct znp *z, enum mt_subsys sub, uint8_t cmd, const void *data,
			 uint8_t len);

/* SYS_RESET_IND: sent at boot and after a reset. */
void znp_send_reset_ind(struct znp *z, enum znp_reset_reason reason);

#endif /* ZIGBEE_ZNP_H_ */
