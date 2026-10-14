/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Network layer, phase 2 part: see include/zigbee/zb_net.h.
 */
#include <errno.h>
#include <string.h>

#include <zephyr/random/random.h>

#include <zigbee/mt.h>
#include <zigbee/zb_net.h>

#include "zb_net_priv.h"

#define BDB_NOTIFY_REMAINING 0x04
#define SCAN_EXP             3
/* A BDB steering window (Z-Stack opens joining for 180 s). */
#define STEERING_SECONDS     180
#define BEACONS_PER_AREQ     8

struct znp_nv_key zbn_exkey(uint8_t item, uint16_t sub)
{
	return (struct znp_nv_key){.sys = ZNP_NV_SYS_ZSTACK, .item = item, .sub = sub};
}

static bool nv_read(struct zb_net *n, uint16_t id, void *buf, uint16_t len)
{
	return znp_nv_len(n->nv, znp_nv_osal(id)) == len &&
	       znp_nv_read(n->nv, znp_nv_osal(id), 0, buf, len) == (int)len;
}

static void nv_put(struct zb_net *n, uint16_t id, const void *buf, uint16_t len)
{
	(void)znp_nv_set(n->nv, znp_nv_osal(id), buf, len);
}

static void nv_put8(struct zb_net *n, uint16_t id, uint8_t v)
{
	nv_put(n, id, &v, 1);
}

static bool all_equal(const uint8_t *p, size_t len, uint8_t v)
{
	for (size_t i = 0; i < len; i++) {
		if (p[i] != v) {
			return false;
		}
	}
	return true;
}

/* Keys and seeds only ever come from the cryptographic generator. */
static int random_bytes(void *buf, size_t len)
{
	return sys_csrand_get(buf, len);
}

/* ------------------------------------------------------------------------------------------ */
/* NIB */

/* A NIB with Z-Stack 3.30's defaults for a running coordinator on @p ch, @p pan. */
static void nib_build(uint8_t nib[ZB_NIB_LEN], const struct zb_net *n, uint8_t ch, uint16_t pan,
		      const uint8_t epid[8], uint32_t channels)
{
	memset(nib, 0, ZB_NIB_LEN);
	nib[1] = 5;  /* PassiveAckTimeout */
	nib[2] = 2;  /* MaxBroadcastRetries */
	nib[3] = 20; /* MaxChildren */
	nib[4] = 20; /* MaxDepth */
	nib[5] = 6;  /* MaxRouters */
	nib[7] = 30; /* BroadcastDeliveryTime */
	nib[9] = 3;  /* RouteDiscRetries */
	nib[11] = 1; /* SecureAllFrames */
	nib[ZB_NIB_SECURITY_LEVEL] = 5;
	nib[13] = 1;    /* SymLink */
	nib[14] = 0x8F; /* CapabilityFlags */
	mt_put_u16(&nib[16], 0x01F4); /* TransactionPersistenceTime */
	nib[18] = 2;                  /* nwkProtocolVersion */
	nib[19] = 5;                  /* RouteDiscoveryTime */
	nib[20] = 30;                 /* RouteExpiryTime */
	mt_put_u16(&nib[ZB_NIB_DEV_ADDR], 0x0000);
	nib[ZB_NIB_CHANNEL] = ch;
	mt_put_u16(&nib[ZB_NIB_COORD_ADDR], 0x0000);
	memcpy(&nib[ZB_NIB_COORD_EXT], n->eui, 8);
	mt_put_u16(&nib[ZB_NIB_PAN_ID], pan);
	nib[ZB_NIB_STATE] = ZB_NWK_ROUTER;
	mt_put_u32(&nib[ZB_NIB_CHANNEL_LIST], channels);
	nib[44] = 15; /* beaconOrder */
	nib[45] = 15; /* superFrameOrder */
	nib[46] = 5;  /* scanDuration */
	memcpy(&nib[ZB_NIB_EPID], epid, 8);
	nib[ZB_NIB_KEY_LOADED] = 1;
	nib[102] = 15;  /* nwkLinkStatusPeriod */
	nib[103] = 3;   /* nwkRouterAgeLimit */
	nib[105] = 1;   /* nwkIsConcentrator */
	nib[106] = 120; /* nwkConcentratorDiscoveryTime */
	nib[107] = 30;  /* nwkConcentratorRadius */
	nib[108] = 1;   /* nwkAllFresh */
}

