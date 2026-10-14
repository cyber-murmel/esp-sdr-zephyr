/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The coordinator's network layer, phase 2 part: NV-backed network state in Z-Stack's item
 * layout, the factory defaults hosts expect, formation and restore (what BDB commissioning
 * does), the child table and the association policy. NWK security and routing come later.
 */
#ifndef ZIGBEE_ZB_NET_H_
#define ZIGBEE_ZB_NET_H_

#include <zigbee/zb_frame.h>
#include <zigbee/zb_mac.h>
#include <zigbee/znp_nv.h>

/* Z-Stack OSAL NV item ids (zigpy-znp types/nvids.py, zigbee-herdsman constants/common.ts). */
enum zb_nv_id {
	ZB_NV_EXTADDR = 0x0001,
	ZB_NV_STARTUP_OPTION = 0x0003,
	ZB_NV_NIB = 0x0021,
	ZB_NV_EXTENDED_PAN_ID = 0x002D,
	ZB_NV_BCAST_DELIVERY_TIME = 0x0030,
	ZB_NV_CONCENTRATOR_ENABLE = 0x0032,
	ZB_NV_CONCENTRATOR_DISCOVERY = 0x0033,
	ZB_NV_CONCENTRATOR_RC = 0x0036,
	ZB_NV_SRC_RTG_EXPIRY_TIME = 0x0038,
	ZB_NV_NWK_ACTIVE_KEY_INFO = 0x003A,
	ZB_NV_NWK_ALTERN_KEY_INFO = 0x003B,
	ZB_NV_NWK_CHILD_AGE_ENABLE = 0x003E,
	ZB_NV_APS_USE_EXT_PANID = 0x0047,
	ZB_NV_APS_LINK_KEY_TABLE = 0x004C,
	ZB_NV_BDB_NODE_IS_ON_NETWORK = 0x0055,
	ZB_NV_MIGRATION_ID = 0x005F,
	ZB_NV_HAS_CONFIGURED_ZSTACK3 = 0x0060,
	ZB_NV_PRECFGKEY = 0x0062,
	ZB_NV_PRECFGKEYS_ENABLE = 0x0063,
	ZB_NV_NWKKEY = 0x0082,
	ZB_NV_PANID = 0x0083,
	ZB_NV_CHANLIST = 0x0084,
	ZB_NV_LOGICAL_TYPE = 0x0087,
	ZB_NV_ZDO_DIRECT_CB = 0x008F,
	ZB_NV_TCLK_SEED = 0x0101,
	/* Private to this firmware: the APS security frame counter, written ahead. */
	ZB_NV_APS_FRAME_COUNTER = 0x0F20,
};

/* Extended NV item ids (system ZNP_NV_SYS_ZSTACK): tables, one sub id per entry. */
enum zb_nv_ex {
	ZB_NVEX_ADDRMGR = 1,
	ZB_NVEX_TCLK_TABLE = 4,
	ZB_NVEX_APS_KEY_DATA = 6,
	ZB_NVEX_NWK_SEC_MATERIAL = 7,
};

#define ZB_NIB_LEN        116 /* Z-Stack 3.30, 32 bit aligned */
#define ZB_NWKKEY_LEN     24
#define ZB_ADDRMGR_LEN    12
#define ZB_TCLK_LEN       20
#define ZB_APS_KEY_LEN    24
#define ZB_SECMAT_LEN     12
#define ZB_KEYDESC_LEN    17 /* key sequence number, key[16] */
#define ZB_LINK_KEY_TABLE_ENTRIES 16

#define ZB_NET_MAX_CHILDREN 32
#define ZB_TABLE_ENTRIES    32 /* ADDRMGR, TCLK and APS key tables */
#define ZB_SECMAT_ENTRIES   4

#define ZB_ALL_CHANNELS 0x07FFF800U

/* NIB byte offsets (aligned layout, host-visible through NV item 0x0021). */
#define ZB_NIB_SECURITY_LEVEL   12
#define ZB_NIB_DEV_ADDR         22
#define ZB_NIB_CHANNEL          24
#define ZB_NIB_COORD_ADDR       26
#define ZB_NIB_COORD_EXT        28
#define ZB_NIB_PAN_ID           36
#define ZB_NIB_STATE            38
#define ZB_NIB_CHANNEL_LIST     40
#define ZB_NIB_EPID             57
#define ZB_NIB_KEY_LOADED       65
#define ZB_NIB_MANAGER_ADDR     110
#define ZB_NIB_UPDATE_ID        114

/* NWK state values in the NIB. */
#define ZB_NWK_INIT   0
#define ZB_NWK_ROUTER 8

