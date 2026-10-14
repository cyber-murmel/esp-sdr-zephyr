/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The coordinator's MAC: one thread that owns the radio. It answers beacon requests, runs
 * the association handshake and the indirect transmission that goes with it, and runs energy
 * and active scans. See tmp/zstack/phase2/mac_final.md for the frame rules it follows.
 */
#ifndef ZIGBEE_ZB_MAC_H_
#define ZIGBEE_ZB_MAC_H_

#include <zephyr/kernel.h>

#include <zigbee/zb_mac_frame.h>
#include <zigbee/zb_radio.h>

#define ZB_MAC_PENDING_MAX 8

/* macTransactionPersistenceTime: 0x01F4 * 15.36 ms. */
#define ZB_MAC_PERSISTENCE_MS 7680
#define ZB_MAC_MAX_RETRIES    3
/* Longest scan duration exponent: 15.36 ms * (2^14 + 1) is about 252 s per channel. */
#define ZB_MAC_SCAN_EXP_MAX   14

struct zb_mac;

/* Callbacks into the layer above; all run in the MAC thread and must not block long. */
struct zb_mac_up {
	/*
	 * Association request of @p ext with capability information @p cap while joining is
	 * permitted. Return an enum zb_assoc_status; on ZB_ASSOC_OK set *short_addr.
	 */
	uint8_t (*assoc_req)(void *ctx, const uint8_t ext[8], uint8_t cap, uint16_t *short_addr);
	/* The response to @p ext was ACKed (delivered) or expired. */
	void (*assoc_done)(void *ctx, const uint8_t ext[8], uint16_t short_addr, bool delivered);
	/* A data frame for this node; @p payload follows the header. */
	void (*data_ind)(void *ctx, const struct zb_mac_hdr *h, const uint8_t *payload, size_t n,
			 uint8_t lqi, int8_t rssi);
	/* Optional: called at least once a second, for timers of the layer above. */
	void (*tick)(void *ctx);
	/* Optional: every frame sent or received (no FCS). */
	void (*trace)(void *ctx, bool tx, const uint8_t *f, size_t n, uint8_t lqi, int8_t rssi);
	void *ctx;
};

struct zb_mac_rx_item {
	uint8_t n;
	uint8_t lqi;
	int8_t rssi;
	uint8_t f[ZB_MAC_MAX_FRAME];
};

struct zb_mac_job {
	int (*fn)(struct zb_mac *m, void *arg);
	void *arg;
	struct k_sem *done;
	int *ret;
};

struct zb_mac_event {
	bool is_job;
	union {
		struct zb_mac_rx_item rx;
		struct zb_mac_job job;
	};
};

struct zb_mac_pending {
	bool used;
	bool assoc;
	bool by_ext;
	uint8_t ext[8];
	uint16_t short_addr;
	uint8_t f[ZB_MAC_MAX_FRAME];
	uint8_t n;
	int64_t expiry;
};

struct zb_mac {
	const struct zb_radio_ops *radio;
	void *rctx;
	struct zb_mac_up up;
	uint8_t eui[8];

	/* The PAN the coordinator runs (valid while started). */
	bool started;
	uint16_t pan;
	uint8_t channel;
	struct zb_beacon_info bcn;
	int64_t permit_until; /* uptime ms; 0 closed, INT64_MAX forever */
	uint8_t dsn, bsn;
	int8_t txpower;
	bool txpower_set;

	/* Set from before a scan reprograms the radio until the operating setup is back. */
	atomic_t scanning;
	struct zb_mac_pending pending[ZB_MAC_PENDING_MAX];

	struct k_msgq q;
	struct k_msgq scanq;
	char qbuf[16 * sizeof(struct zb_mac_event)];
	char scanbuf[16 * sizeof(struct zb_mac_rx_item)];
	struct k_thread thread;
	k_tid_t tid;

	uint32_t rx_dropped, tx_fail;
};

/* Set up; the radio is not touched until zb_mac_run(). */
void zb_mac_init(struct zb_mac *m, const struct zb_radio_ops *radio, void *rctx,
		 const struct zb_mac_up *up, const uint8_t eui[8]);

/* Start the MAC thread (stack of @p stack_size bytes, defined by the caller). */
void zb_mac_run(struct zb_mac *m, k_thread_stack_t *stack, size_t stack_size, int prio);

/* The radio's receive path calls this for each frame (no FCS); any thread, never blocks. */
void zb_mac_rx(struct zb_mac *m, const uint8_t *f, uint8_t n, uint8_t lqi, int8_t rssi);

/* Run @p fn in the MAC thread and wait for it (directly when called from it). */
int zb_mac_call(struct zb_mac *m, int (*fn)(struct zb_mac *m, void *arg), void *arg);

/* Queue @p fn for the MAC thread without waiting (@p arg must stay valid). */
int zb_mac_post(struct zb_mac *m, int (*fn)(struct zb_mac *m, void *arg), void *arg);

/*
 * Energy scan: for each channel in @p mask (ascending), the maximum energy over the scan
 * duration 15.36 ms * (2^exp + 1), as 0..255 into @p ed (one byte per channel). @p exp is
 * 0..ZB_MAC_SCAN_EXP_MAX (-EINVAL above).
 */
int zb_mac_energy_scan(struct zb_mac *m, uint32_t mask, uint8_t exp, uint8_t *ed);

/* Active scan: beacons seen on the channels of @p mask; *count of at most @p max. */
int zb_mac_active_scan(struct zb_mac *m, uint32_t mask, uint8_t exp, struct zb_beacon_info *out,
		       size_t max, size_t *count);

/*
 * Become the PAN coordinator of @p pan on @p channel (short address 0x0000): program the radio
 * and answer beacon requests with @p bcn (permit join closed until zb_mac_permit()).
 */
int zb_mac_start(struct zb_mac *m, uint16_t pan, uint8_t channel, const struct zb_beacon_info *bcn);
int zb_mac_stop(struct zb_mac *m);

/* Association permit: 0 closes, 0xFF stays open, otherwise seconds. */
int zb_mac_permit(struct zb_mac *m, uint8_t seconds);
bool zb_mac_permit_open(struct zb_mac *m);

/* Router and end device capacity bits of the beacon. */
int zb_mac_set_capacity(struct zb_mac *m, bool router_cap, bool ed_cap);

/* Beacon fields that follow the NIB: extended PAN ID and network update id. */
int zb_mac_set_network(struct zb_mac *m, const uint8_t epid[8], uint8_t upd);

/* Transmit power in dBm, applied at the next radio start and now (a running network restarts). */
int zb_mac_set_txpower(struct zb_mac *m, int8_t dbm);

/* The extended address in use (while stopped; the host may change it with a reset). */
int zb_mac_set_eui(struct zb_mac *m, const uint8_t eui[8]);

/* Send a data frame to a short address with MAC retries; 0, -ENOMSG (no ACK), -EAGAIN. */
int zb_mac_send_data(struct zb_mac *m, uint16_t dst_short, const uint8_t *payload, size_t n,
		     bool ar);

#endif /* ZIGBEE_ZB_MAC_H_ */
