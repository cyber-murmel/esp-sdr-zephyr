/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * MT SYS and UTIL commands: liveness, version, identity and the NV item store.
 */
#include <errno.h>
#include <string.h>

#include <zigbee/zb_stack.h>
#include <zigbee/znp.h>

#include "znp_priv.h"

enum {
	SYS_RESET_REQ = 0x00,
	SYS_PING = 0x01,
	SYS_VERSION = 0x02,
	SYS_SET_EXT_ADDR = 0x03,
	SYS_GET_EXT_ADDR = 0x04,
	SYS_OSAL_NV_ITEM_INIT = 0x07,
	SYS_OSAL_NV_READ = 0x08,
	SYS_OSAL_NV_WRITE = 0x09,
	SYS_OSAL_NV_DELETE = 0x12,
	SYS_OSAL_NV_LENGTH = 0x13,
	SYS_STACK_TUNE = 0x0F,
	SYS_SET_TX_POWER = 0x14,
	SYS_OSAL_NV_READ_EXT = 0x1C,
	SYS_OSAL_NV_WRITE_EXT = 0x1D,
	SYS_NV_CREATE = 0x30,
	SYS_NV_DELETE = 0x31,
	SYS_NV_LENGTH = 0x32,
	SYS_NV_READ = 0x33,
	SYS_NV_WRITE = 0x34,
};

enum {
	UTIL_GET_DEVICE_INFO = 0x00,
	UTIL_LED_CONTROL = 0x0A,
	UTIL_ASSOC_FIND_DEVICE = 0x49,
	UTIL_ASSOC_GET_WITH_ADDRESS = 0x4A,
	UTIL_ASSOC_REMOVE = 0x63,
	UTIL_ASSOC_ADD = 0x64,
};

/* associated_devices_t node relations. */
#define REL_CHILD_RFD_RX_IDLE 2
#define REL_CHILD_FFD_RX_IDLE 4
#define REL_NOT_USED          0xFF

/* User-supplied code revision in SYS_VERSION (a build date). */
#define ZNP_CODE_REVISION 20261014U
/* The host takes one NV chunk per request; stay below the 250 byte frame. */
#define NV_CHUNK          240
/* Size of Z-Stack 3.30's associated_devices_t on a 32 bit ARM: tells hosts how it aligns. */
#define ASSOC_DEV_BYTES   36

#define U16(p) mt_get_u16(p)

/* Status for a failed NV lookup or access. */
static uint8_t nv_status(int err)
{
	switch (err) {
	case 0:
		return MT_STATUS_SUCCESS;
	case -EINVAL:
		return MT_STATUS_INVALID_PARAMETER;
	default:
		return MT_STATUS_NV_OPER_FAILED;
	}
}

/* Apply a transmit power: through the stack to the radio, else the app's hook. */
static int8_t set_tx_power(struct znp *z, int8_t dbm)
{
	if (z->stack != NULL) {
		return zb_stack_set_txpower(z->stack, dbm);
	}
	return z->cfg.set_tx_power != NULL ? z->cfg.set_tx_power(z->cfg.user, dbm) : dbm;
}

static int status_only(struct mt_frame *rsp, uint8_t status)
{
	rsp->data[0] = status;
	rsp->len = 1;
	return 0;
}

/* SRSP: status, length byte, then up to @p want bytes of the item from @p off. */
static int nv_read_rsp(struct znp *z, struct mt_frame *rsp, struct znp_nv_key key, uint16_t off,
		       uint16_t want)
{
	int n = znp_nv_read(z->cfg.nv, key, off, &rsp->data[2], MIN(want, NV_CHUNK));

	if (n < 0) {
		rsp->data[0] = nv_status(n);
		rsp->data[1] = 0;
		rsp->len = 2;
		return 0;
	}
	rsp->data[0] = MT_STATUS_SUCCESS;
	rsp->data[1] = (uint8_t)n;
	rsp->len = (uint8_t)(2 + n);
	return 0;
}