/* Network parameters from NV, if a formed network is stored. */
static bool load_network(struct zb_net *n)
{
	uint8_t nib[ZB_NIB_LEN], key[ZB_KEYDESC_LEN], on = 0;
	uint16_t pan;
	uint8_t ch;

	if (!nv_read(n, ZB_NV_NIB, nib, sizeof(nib)) || !nv_read(n, ZB_NV_BDB_NODE_IS_ON_NETWORK, &on, 1) ||
	    on != 1U || !nv_read(n, ZB_NV_NWK_ACTIVE_KEY_INFO, key, sizeof(key))) {
		return false;
	}
	ch = nib[ZB_NIB_CHANNEL];
	pan = mt_get_u16(&nib[ZB_NIB_PAN_ID]);
	if (ch < 11U || ch > 26U || pan == 0U || pan >= 0xFFFEU || nib[ZB_NIB_KEY_LOADED] != 1U) {
		return false;
	}
	n->channel = ch;
	n->pan = pan;
	memcpy(n->epid, &nib[ZB_NIB_EPID], 8);
	n->upd = nib[ZB_NIB_UPDATE_ID];
	return true;
}

/* ------------------------------------------------------------------------------------------ */
/* Children and association policy */

struct zb_child *zbn_child_by_ext(struct zb_net *n, const uint8_t ext[8])
{
	for (int i = 0; i < ZB_NET_MAX_CHILDREN; i++) {
		if (n->children[i].used && memcmp(n->children[i].ext, ext, 8) == 0) {
			return &n->children[i];
		}
	}
	return NULL;
}

struct zb_child *zbn_child_by_nwk(struct zb_net *n, uint16_t nwk)
{
	for (int i = 0; i < ZB_NET_MAX_CHILDREN; i++) {
		if (n->children[i].used && n->children[i].nwk == nwk) {
			return &n->children[i];
		}
	}
	return NULL;
}

static size_t child_count(const struct zb_net *n)
{
	size_t c = 0;

	for (int i = 0; i < ZB_NET_MAX_CHILDREN; i++) {
		c += n->children[i].used ? 1U : 0U;
	}
	return c;
}

/* AddrMgrUserType bits: Assoc is ours, Security and the rest belong to the key tables. */
#define ADDRMGR_ASSOC    BIT(0)
#define ADDRMGR_SECURITY BIT(1)

static bool addrmgr_read(struct zb_net *n, int idx, uint8_t e[ZB_ADDRMGR_LEN])
{
	return znp_nv_read(n->nv, zbn_exkey(ZB_NVEX_ADDRMGR, (uint16_t)idx), 0, e, ZB_ADDRMGR_LEN) ==
	       (int)ZB_ADDRMGR_LEN;
}

/* The ADDRMGR entry of a child: Assoc set while it is a child, cleared when it is not. */
void zbn_child_persist(struct zb_net *n, int idx)
{
	const struct zb_child *c = &n->children[idx];
	uint8_t e[ZB_ADDRMGR_LEN];
	bool mine = c->used && !c->tentative;

	if (!addrmgr_read(n, idx, e)) {
		memset(e, 0xFF, sizeof(e));
	}
	if (mine) {
		e[0] = (e[0] == 0xFFU ? 0U : e[0]) | ADDRMGR_ASSOC;
		e[0] = c->authorized ? (e[0] | ADDRMGR_SECURITY) : (e[0] & ~ADDRMGR_SECURITY);
		e[1] = 0xFF; /* alignment padding */
		mt_put_u16(&e[2], c->nwk);
		memcpy(&e[4], c->ext, 8);
	} else if (e[0] != 0xFFU && (e[0] & ~ADDRMGR_ASSOC) != 0U) {
		e[0] &= ~ADDRMGR_ASSOC; /* still a key table entry */
	} else {
		memset(e, 0xFF, sizeof(e));
	}
	(void)znp_nv_set(n->nv, zbn_exkey(ZB_NVEX_ADDRMGR, (uint16_t)idx), e, sizeof(e));
}