/* ZDO device states (ZDO_STATE_CHANGE_IND, UTIL_GET_DEVICE_INFO). */
enum zb_dev_state {
	ZB_DEV_HOLD = 0,
	ZB_DEV_COORD_STARTING = 8,
	ZB_DEV_COORD = 9,
};

enum zb_bdb_mode {
	ZB_BDB_STEERING = 0x02,
	ZB_BDB_FORMATION = 0x04,
};

enum zb_bdb_status {
	ZB_BDB_SUCCESS = 0,
	ZB_BDB_IN_PROGRESS = 1,
	ZB_BDB_FORMATION_FAILURE = 8,
};

/* STARTUP_OPTION bits applied at a reset. */
#define ZB_STARTUP_CLEAR_CONFIG BIT(0)
#define ZB_STARTUP_CLEAR_STATE  BIT(1)

struct zb_child {
	bool used;
	/* True from the association until the response is ACKed. */
	bool tentative;
	/* The network key has been delivered: frames from it are accepted. */
	bool authorized;
	uint16_t nwk;
	uint8_t ext[8];
	uint8_t cap;
	/* Highest NWK frame counter seen from it (replay protection). */
	uint32_t rx_fc;
	int64_t last_seen;
};

/* An application frame received (APS payload of an authorized neighbour). */
struct zb_aps_rx {
	uint16_t src;
	uint16_t cluster, profile;
	uint8_t src_ep, dst_ep;
	bool broadcast;
	uint8_t lqi;
	uint8_t aps_counter;
	const uint8_t *data;
	uint8_t len;
};

/* An application frame to send. */
struct zb_aps_tx {
	/* NWK destination: a short address or one of the broadcast addresses. */
	uint16_t dst;
	bool broadcast;
	uint8_t src_ep, dst_ep;
	uint16_t cluster, profile;
	bool ack_req;
	/* Report the outcome through zb_net_cb.data_confirm (not for ZDO and broadcasts). */
	bool confirm;
	/* Handed back with the confirmation. */
	uint32_t tag;
	const uint8_t *data;
	size_t len;
};

/* Result codes of a confirmation (Z-Stack's). */
#define ZB_CONFIRM_OK            0x00
#define ZB_CONFIRM_FAILURE       0x01
#define ZB_CONFIRM_APS_NO_ACK    0xB7
#define ZB_CONFIRM_NO_ROUTE      0xCD
#define ZB_CONFIRM_CHANNEL_BUSY  0xE1
#define ZB_CONFIRM_MAC_NO_ACK    0xE9

/* Longest application payload with NWK security (no APS security). */
#define ZB_APS_MAX_PAYLOAD 82

/* What the net layer reports upwards; the callbacks run in the MAC thread. */
struct zb_net_cb {
	void (*state)(void *ctx, uint8_t dev_state);
	void (*bdb)(void *ctx, uint8_t status, uint8_t mode, uint8_t remaining);
	/* Beacons of an active scan asked by the host, then the confirmation (status 0). */
	void (*beacons)(void *ctx, const struct zb_beacon_info *b, size_t n);
	void (*discovery_done)(void *ctx, uint8_t status);
	/* Energy scan asked by the host: one ED byte per channel of @p mask. */
	void (*energy_done)(void *ctx, int status, uint32_t mask, const uint8_t *ed, size_t n,
			    uint32_t tag);
	void (*permit)(void *ctx, uint8_t duration);
	/* A device was given the network key (the trust center's join). */
	void (*tc_join)(void *ctx, uint16_t nwk, const uint8_t ext[8], uint16_t parent);
	/* An application frame came in. */
	void (*aps_rx)(void *ctx, const struct zb_aps_rx *rx);
	/* Outcome of a zb_net_send() with confirm set. */
	void (*data_confirm)(void *ctx, uint8_t status, uint32_t tag);
	/* A line of protocol trace (why a frame was dropped, what the trust center did). */
	void (*trace)(void *ctx, const char *msg);
	void *ctx;
};

#define ZB_APS_WAITS 6
#define ZB_APS_DUPS  8

struct zb_aps_wait {
	bool used;
	uint16_t dst;
	uint8_t counter;
	uint32_t tag;
	int64_t deadline;
};

struct zb_aps_dup {
	uint16_t src;
	uint8_t counter;
	int64_t until;
};

struct zb_net_job {
	bool busy;
	uint32_t mask;
	uint8_t exp;
	uint8_t mode;
	uint32_t tag;
	uint8_t ed[16];
	struct zb_beacon_info beacons[16];
};