static int sys_nv(struct znp *z, const struct mt_frame *req, struct mt_frame *rsp)
{
	const uint8_t *d = req->data;
	uint8_t len = req->len;
	struct znp_nv *nv = z->cfg.nv;
	struct znp_nv_key key;
	int r;

	switch (req->cmd) {
	case SYS_OSAL_NV_ITEM_INIT: {
		/* id, item length, initial value length, initial value */
		if (len < 5U || len < 5U + d[4]) {
			return MT_ERR_LENGTH;
		}
		key = znp_nv_osal(U16(d));
		if (znp_nv_len(nv, key) != 0U) {
			return status_only(rsp, MT_STATUS_SUCCESS);
		}
		r = znp_nv_create(nv, key, U16(d + 2), d + 5, MIN(d[4], U16(d + 2)));
		return status_only(rsp, r == 0 ? MT_STATUS_NV_ITEM_UNINIT : MT_STATUS_NV_OPER_FAILED);
	}
	case SYS_OSAL_NV_READ:
		if (len != 3U) {
			return MT_ERR_LENGTH;
		}
		return nv_read_rsp(z, rsp, znp_nv_osal(U16(d)), d[2], NV_CHUNK);
	case SYS_OSAL_NV_READ_EXT:
		if (len != 4U) {
			return MT_ERR_LENGTH;
		}
		return nv_read_rsp(z, rsp, znp_nv_osal(U16(d)), U16(d + 2), NV_CHUNK);
	case SYS_OSAL_NV_WRITE:
		/* id, offset, length, value */
		if (len < 4U || len != 4U + d[3]) {
			return MT_ERR_LENGTH;
		}
		return status_only(rsp, nv_status(znp_nv_write(nv, znp_nv_osal(U16(d)), d[2], d + 4,
							       d[3])));
	case SYS_OSAL_NV_WRITE_EXT:
		/* id, offset (16 bit), length (16 bit), value */
		if (len < 6U || len != 6U + U16(d + 4)) {
			return MT_ERR_LENGTH;
		}
		return status_only(rsp, nv_status(znp_nv_write(nv, znp_nv_osal(U16(d)), U16(d + 2),
							       d + 6, U16(d + 4))));
	case SYS_OSAL_NV_DELETE:
		if (len != 4U) {
			return MT_ERR_LENGTH;
		}
		key = znp_nv_osal(U16(d));
		if (znp_nv_len(nv, key) != U16(d + 2)) {
			return status_only(rsp, znp_nv_len(nv, key) == 0U ? MT_STATUS_NV_OPER_FAILED
									  : MT_STATUS_NV_BAD_ITEM_LEN);
		}
		return status_only(rsp, nv_status(znp_nv_delete(nv, key)));
	case SYS_OSAL_NV_LENGTH:
		if (len != 2U) {
			return MT_ERR_LENGTH;
		}
		mt_put_u16(rsp->data, znp_nv_len(nv, znp_nv_osal(U16(d))));
		rsp->len = 2;
		return 0;
	/* Extended NV: system, item id, sub id, ... */
	case SYS_NV_CREATE:
		if (len != 9U) {
			return MT_ERR_LENGTH;
		}
		key = (struct znp_nv_key){.sys = d[0], .item = U16(d + 1), .sub = U16(d + 3)};
		if (mt_get_u32(d + 5) > UINT16_MAX) {
			return status_only(rsp, MT_STATUS_INVALID_PARAMETER);
		}
		if (znp_nv_len(nv, key) != 0U) {
			return status_only(rsp, MT_STATUS_SUCCESS);
		}
		r = znp_nv_create(nv, key, (uint16_t)mt_get_u32(d + 5), NULL, 0);
		/* herdsman insists on 0 here, zigpy-znp takes 0 as well as NV_ITEM_UNINIT. */
		return status_only(rsp, r == 0 ? MT_STATUS_SUCCESS : MT_STATUS_NV_OPER_FAILED);
	case SYS_NV_DELETE:
		if (len != 5U) {
			return MT_ERR_LENGTH;
		}
		key = (struct znp_nv_key){.sys = d[0], .item = U16(d + 1), .sub = U16(d + 3)};
		return status_only(rsp, nv_status(znp_nv_delete(nv, key)));
	case SYS_NV_LENGTH:
		if (len != 5U) {
			return MT_ERR_LENGTH;
		}
		key = (struct znp_nv_key){.sys = d[0], .item = U16(d + 1), .sub = U16(d + 3)};
		mt_put_u32(rsp->data, znp_nv_len(nv, key));
		rsp->len = 4;
		return 0;
	case SYS_NV_READ:
		/* system, item, sub, offset (16 bit), length */
		if (len != 8U) {
			return MT_ERR_LENGTH;
		}
		key = (struct znp_nv_key){.sys = d[0], .item = U16(d + 1), .sub = U16(d + 3)};
		return nv_read_rsp(z, rsp, key, U16(d + 5), d[7]);
	case SYS_NV_WRITE:
		/* system, item, sub, offset (16 bit), length, value */
		if (len < 8U || len != 8U + d[7]) {
			return MT_ERR_LENGTH;
		}
		key = (struct znp_nv_key){.sys = d[0], .item = U16(d + 1), .sub = U16(d + 3)};
		return status_only(rsp, nv_status(znp_nv_write(nv, key, U16(d + 5), d + 8, d[7])));
	}
	return MT_ERR_COMMAND;
}

