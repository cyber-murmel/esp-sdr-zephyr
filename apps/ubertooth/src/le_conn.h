/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * BLE connection following as a pure state machine (no Zephyr, no radio): a port of the
 * logic of the Ubertooth firmware's le_phy.c (CONNECT_REQ parsing, connection events,
 * anchor tracking, connection update and channel map instants, supervision timeout) with
 * three differences: it plans around the 3 to 4 ms retune of the SDR (events it cannot
 * reach are skipped, the channel sequence still advances), it also does channel selection
 * algorithm #2, and it keeps the window widening honest instead of a fixed 3 us epsilon.
 *
 * Events in: packets from the finder (ubt_le.h) and the passage of time. Actions out: which
 * channel to be on, from when, until when. All times are 64 bit counts of 100 ns (the glue
 * unwraps clk100ns, see ubt_clk_unwrap()).
 *
 * Glue contract:
 *  - feed every emitted LE packet to le_conn_packet() (with the dewhitened PDU, no AA, no
 *    CRC; crc_ok from struct ubt_le_info) and call le_conn_tick(now) when t_end of the last
 *    action has passed AND the finder is idle (ubt_le_busy() false);
 *  - on LE_ACT_TUNE: start the retune at t_retune (not earlier), be receiving by t_listen,
 *    point the finder at the action's AA, CRCInit and channel (ubt_le_set_aa/crc_init/channel;
 *    crc_verify is not needed, the state machine only trusts packets with crc_ok);
 *  - on LE_ACT_WAIT: same channel, only the deadline t_end moved (more packets expected);
 *  - on LE_ACT_ADV: back to the advertising channel with the advertising AA and CRCInit.
 *
 * The logic is ported from the Project Ubertooth firmware (bluetooth_rxtx.c, Copyright
 * 2010-2013 Michael Ossmann and 2011-2013 Dominic Spill; le_phy.c, Copyright 2017 Mike Ryan),
 * released under the GPL version 2 or later.
 */

#ifndef LE_CONN_H_
#define LE_CONN_H_

#include <stdbool.h>
#include <stdint.h>

#include "ubt_le.h"

/* Units of the time base (100 ns). */
#define LE_TICKS_PER_US   10U
#define LE_TICKS_PER_UNIT 12500U  /* 1.25 ms: interval, window offset and size */
#define LE_TICKS_PER_10MS 100000U /* supervision timeout unit */
#define LE_T_IFS_TICKS    1500U   /* inter frame space, 150 us */
/* Air time of a packet with an n byte payload: preamble, AA, header, payload, CRC. */
#define LE_PKT_TICKS(n)   ((80U + 8U * (uint32_t)(n)) * LE_TICKS_PER_US)

enum le_conn_state {
	LE_CONN_ADV = 0, /* no connection followed, waiting for a CONNECT_REQ */
	LE_CONN_FIRST,   /* connection requested, anchor not yet seen */
	LE_CONN_TRACK,   /* anchor known, following events */
};

enum le_conn_act {
	LE_ACT_NONE = 0,
	LE_ACT_TUNE, /* go to mhz for the planned event (also when the channel is the same) */
	LE_ACT_WAIT, /* stay, the deadline moved */
	LE_ACT_ADV,  /* connection over (see reason) or cancelled */
};

enum le_conn_reason {
	LE_REASON_NONE = 0,
	LE_REASON_CANCEL,     /* le_conn_cancel() (UBERTOOTH_CANCEL_FOLLOW) */
	LE_REASON_TIMEOUT,    /* supervision timeout, or no packet in the first 6 events */
	LE_REASON_TERMINATE,  /* LL_TERMINATE_IND seen */
	LE_REASON_TOO_FAST,   /* more than cfg.max_skip events would have to be skipped */
	LE_REASON_BAD_PARAMS, /* CONNECT_REQ with parameters the spec forbids */
	LE_REASON_PHY,        /* LL_PHY_UPDATE_IND to a PHY other than LE 1M took effect */
};

struct le_conn_cfg {
	/* start of the retune to receiving, incl. ring restart (3 to 4 ms: 40000) */
	uint32_t retune_ticks;
	/* listen this long before the earliest possible preamble */
	uint32_t pre_ticks;
	/* fixed part of the window widening, each side */
	uint32_t jitter_ticks;
	/* t_end lies this long after the latest possible packet start ... */
	uint32_t detect_ticks;
	/* ... plus this long, to still catch the answer when the master's packet was lost */
	uint32_t slave_ticks;
	/* extra wait for the answer packet of an event */
	uint32_t ifs_slack;
	/* extra early margin while the anchor is a single unverified packet */
	uint32_t guard_ticks;
	/* supervision timeout assumed for a connection found in promiscuous mode */
	uint32_t promisc_timeout;
	/* our clock error, added to the master's sleep clock accuracy */
	uint16_t own_ppm;
	/* channel to return to */
	uint16_t adv_mhz;
	/* give up when more events than this must be skipped in a row */
	uint8_t max_skip;
	/* only follow a CONNECT_REQ whose CRC is good */
	bool connect_crc;
};

/* A packet of the finder: pdu = dewhitened header, length and payload, len = valid bytes. */
struct le_conn_pkt {
	uint16_t mhz;
	uint64_t t; /* packet start (first preamble bit), 100 ns */
	const uint8_t *pdu;
	uint16_t len;
	bool crc_ok;
};

/*
 * The packet as the finder emitted it: pk (an LE_PACKET), info (ubt_le's side information,
 * valid inside the emit callback) and the clk100ns unwrapper of the glue.
 */