struct zb_net {
	struct zb_mac *mac;
	struct znp_nv *nv;
	struct zb_net_cb cb;
	struct k_mutex lock;

	uint8_t eui[8];
	uint8_t dev_state;
	bool formed;
	uint16_t pan;
	uint8_t channel;
	uint8_t epid[8];
	uint8_t upd;
	uint32_t bdb_primary, bdb_secondary;
	struct zb_child children[ZB_NET_MAX_CHILDREN];
	struct zb_net_job job;

	/* Network security: the active key and the frame counters (written ahead to flash). */
	bool keys_ok;
	uint8_t nwk_key[ZB_KEY_LEN];
	uint8_t nwk_key_seq;
	uint8_t tc_link_key[ZB_KEY_LEN];
	uint32_t nwk_fc, nwk_fc_limit;
	uint32_t aps_fc, aps_fc_limit;
	uint8_t nwk_seq, aps_counter;
	int64_t next_link_status;
	struct zb_aps_wait waits[ZB_APS_WAITS];
	struct zb_aps_dup dups[ZB_APS_DUPS];
};

/* The MAC callbacks of the net layer (association policy); pass to zb_mac_init(). */
void zb_net_mac_up(struct zb_net *n, struct zb_mac_up *up);

void zb_net_init(struct zb_net *n, struct zb_mac *mac, struct znp_nv *nv,
		 const struct zb_net_cb *cb);

/*
 * Boot the network layer from NV (a power up or a reset): apply STARTUP_OPTION, make sure the
 * factory items exist, load the identity and children, and bring a formed network up.
 */
int zb_net_boot(struct zb_net *n, const uint8_t default_eui[8]);

/* The factory default NV items; @p reset_config replaces the config items that exist. */
void zb_nvdef_ensure(struct znp_nv *nv, const uint8_t eui[8], bool reset_config);
/* ClearState: forget the network (NIB, keys, children, tables) but keep identity and counters. */
void zb_nvdef_clear_state(struct znp_nv *nv);

/* BDB: the channel masks formation chooses from, and starting a commissioning mode. */
void zb_net_bdb_set_channel(struct zb_net *n, bool primary, uint32_t mask);
/* Returns 0 when the mode is accepted; progress and result follow as callbacks. */
int zb_net_bdb_start(struct zb_net *n, uint8_t mode);

/* Resume a formed network without BDB (ZDO_STARTUP_FROM_APP): 0 restored, 1 new, 2 not started. */
int zb_net_startup(struct zb_net *n);

/* Association permit: 0 closes, 0xFF stays open, otherwise seconds. */
int zb_net_permit(struct zb_net *n, uint8_t seconds);

/* Asynchronous scans for the host; one at a time (-EBUSY). Results come through zb_net_cb. */
int zb_net_discover(struct zb_net *n, uint32_t mask, uint8_t exp);
/* @p tag comes back with the result: the caller's way to tell scans apart. */
int zb_net_energy_scan(struct zb_net *n, uint32_t mask, uint8_t exp, uint32_t tag);

/*
 * Move the formed network to @p channel (ZDO Mgmt_NWK_Update, ScanDuration 0xFE): the NIB gets
 * the channel and an incremented network update id, then the MAC restarts. -EINVAL, or
 * -ENOTCONN without a network.
 */
int zb_net_change_channel(struct zb_net *n, uint8_t channel);
/* ScanDuration 0xFF: the channel mask and network manager address in the NIB. */
int zb_net_set_channel_mask(struct zb_net *n, uint32_t mask, uint16_t manager);

/*
 * Send an application frame to a neighbour (or everyone): NWK secured, no routing yet. Returns
 * 0 once accepted, with the outcome (MAC and APS acknowledgements) through data_confirm;
 * -EMSGSIZE, -ENOTCONN.
 */
int zb_net_send(struct zb_net *n, const struct zb_aps_tx *tx);

/* Association table for the host's UTIL_ASSOC_* commands; slot index or -1. */
int zb_net_child_add(struct zb_net *n, uint16_t nwk, const uint8_t ext[8], uint8_t cap);
int zb_net_child_remove(struct zb_net *n, const uint8_t ext[8]);

/* Child table. */
struct zb_child *zb_net_child_by_ext(struct zb_net *n, const uint8_t ext[8]);
struct zb_child *zb_net_child_by_nwk(struct zb_net *n, uint16_t nwk);
size_t zb_net_child_count(struct zb_net *n);

/* The IEEE address in use (NV 0x0001, set by the host or the factory default). */
const uint8_t *zb_net_eui(const struct zb_net *n);

#endif /* ZIGBEE_ZB_NET_H_ */