int znp_sys_handler(struct znp *z, const struct mt_frame *req, struct mt_frame *rsp)
{
	switch (req->cmd) {
	case SYS_RESET_REQ:
		/* AREQ: type 0 hard, 1 soft, 2 shutdown (treated as a soft reset). */
		if (req->len != 1U) {
			return 0;
		}
		if (z->stack != NULL) {
			zb_stack_reset(z->stack, req->data[0]);
		} else if (z->cfg.reset != NULL) {
			z->cfg.reset(z->cfg.user, req->data[0]);
		} else {
			znp_send_reset_ind(z, ZNP_RESET_EXTERNAL);
		}
		return 0;
	case SYS_PING:
		mt_put_u16(rsp->data, z->caps);
		rsp->len = 2;
		return 0;
	case SYS_VERSION: {
		const uint8_t v[] = {ZNP_TRANSPORT_REV, ZNP_PRODUCT_ID, ZNP_VERSION_MAJOR,
				     ZNP_VERSION_MINOR, ZNP_VERSION_MAINT};

		memcpy(rsp->data, v, sizeof(v));
		mt_put_u32(&rsp->data[5], ZNP_CODE_REVISION);
		/* No bootloader: build type 0, revision "not supported". */
		rsp->data[9] = 0;
		mt_put_u32(&rsp->data[10], 0xFFFFFFFFU);
		rsp->len = 14;
		return 0;
	}
	case SYS_SET_EXT_ADDR:
		if (req->len != 8U) {
			return MT_ERR_LENGTH;
		}
		memcpy(z->cfg.ext_addr, req->data, 8);
		if (z->cfg.nv != NULL) {
			(void)znp_nv_set(z->cfg.nv, znp_nv_osal(0x0001), req->data, 8);
		}
		return status_only(rsp, MT_STATUS_SUCCESS);
	case SYS_GET_EXT_ADDR:
		memcpy(rsp->data, z->cfg.ext_addr, 8);
		rsp->len = 8;
		return 0;
	case SYS_STACK_TUNE:
		/* operation 0: transmit power in dBm (reply: the value in force); 1: rx on idle */
		if (req->len != 2U) {
			return MT_ERR_LENGTH;
		}
		if (req->data[0] == 0U) {
			z->tx_power = set_tx_power(z, (int8_t)req->data[1]);
			rsp->data[0] = (uint8_t)z->tx_power;
		} else {
			rsp->data[0] = 0;
		}
		rsp->len = 1;
		return 0;
	case SYS_SET_TX_POWER:
		if (req->len != 1U) {
			return MT_ERR_LENGTH;
		}
		z->tx_power = set_tx_power(z, (int8_t)req->data[0]);
		/* Z-Stack 3.30: a status (0 = success). */
		return status_only(rsp, MT_STATUS_SUCCESS);
	}
	return sys_nv(z, req, rsp);
}

