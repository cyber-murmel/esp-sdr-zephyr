/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The ZNP non-volatile item store: Z-Stack's OSAL NV (16-bit id) and the extended NV
 * (system, item, sub id) as one key space, items of fixed length created by the host.
 * RAM copy in a pool; changes are written through a backend by znp_nv_flush(), which
 * the application calls some time after the last change.
 */
#ifndef ZIGBEE_ZNP_NV_H_
#define ZIGBEE_ZNP_NV_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#define ZNP_NV_SYS_ZSTACK  1
/* Extended NV item id under which OSAL NV ids are kept (the id is the sub id). */
#define ZNP_NV_ITEM_LEGACY 0

struct znp_nv_key {
	uint8_t sys;
	uint16_t item;
	uint16_t sub;
};

static inline struct znp_nv_key znp_nv_osal(uint16_t id)
{
	return (struct znp_nv_key){.sys = ZNP_NV_SYS_ZSTACK, .item = ZNP_NV_ITEM_LEGACY, .sub = id};
}

struct znp_nv_backend {
	/* Store one slot (len > 0) or erase it (len == 0); 0 on success. */
	int (*store)(void *ctx, uint16_t slot, const struct znp_nv_key *key, const void *data,
		     uint16_t len);
	void *ctx;
};

struct znp_nv_item {
	struct znp_nv_key key;
	uint16_t len;
	uint16_t off;
	bool used;
	bool dirty;
};

struct znp_nv {
	struct znp_nv_item items[CONFIG_ZNP_NV_ITEMS];
	uint8_t pool[CONFIG_ZNP_NV_POOL_SIZE];
	uint16_t pool_used;
	struct znp_nv_backend be;
	struct k_mutex lock;
};

void znp_nv_init(struct znp_nv *nv, const struct znp_nv_backend *be);

/* Length of an item, 0 when it does not exist. */
uint16_t znp_nv_len(struct znp_nv *nv, struct znp_nv_key key);

/* Create an item of @p len bytes, zero filled but for @p init_len bytes of @p init. */
int znp_nv_create(struct znp_nv *nv, struct znp_nv_key key, uint16_t len, const void *init,
		  uint16_t init_len);

/* Copy up to @p n bytes from @p off; returns the count (0 at the end) or a -errno. */
int znp_nv_read(struct znp_nv *nv, struct znp_nv_key key, uint16_t off, void *buf, uint16_t n);

/* Overwrite @p n bytes at @p off; -ENOENT, or -EINVAL when it runs past the item. */
int znp_nv_write(struct znp_nv *nv, struct znp_nv_key key, uint16_t off, const void *buf,
		 uint16_t n);

int znp_nv_delete(struct znp_nv *nv, struct znp_nv_key key);

/* Create-or-replace the whole item (for the stack's own state); marks it dirty. */
int znp_nv_set(struct znp_nv *nv, struct znp_nv_key key, const void *data, uint16_t len);

/* Load a stored slot at boot (does not mark dirty); the slot number is kept. */
int znp_nv_restore(struct znp_nv *nv, uint16_t slot, struct znp_nv_key key, const void *data,
		   uint16_t len);

/* Push changed slots to the backend; returns the first backend error or 0. */
int znp_nv_flush(struct znp_nv *nv);

/* Items that need a flush. */
bool znp_nv_dirty(struct znp_nv *nv);

#endif /* ZIGBEE_ZNP_NV_H_ */