static inline struct le_conn_pkt le_conn_pkt_from_usb(const usb_pkt_rx *pk,
						      const struct ubt_le_info *info,
						      struct ubt_clk_unwrap *unwrap)
{
	struct le_conn_pkt p;

	p.mhz = (uint16_t)(pk->channel + 2402U);
	p.t = ubt_clk_unwrap(unwrap, pk->clk100ns);
	p.pdu = &pk->data[4];
	p.len = info->pdu_len < UBT_LE_USB_BODY ? info->pdu_len : (uint16_t)UBT_LE_USB_BODY;
	p.crc_ok = info->crc_ok;
	return p;
}

struct le_conn_action {
	uint8_t act;    /* enum le_conn_act */
	uint8_t reason; /* ADV: enum le_conn_reason */
	uint16_t mhz;   /* TUNE, WAIT, ADV: frequency */
	uint32_t aa;    /* access address to sync on */
	uint32_t crc_init;
	uint32_t event;   /* TUNE: connection event counter of the planned event (16 bit) */
	uint32_t skipped; /* TUNE: events passed over since the last planned one */
	uint64_t t_retune;
	uint64_t t_listen;
	uint64_t t_end;
};

struct le_conn_stats {
	uint32_t connects;    /* CONNECT_REQs followed */
	uint32_t planned;     /* events the radio was sent to */
	uint32_t followed;    /* of those, with at least one good packet */
	uint32_t missed;      /* planned events without a good packet */
	uint32_t skipped;     /* events passed over because the retune could not make it */
	uint32_t packets;     /* good packets used */
	uint32_t bad_crc;     /* packets ignored for their CRC */
	uint32_t updates;     /* connection updates applied */
	uint32_t chm_updates; /* channel map updates applied */
	uint32_t stale;       /* packets on a channel other than the planned one */
};

struct le_conn_chm {
	uint8_t map[5];
	uint8_t n; /* channels in use */
};

struct le_conn_ev_pkt {
	uint64_t t;
	uint64_t end;
	bool md;
};

struct le_conn {
	struct le_conn_cfg cfg;
	uint8_t state;
	bool csa2;
	bool anchored;      /* the anchor of the planned event is based on an observation */
	bool verified;      /* the anchor is known to be the master's packet */
	bool counter_known; /* ev is the real connEventCounter (not after promiscuous hand over) */
	bool terminating;
	bool encrypted;
	bool phy_pending;
	bool phy_lost;
	uint16_t phy_instant;
	uint8_t hop;
	uint8_t unmapped0; /* CSA#1: unmapped channel of event ev0 */
	uint32_t ev0;
	uint32_t aa;
	uint32_t crc_init;
	uint32_t ppm;
	uint64_t interval;
	uint64_t timeout;
	uint64_t spread; /* width of the window the anchor lies in (first event, update) */
	struct le_conn_chm chm;
	struct le_conn_chm chm_new;
	bool chm_pending;
	uint16_t chm_instant;
	bool upd_pending;
	uint16_t upd_instant;
	uint64_t upd_interval, upd_offset, upd_size, upd_timeout;

	uint32_t ev;     /* planned event */
	uint64_t anchor; /* predicted anchor of ev */
	uint64_t ref_t;  /* last time the master's timing was observed */
	uint64_t last_valid;
	uint64_t first_deadline;
	uint64_t radio_free;
	uint64_t deadline;
	uint16_t cur_mhz;
	uint8_t npk;
	struct le_conn_ev_pkt pk[4];
	struct le_conn_stats stats;
};

void le_conn_cfg_default(struct le_conn_cfg *cfg);
void le_conn_init(struct le_conn *c, const struct le_conn_cfg *cfg);

/*
 * A packet. In LE_CONN_ADV only a CONNECT_REQ matters (the target filter is the finder's):
 * the result is the plan for the first event. Otherwise it is a packet of the planned event.
 * now >= the end of the packet.
 */
struct le_conn_action le_conn_packet(struct le_conn *c, const struct le_conn_pkt *p, uint64_t now);
/* Time passed: closes the event when its deadline is over. */
struct le_conn_action le_conn_tick(struct le_conn *c, uint64_t now);
/* UBERTOOTH_CANCEL_FOLLOW: stop following. */
struct le_conn_action le_conn_cancel(struct le_conn *c);

/* A connection found by promiscuous mode: parameters and the packet that completed it. */
struct le_conn_known {
	uint32_t aa;
	uint32_t crc_init;
	uint16_t interval; /* 1.25 ms units */
	uint8_t hop;
	uint8_t chan_idx; /* channel index of the packet at t */
	uint64_t t;       /* start of that packet (master or slave) */
};
struct le_conn_action le_conn_start_known(struct le_conn *c, const struct le_conn_known *k,
					  uint64_t now);

/* The hand over from promiscuous mode, once pm->phase is UBT_PM_DONE. */
static inline struct le_conn_known le_conn_known_from_promisc(const struct ubt_promisc *pm)
{
	struct le_conn_known k = {pm->aa,  pm->crc_init, pm->interval,
				  pm->hop, pm->last_idx, pm->t_last_pkt};

	return k;
}

static inline bool le_conn_following(const struct le_conn *c)
{
	return c->state != LE_CONN_ADV;
}

/* Channel index (0..36) of connection event ev under the current map. */
unsigned int le_conn_chan_idx(const struct le_conn *c, uint32_t ev);
/* CSA #2 pseudo random number of an event counter (Core Vol 6 Part B 4.5.8.3). */
uint16_t le_conn_csa2_prn(uint16_t counter, uint32_t aa);

#endif /* LE_CONN_H_ */