/* Only entries with the Assoc bit are children; the host's other entries stay its own. */
static void load_children(struct zb_net *n)
{
	memset(n->children, 0, sizeof(n->children));
	for (int i = 0; i < ZB_NET_MAX_CHILDREN; i++) {
		uint8_t e[ZB_ADDRMGR_LEN];

		if (!addrmgr_read(n, i, e) || e[0] == 0xFFU || (e[0] & ADDRMGR_ASSOC) == 0U ||
		    mt_get_u16(&e[2]) == 0xFFFFU) {
			continue;
		}
		n->children[i] = (struct zb_child){.used = true,
						   .authorized = (e[0] & ADDRMGR_SECURITY) != 0U,
						   .nwk = mt_get_u16(&e[2]),
						   .cap = 0x8E};
		memcpy(n->children[i].ext, &e[4], 8);
	}
}

/* A table slot that is neither a child nor held by a host-written key table entry. */
static int free_slot(struct zb_net *n)
{
	for (int i = 0; i < ZB_NET_MAX_CHILDREN; i++) {
		uint8_t e[ZB_ADDRMGR_LEN];

		if (!n->children[i].used && (!addrmgr_read(n, i, e) || e[0] == 0xFFU)) {
			return i;
		}
	}
	return -1;
}

static void update_capacity(struct zb_net *n)
{
	bool room = free_slot(n) >= 0;

	(void)zb_mac_set_capacity(n->mac, room, room);
}

static uint8_t net_assoc_req(void *ctx, const uint8_t ext[8], uint8_t cap, uint16_t *short_addr)
{
	struct zb_net *n = ctx;
	struct zb_child *c;
	uint8_t status = ZB_ASSOC_OK;

	k_mutex_lock(&n->lock, K_FOREVER);
	c = zbn_child_by_ext(n, ext);
	if (c != NULL) {
		/* A device that joins again keeps its address, and gets the network key again. */
		c->cap = cap;
		c->tentative = true;
		c->authorized = false;
		c->rx_fc = 0;
		*short_addr = c->nwk;
		goto out;
	}
	{
		int i = free_slot(n);
		uint16_t a;

		if (i >= 0) {
			/* Stochastic address assignment: any free address in 0x0001-0xFFF7. */
			do {
				a = (uint16_t)sys_rand32_get();
			} while (a == 0U || a > 0xFFF7U || zbn_child_by_nwk(n, a) != NULL);
			n->children[i] =
				(struct zb_child){.used = true, .tentative = true, .nwk = a, .cap = cap};
			memcpy(n->children[i].ext, ext, 8);
			*short_addr = a;
			update_capacity(n);
			goto out;
		}
	}
	status = ZB_ASSOC_PAN_FULL;
out:
	k_mutex_unlock(&n->lock);
	return status;
}

static void net_assoc_done(void *ctx, const uint8_t ext[8], uint16_t short_addr, bool delivered)
{
	struct zb_net *n = ctx;
	struct zb_child *c;
	int idx = -1;

	k_mutex_lock(&n->lock, K_FOREVER);
	c = zbn_child_by_ext(n, ext);
	if (c != NULL && c->tentative) {
		idx = (int)(c - n->children);
		if (delivered) {
			c->tentative = false;
			zbn_child_persist(n, idx);
		} else {
			memset(c, 0, sizeof(*c));
			zbn_child_persist(n, idx);
			update_capacity(n);
			idx = -1;
		}
	}
	k_mutex_unlock(&n->lock);

	/* The joiner has its address: the trust center now hands it the network key. */
	if (idx >= 0) {
		zbn_tc_join(n, idx);
	}
}

static void net_data_ind(void *ctx, const struct zb_mac_hdr *h, const uint8_t *payload, size_t len,
			 uint8_t lqi, int8_t rssi)
{
	ARG_UNUSED(rssi);
	zbn_rx(ctx, h, payload, len, lqi);
}

static void net_tick(void *ctx)
{
	zbn_tick(ctx);
}

void zb_net_mac_up(struct zb_net *n, struct zb_mac_up *up)
{
	*up = (struct zb_mac_up){
		.assoc_req = net_assoc_req,
		.assoc_done = net_assoc_done,
		.data_ind = net_data_ind,
		.tick = net_tick,
		.ctx = n,
	};
}

