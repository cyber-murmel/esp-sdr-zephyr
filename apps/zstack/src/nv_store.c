/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * NV store backend: one NVS entry per slot (id slot + 1), holding the item key and data.
 */
#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>

#include "zstack.h"

LOG_MODULE_REGISTER(nv_store, LOG_LEVEL_INF);

#define STORAGE_ID   PARTITION_ID(storage_partition)
#define KEY_BYTES    5
#define REC_MAX      (KEY_BYTES + CONFIG_ZSTACK_NV_REC_MAX)

static struct nvs_fs fs;
/* Only the main loop and the reset hook use it, never at the same time. */
static uint8_t rec[REC_MAX];

static int store(void *ctx, uint16_t slot, const struct znp_nv_key *key, const void *data,
		 uint16_t len)
{
	ARG_UNUSED(ctx);

	if (len == 0U) {
		return nvs_delete(&fs, slot + 1U);
	}
	if (len > CONFIG_ZSTACK_NV_REC_MAX) {
		LOG_WRN("NV item %02x/%04x/%04x of %u bytes is too large to store", key->sys,
			key->item, key->sub, len);
		return -EFBIG;
	}
	rec[0] = key->sys;
	mt_put_u16(&rec[1], key->item);
	mt_put_u16(&rec[3], key->sub);
	memcpy(&rec[KEY_BYTES], data, len);

	int r = nvs_write(&fs, slot + 1U, rec, KEY_BYTES + len);

	return r < 0 ? r : 0;
}

int nv_store_init(struct znp_nv *nv)
{
	struct flash_pages_info page;
	int ret, restored = 0;

	fs.flash_device = PARTITION_DEVICE(storage_partition);
	fs.offset = PARTITION_OFFSET(storage_partition);
	if (!device_is_ready(fs.flash_device)) {
		return -ENODEV;
	}
	ret = flash_get_page_info_by_offs(fs.flash_device, fs.offset, &page);
	if (ret != 0) {
		return ret;
	}
	fs.sector_size = page.size;
	fs.sector_count = PARTITION_SIZE(storage_partition) / page.size;
	ret = nvs_mount(&fs);
	if (ret != 0) {
		LOG_ERR("nvs_mount: %d", ret);
		return ret;
	}

	for (int slot = 0; slot < CONFIG_ZNP_NV_ITEMS; slot++) {
		ssize_t n = nvs_read(&fs, slot + 1, rec, sizeof(rec));

		if (n <= KEY_BYTES) {
			continue;
		}
		struct znp_nv_key key = {
			.sys = rec[0], .item = mt_get_u16(&rec[1]), .sub = mt_get_u16(&rec[3])};

		if (znp_nv_restore(nv, slot, key, &rec[KEY_BYTES], n - KEY_BYTES) == 0) {
			restored++;
		}
	}
	LOG_INF("NV store: %d items restored", restored);
	nv->be = (struct znp_nv_backend){.store = store};
	return 0;
}
