/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Factory default NV items. Hosts read some of these before they ever write them, and crash
 * or fall back to another Z-Stack dialect when they are missing (see
 * tmp/zstack/phase2/zigpy_sequence.md section 3.3 and herdsman.md section 3).
 */
#include <string.h>

#include <zigbee/zb_net.h>

struct nvdef {
	uint16_t id;
	uint16_t len;
	uint8_t v[4];
	/* Fill byte for the rest (items longer than the value). */
	uint8_t fill;
};

/* Config items: a ClearConfig puts these back. */
static const struct nvdef config_items[] = {
	{ZB_NV_STARTUP_OPTION, 1, {0}, 0},
	{ZB_NV_BCAST_DELIVERY_TIME, 1, {30}, 0},
	{ZB_NV_CONCENTRATOR_ENABLE, 1, {1}, 0},
	{ZB_NV_CONCENTRATOR_DISCOVERY, 1, {120}, 0},
	{ZB_NV_CONCENTRATOR_RC, 1, {1}, 0},
	{ZB_NV_SRC_RTG_EXPIRY_TIME, 1, {255}, 0},
	{ZB_NV_NWK_CHILD_AGE_ENABLE, 1, {0}, 0},
	{ZB_NV_EXTENDED_PAN_ID, 8, {0}, 0},
	{ZB_NV_APS_USE_EXT_PANID, 8, {0}, 0},
	{ZB_NV_PRECFGKEY, 16, {0}, 0},
	{ZB_NV_PRECFGKEYS_ENABLE, 1, {1}, 0},
	{ZB_NV_PANID, 2, {0xFF, 0xFF}, 0},
	{ZB_NV_CHANLIST, 4, {0x00, 0xF8, 0xFF, 0x07}, 0},
	{ZB_NV_LOGICAL_TYPE, 1, {0}, 0},
	{ZB_NV_ZDO_DIRECT_CB, 1, {1}, 0},
	/* zigpy-znp's own marker: 1 saves it a migration and a reset. */
	{ZB_NV_MIGRATION_ID, 1, {1}, 0},
	/* herdsman reads this one to pick the struct alignment: 21 packed, 24 aligned. */
	{ZB_NV_NWKKEY, ZB_NWKKEY_LEN, {0}, 0},
	/* count, then entries of 6 bytes (aligned): none in use. */
	{ZB_NV_APS_LINK_KEY_TABLE, 2 + 6 * ZB_LINK_KEY_TABLE_ENTRIES, {0}, 0},
};

static void set_item(struct znp_nv *nv, uint16_t id, const void *data, uint16_t len)
{
	(void)znp_nv_set(nv, znp_nv_osal(id), data, len);
}

static void make_item(struct znp_nv *nv, const struct nvdef *d, bool replace)
{
	uint8_t buf[ZB_NWKKEY_LEN + 2 + 6 * ZB_LINK_KEY_TABLE_ENTRIES];
	size_t vlen = MIN(sizeof(d->v), (size_t)d->len);

	if (!replace && znp_nv_len(nv, znp_nv_osal(d->id)) != 0U) {
		return;
	}
	memset(buf, d->fill, d->len);
	memcpy(buf, d->v, vlen);
	set_item(nv, d->id, buf, d->len);
}

static void table(struct znp_nv *nv, uint8_t item, size_t entries, uint16_t len, bool reset,
		  void (*fill)(uint8_t *e))
{
	uint8_t e[ZB_APS_KEY_LEN];

	for (size_t i = 0; i < entries; i++) {
		struct znp_nv_key k = {.sys = ZNP_NV_SYS_ZSTACK, .item = item, .sub = (uint16_t)i};

		if (!reset && znp_nv_len(nv, k) != 0U) {
			continue;
		}
		memset(e, 0, len);
		fill(e);
		(void)znp_nv_set(nv, k, e, len);
	}
}

/* Empty entries as Z-Stack 3.30 stores them. */
static void fill_addrmgr(uint8_t *e)
{
	memset(e, 0xFF, ZB_ADDRMGR_LEN);
}

static void fill_tclk(uint8_t *e)
{
	e[16] = 0xFF; /* keyAttributes: default key */
}

static void fill_zero(uint8_t *e)
{
	ARG_UNUSED(e);
}

static void tables(struct znp_nv *nv, bool reset)
{
	table(nv, ZB_NVEX_ADDRMGR, ZB_TABLE_ENTRIES, ZB_ADDRMGR_LEN, reset, fill_addrmgr);
	table(nv, ZB_NVEX_TCLK_TABLE, ZB_TABLE_ENTRIES, ZB_TCLK_LEN, reset, fill_tclk);
	table(nv, ZB_NVEX_APS_KEY_DATA, ZB_TABLE_ENTRIES, ZB_APS_KEY_LEN, reset, fill_zero);
	table(nv, ZB_NVEX_NWK_SEC_MATERIAL, ZB_SECMAT_ENTRIES, ZB_SECMAT_LEN, false, fill_zero);
}

void zb_nvdef_ensure(struct znp_nv *nv, const uint8_t eui[8], bool reset_config)
{
	for (size_t i = 0; i < ARRAY_SIZE(config_items); i++) {
		make_item(nv, &config_items[i], reset_config);
	}
	if (znp_nv_len(nv, znp_nv_osal(ZB_NV_EXTADDR)) == 0U) {
		set_item(nv, ZB_NV_EXTADDR, eui, 8);
	}
	tables(nv, false);
}

void zb_nvdef_clear_state(struct znp_nv *nv)
{
	static const uint16_t gone[] = {ZB_NV_NIB, ZB_NV_BDB_NODE_IS_ON_NETWORK,
					ZB_NV_NWK_ACTIVE_KEY_INFO, ZB_NV_NWK_ALTERN_KEY_INFO,
					ZB_NV_HAS_CONFIGURED_ZSTACK3};

	for (size_t i = 0; i < ARRAY_SIZE(gone); i++) {
		(void)znp_nv_delete(nv, znp_nv_osal(gone[i]));
	}
	/* Keys and tables back to empty; the frame counters (security material) stay. */
	if (znp_nv_len(nv, znp_nv_osal(ZB_NV_APS_LINK_KEY_TABLE)) != 0U) {
		uint16_t n = znp_nv_len(nv, znp_nv_osal(ZB_NV_APS_LINK_KEY_TABLE));
		uint8_t z[2 + 6 * ZB_LINK_KEY_TABLE_ENTRIES] = {0};

		if (n <= sizeof(z)) {
			set_item(nv, ZB_NV_APS_LINK_KEY_TABLE, z, n);
		}
	}
	tables(nv, true);
}