struct zb_child *zb_net_child_by_ext(struct zb_net *n, const uint8_t ext[8])
{
	return zbn_child_by_ext(n, ext);
}

struct zb_child *zb_net_child_by_nwk(struct zb_net *n, uint16_t nwk)
{
	return zbn_child_by_nwk(n, nwk);
}

size_t zb_net_child_count(struct zb_net *n)
{
	return child_count(n);
}

const uint8_t *zb_net_eui(const struct zb_net *n)
{
	return n->eui;
}

/* ------------------------------------------------------------------------------------------ */
/* State, boot, formation */

static void set_state(struct zb_net *n, uint8_t st)
{
	n->dev_state = st;
	if (n->cb.state != NULL) {
		n->cb.state(n->cb.ctx, st);
	}
}

static void report_bdb(struct zb_net *n, uint8_t status, uint8_t mode, uint8_t remaining)
{
	if (n->cb.bdb != NULL) {
		n->cb.bdb(n->cb.ctx, status, mode, remaining);
	}
}

void zb_net_init(struct zb_net *n, struct zb_mac *mac, struct znp_nv *nv,
		 const struct zb_net_cb *cb)
{
	memset(&n->children, 0, sizeof(n->children));
	memset(&n->job, 0, sizeof(n->job));
	n->mac = mac;
	n->nv = nv;
	n->cb = *cb;
	n->formed = false;
	n->dev_state = ZB_DEV_HOLD;
	n->bdb_primary = 0;
	n->bdb_secondary = 0;
	k_mutex_init(&n->lock);
}

/* Program the MAC for the stored or new network. */
static int start_mac(struct zb_net *n)
{
	bool room = free_slot(n) >= 0;
	struct zb_beacon_info b = {
		.router_cap = room,
		.ed_cap = room,
		.upd = n->upd,
	};
	int r;

	memcpy(b.epid, n->epid, 8);
	zbn_security_load(n);
	r = zb_mac_start(n->mac, n->pan, n->channel, &b);
	if (r == 0) {
		n->formed = true;
	}
	return r;
}

int zb_net_boot(struct zb_net *n, const uint8_t default_eui[8])
{
	uint8_t so = 0;

	(void)zb_mac_stop(n->mac);
	n->formed = false;
	n->dev_state = ZB_DEV_HOLD;

	(void)nv_read(n, ZB_NV_STARTUP_OPTION, &so, 1);
	if ((so & ZB_STARTUP_CLEAR_CONFIG) != 0U) {
		zb_nvdef_ensure(n->nv, default_eui, true);
	}
	if ((so & ZB_STARTUP_CLEAR_STATE) != 0U) {
		zb_nvdef_clear_state(n->nv);
	}
	zb_nvdef_ensure(n->nv, default_eui, false);
	if (so != 0U) {
		nv_put8(n, ZB_NV_STARTUP_OPTION, 0);
	}

	if (!nv_read(n, ZB_NV_EXTADDR, n->eui, 8)) {
		memcpy(n->eui, default_eui, 8);
	}
	(void)zb_mac_set_eui(n->mac, n->eui);

	load_children(n);
	if (load_network(n) && start_mac(n) == 0) {
		n->dev_state = ZB_DEV_COORD;
	}
	(void)znp_nv_flush(n->nv);
	return 0;
}

static int popcount_mask(uint32_t m)
{
	return __builtin_popcount(m);
}