/* The 36 byte associated_devices_t of a child, or the "not used" one. */
static void assoc_struct(struct mt_frame *rsp, const struct zb_child *c, int idx)
{
	memset(rsp->data, 0xFF, ASSOC_DEV_BYTES);
	if (c != NULL) {
		memset(rsp->data, 0, ASSOC_DEV_BYTES);
		mt_put_u16(&rsp->data[0], c->nwk);
		mt_put_u16(&rsp->data[2], (uint16_t)idx);
		rsp->data[4] = (c->cap & ZB_CAP_FFD) != 0U ? REL_CHILD_FFD_RX_IDLE : REL_CHILD_RFD_RX_IDLE;
	} else {
		rsp->data[4] = REL_NOT_USED;
	}
	rsp->len = ASSOC_DEV_BYTES;
}

int znp_util_handler(struct znp *z, const struct mt_frame *req, struct mt_frame *rsp)
{
	struct zb_stack *st = z->stack;

	switch (req->cmd) {
	case UTIL_GET_DEVICE_INFO: {
		/* status, IEEE, NWK, device type, state, associated devices (count, NWK list) */
		uint8_t n = 0;

		rsp->data[0] = MT_STATUS_SUCCESS;
		memcpy(&rsp->data[1], z->cfg.ext_addr, 8);
		mt_put_u16(&rsp->data[9], st != NULL && st->net.formed ? 0x0000 : 0xFFFE);
		rsp->data[11] = 1; /* coordinator */
		rsp->data[12] = st != NULL ? st->net.dev_state : 0;
		if (st != NULL) {
			for (int i = 0; i < ZB_NET_MAX_CHILDREN && n < 16U; i++) {
				if (st->net.children[i].used && !st->net.children[i].tentative) {
					mt_put_u16(&rsp->data[14 + 2 * n], st->net.children[i].nwk);
					n++;
				}
			}
		}
		rsp->data[13] = n;
		rsp->len = (uint8_t)(14U + 2U * n);
		return 0;
	}
	case UTIL_LED_CONTROL:
		if (req->len != 2U) {
			return MT_ERR_LENGTH;
		}
		return status_only(rsp, MT_STATUS_SUCCESS);
	case UTIL_ASSOC_FIND_DEVICE: {
		int found = -1;

		if (req->len != 1U) {
			return MT_ERR_LENGTH;
		}
		if (st != NULL) {
			for (int i = 0, k = 0; i < ZB_NET_MAX_CHILDREN && found < 0; i++) {
				if (st->net.children[i].used && !st->net.children[i].tentative &&
				    k++ == req->data[0]) {
					found = i;
				}
			}
		}
		assoc_struct(rsp, st != NULL && found >= 0 ? &st->net.children[found] : NULL, found);
		return 0;
	}
	case UTIL_ASSOC_GET_WITH_ADDRESS: {
		/* IEEE address (all zero: look up by the short address), short address */
		static const uint8_t zero[8];
		struct zb_child *c = NULL;

		if (req->len != 10U) {
			return MT_ERR_LENGTH;
		}
		if (st != NULL) {
			c = memcmp(req->data, zero, 8) == 0
				    ? zb_net_child_by_nwk(&st->net, mt_get_u16(&req->data[8]))
				    : zb_net_child_by_ext(&st->net, req->data);
		}
		assoc_struct(rsp, c != NULL && !c->tentative ? c : NULL, c != NULL ? (int)(c - st->net.children) : -1);
		return 0;
	}
	case UTIL_ASSOC_REMOVE:
		if (req->len != 8U) {
			return MT_ERR_LENGTH;
		}
		return status_only(rsp, st != NULL && zb_net_child_remove(&st->net, req->data) >= 0
						? MT_STATUS_SUCCESS
						: MT_STATUS_FAILURE);
	case UTIL_ASSOC_ADD:
		/* short address, IEEE address, node relation */
		if (req->len != 11U) {
			return MT_ERR_LENGTH;
		}
		return status_only(rsp, st != NULL &&
						zb_net_child_add(&st->net, mt_get_u16(req->data),
								 &req->data[2],
								 req->data[10] == REL_CHILD_RFD_RX_IDLE ||
										 req->data[10] == 1U
									 ? 0x80
									 : 0x8E) >= 0
						? MT_STATUS_SUCCESS
						: MT_STATUS_FAILURE);
	}
	return MT_ERR_COMMAND;
}