/* Form a new network (energy and active scan, choose, store, start). */
static int form_new(struct zb_net *n)
{
	uint32_t mask = n->bdb_primary != 0U ? n->bdb_primary : n->bdb_secondary;
	uint8_t ed[16], nib[ZB_NIB_LEN], key[ZB_KEYDESC_LEN], epid[8], secmat[ZB_SECMAT_LEN];
	struct zb_beacon_info *bcn = n->job.beacons;
	size_t nb = 0;
	uint8_t pick_ch = 0;
	uint16_t pan;
	int best_nets = INT32_MAX, best_ed = INT32_MAX, r;

	mask &= ZB_ALL_CHANNELS;
	if (mask == 0U) {
		mask = ZB_ALL_CHANNELS;
	}

	/* Scan every channel of the mask, quietest first. */
	r = zb_mac_energy_scan(n->mac, mask, SCAN_EXP, ed);
	if (r != 0) {
		return r;
	}
	r = zb_mac_active_scan(n->mac, mask, SCAN_EXP, bcn, ARRAY_SIZE(n->job.beacons), &nb);
	if (r != 0) {
		return r;
	}
	for (int ch = 11, i = 0; ch <= 26; ch++) {
		int nets = 0;

		if ((mask & BIT(ch)) == 0U) {
			continue;
		}
		for (size_t k = 0; k < nb; k++) {
			nets += bcn[k].channel == ch ? 1 : 0;
		}
		if (nets < best_nets || (nets == best_nets && ed[i] < best_ed)) {
			best_nets = nets;
			best_ed = ed[i];
			pick_ch = (uint8_t)ch;
		}
		i++;
	}
	if (pick_ch == 0U) {
		return -ENOENT;
	}

	/* PAN ID: the host's, else a random one nobody on the channel uses. */
	{
		uint8_t b[2];

		pan = nv_read(n, ZB_NV_PANID, b, 2) ? mt_get_u16(b) : 0xFFFF;
	}
	if (pan == 0xFFFFU || pan == 0xFFFEU || pan == 0U) {
		bool clash;

		do {
			pan = (uint16_t)(sys_rand32_get() & 0x3FFFU);
			clash = pan == 0U;
			for (size_t k = 0; k < nb; k++) {
				clash |= bcn[k].channel == pick_ch && bcn[k].pan == pan;
			}
		} while (clash);
	}

	/* Extended PAN ID: the host's, else this device's own IEEE address. */
	if (!nv_read(n, ZB_NV_APS_USE_EXT_PANID, epid, 8) || all_equal(epid, 8, 0x00) ||
	    all_equal(epid, 8, 0xDD)) {
		memcpy(epid, n->eui, 8);
	}

	/* Network key: the pre-configured one if the host wrote it, else random. */
	key[0] = 0; /* key sequence number */
	if (!nv_read(n, ZB_NV_PRECFGKEY, &key[1], 16) || all_equal(&key[1], 16, 0x00)) {
		if (random_bytes(&key[1], 16) != 0) {
			return -EIO;
		}
	}

	nib_build(nib, n, pick_ch, pan, epid, BIT(pick_ch));
	r = znp_nv_set(n->nv, znp_nv_osal(ZB_NV_NIB), nib, sizeof(nib));
	r |= znp_nv_set(n->nv, znp_nv_osal(ZB_NV_NWK_ACTIVE_KEY_INFO), key, sizeof(key));
	r |= znp_nv_set(n->nv, znp_nv_osal(ZB_NV_NWK_ALTERN_KEY_INFO), key, sizeof(key));
	r |= znp_nv_set(n->nv, znp_nv_osal(ZB_NV_EXTENDED_PAN_ID), epid, 8);
	if (znp_nv_len(n->nv, znp_nv_osal(ZB_NV_TCLK_SEED)) != 16U) {
		uint8_t seed[16];

		if (random_bytes(seed, sizeof(seed)) != 0) {
			return -EIO;
		}
		r |= znp_nv_set(n->nv, znp_nv_osal(ZB_NV_TCLK_SEED), seed, sizeof(seed));
	}
	/* The NWK frame counter of this network: starts at 0. */
	memset(secmat, 0, sizeof(secmat));
	memcpy(&secmat[4], epid, 8);
	r |= znp_nv_set(n->nv, zbn_exkey(ZB_NVEX_NWK_SEC_MATERIAL, 0), secmat, sizeof(secmat));
	/* A new network has no children. */
	for (int i = 0; i < ZB_NET_MAX_CHILDREN; i++) {
		if (n->children[i].used) {
			memset(&n->children[i], 0, sizeof(n->children[i]));
			zbn_child_persist(n, i);
		}
	}
	/* The marker last: a network counts as formed only once everything else is stored. */
	{
		uint8_t on = 1;

		r |= znp_nv_set(n->nv, znp_nv_osal(ZB_NV_BDB_NODE_IS_ON_NETWORK), &on, 1);
	}
	r |= znp_nv_flush(n->nv);
	if (r != 0) {
		return -EIO;
	}

	memcpy(n->epid, epid, 8);
	n->pan = pan;
	n->channel = pick_ch;
	n->upd = 0;
	return start_mac(n);
}

static int bdb_job(struct zb_mac *m, void *arg)
{
	struct zb_net *n = arg;
	uint8_t mode = n->job.mode;
	bool restored = n->formed;
	int r = 0;

	ARG_UNUSED(m);
	if ((mode & ZB_BDB_FORMATION) != 0U) {
		report_bdb(n, ZB_BDB_IN_PROGRESS, ZB_BDB_FORMATION, BDB_NOTIFY_REMAINING);
		if (!restored) {
			set_state(n, ZB_DEV_COORD_STARTING);
			/* A stored network is brought up, never overwritten by a new one. */
			if (load_network(n)) {
				r = start_mac(n);
				restored = r == 0;
			} else {
				r = form_new(n);
			}
		}
		if (r == 0) {
			set_state(n, ZB_DEV_COORD);
			/* Z-Stack answers FormationFailure when the network already existed. */
			report_bdb(n, restored ? ZB_BDB_FORMATION_FAILURE : ZB_BDB_SUCCESS,
				   ZB_BDB_FORMATION, 0);
		} else {
			set_state(n, ZB_DEV_HOLD);
			report_bdb(n, ZB_BDB_FORMATION_FAILURE, ZB_BDB_FORMATION, 0);
		}
	}
	if ((mode & ZB_BDB_STEERING) != 0U) {
		if (n->formed) {
			(void)zb_net_permit(n, STEERING_SECONDS);
			report_bdb(n, ZB_BDB_SUCCESS, ZB_BDB_STEERING, 0);
		} else {
			report_bdb(n, 2 /* NoNetwork */, ZB_BDB_STEERING, 0);
		}
	}
	n->job.busy = false;
	return 0;
}

void zb_net_bdb_set_channel(struct zb_net *n, bool primary, uint32_t mask)
{
	if (primary) {
		n->bdb_primary = mask;
	} else {
		n->bdb_secondary = mask;
	}
}

int zb_net_bdb_start(struct zb_net *n, uint8_t mode)
{
	if (n->job.busy) {
		return -EBUSY;
	}
	n->job.busy = true;
	n->job.mode = mode;
	if (zb_mac_post(n->mac, bdb_job, n) != 0) {
		n->job.busy = false;
		return -EBUSY;
	}
	return 0;
}

int zb_net_startup(struct zb_net *n)
{
	if (n->formed) {
		set_state(n, ZB_DEV_COORD);
		return 0;
	}
	return 2;
}

int zb_net_permit(struct zb_net *n, uint8_t seconds)
{
	int r = zb_mac_permit(n->mac, seconds);

	if (r == 0 && n->cb.permit != NULL) {
		n->cb.permit(n->cb.ctx, seconds);
	}
	return r;
}

static int discover_job(struct zb_mac *m, void *arg)
{
	struct zb_net *n = arg;
	size_t nb = 0;
	int r = zb_mac_active_scan(m, n->job.mask, n->job.exp, n->job.beacons,
				   ARRAY_SIZE(n->job.beacons), &nb);

	for (size_t i = 0; i < nb && n->cb.beacons != NULL; i += BEACONS_PER_AREQ) {
		n->cb.beacons(n->cb.ctx, &n->job.beacons[i], MIN((size_t)BEACONS_PER_AREQ, nb - i));
	}
	if (n->cb.discovery_done != NULL) {
		n->cb.discovery_done(n->cb.ctx, r == 0 ? 0 : 1);
	}
	n->job.busy = false;
	return 0;
}

int zb_net_discover(struct zb_net *n, uint32_t mask, uint8_t exp)
{
	if (exp > ZB_MAC_SCAN_EXP_MAX) {
		return -EINVAL;
	}
	if (n->job.busy) {
		return -EBUSY;
	}
	n->job.busy = true;
	n->job.mask = mask & ZB_ALL_CHANNELS;
	n->job.exp = exp;
	if (zb_mac_post(n->mac, discover_job, n) != 0) {
		n->job.busy = false;
		return -EBUSY;
	}
	return 0;
}

static int energy_job(struct zb_mac *m, void *arg)
{
	struct zb_net *n = arg;
	int r = zb_mac_energy_scan(m, n->job.mask, n->job.exp, n->job.ed);

	if (n->cb.energy_done != NULL) {
		n->cb.energy_done(n->cb.ctx, r, n->job.mask, n->job.ed,
				  (size_t)popcount_mask(n->job.mask), n->job.tag);
	}
	n->job.busy = false;
	return 0;
}

int zb_net_energy_scan(struct zb_net *n, uint32_t mask, uint8_t exp, uint32_t tag)
{
	mask &= ZB_ALL_CHANNELS;
	if (mask == 0U || exp > ZB_MAC_SCAN_EXP_MAX) {
		return -EINVAL;
	}
	if (n->job.busy) {
		return -EBUSY;
	}
	n->job.busy = true;
	n->job.mask = mask;
	n->job.exp = exp;
	n->job.tag = tag;
	if (zb_mac_post(n->mac, energy_job, n) != 0) {
		n->job.busy = false;
		return -EBUSY;
	}
	return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* Channel change and the association table, for the host */

int zb_net_change_channel(struct zb_net *n, uint8_t channel)
{
	uint8_t upd;
	uint8_t mask[4];

	if (channel < 11U || channel > 26U) {
		return -EINVAL;
	}
	if (!n->formed) {
		return -ENOTCONN;
	}
	upd = (uint8_t)(n->upd + 1U);
	mt_put_u32(mask, BIT(channel));
	/* NV first (the NIB is the source of truth), then the radio. */
	if (znp_nv_write(n->nv, znp_nv_osal(ZB_NV_NIB), ZB_NIB_CHANNEL, &channel, 1) != 0 ||
	    znp_nv_write(n->nv, znp_nv_osal(ZB_NV_NIB), ZB_NIB_CHANNEL_LIST, mask, 4) != 0 ||
	    znp_nv_write(n->nv, znp_nv_osal(ZB_NV_NIB), ZB_NIB_UPDATE_ID, &upd, 1) != 0 ||
	    znp_nv_flush(n->nv) != 0) {
		return -EIO;
	}
	n->channel = channel;
	n->upd = upd;
	return start_mac(n);
}

int zb_net_set_channel_mask(struct zb_net *n, uint32_t mask, uint16_t manager)
{
	uint8_t m[4], a[2];

	if ((mask & ~ZB_ALL_CHANNELS) != 0U) {
		return -EINVAL;
	}
	if (!n->formed) {
		return -ENOTCONN;
	}
	mt_put_u32(m, mask);
	mt_put_u16(a, manager);
	if (znp_nv_write(n->nv, znp_nv_osal(ZB_NV_NIB), ZB_NIB_CHANNEL_LIST, m, 4) != 0 ||
	    znp_nv_write(n->nv, znp_nv_osal(ZB_NV_NIB), ZB_NIB_MANAGER_ADDR, a, 2) != 0) {
		return -EIO;
	}
	return 0;
}

int zb_net_child_add(struct zb_net *n, uint16_t nwk, const uint8_t ext[8], uint8_t cap)
{
	struct zb_child *c;
	int i;

	k_mutex_lock(&n->lock, K_FOREVER);
	c = zbn_child_by_ext(n, ext);
	i = c != NULL ? (int)(c - n->children) : free_slot(n);
	if (i >= 0) {
		n->children[i] = (struct zb_child){.used = true, .nwk = nwk, .cap = cap};
		memcpy(n->children[i].ext, ext, 8);
		zbn_child_persist(n, i);
		update_capacity(n);
	}
	k_mutex_unlock(&n->lock);
	return i;
}

int zb_net_child_remove(struct zb_net *n, const uint8_t ext[8])
{
	struct zb_child *c;
	int i = -1;

	k_mutex_lock(&n->lock, K_FOREVER);
	c = zbn_child_by_ext(n, ext);
	if (c != NULL) {
		i = (int)(c - n->children);
		memset(c, 0, sizeof(*c));
		zbn_child_persist(n, i);
		update_capacity(n);
	}
	k_mutex_unlock(&n->lock);
	return i;
}
