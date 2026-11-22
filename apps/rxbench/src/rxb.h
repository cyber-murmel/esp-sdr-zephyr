/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RXB_H_
#define RXB_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/net/net_if.h>

#include "rxb_seq.h"

/* What the consumer counted (a snapshot gives the difference to the last start). */
struct rxb_totals {
	uint64_t pkts;
	uint64_t frame_bytes;
	uint64_t payload_bytes;
	uint32_t bad;		/* short payload or wrong pattern */
	struct rxb_seq_counts seq;
};

/* What the receive hook counted (interrupt side), since the last start. */
struct rxb_hook_counts {
	uint32_t taken;		/* frames put in the ring */
	uint32_t ip4, ip6;	/* of those, by IP version */
	uint32_t ring_full;	/* claimed and dropped: the consumer is behind */
	uint32_t idle;		/* claimed and dropped: not started */
};

struct rxb_snap {
	bool running;
	bool full_check;
	int64_t start_ms;
	struct rxb_totals t;
	struct rxb_hook_counts h;
	struct rxb_seq pos;	/* first, latest and next sequence number of the run */
	uint32_t ring_high;	/* most frames waiting in the ring since the last snapshot */
	uint32_t ring_slots;
	uint32_t consumer_us_per_pkt_x10;	/* consumer time per packet, tenths of a microsecond */
};

/*
 * Register the hook on the Ethernet device and start the consumer thread. Frames are taken for the
 * interface's current link address (read for each frame, so a changed MAC address follows).
 */
int rxb_init(const struct device *eth, struct net_if *iface);

/* Take the counters as the baseline and take frames; full_check reads every payload byte. */
void rxb_start(bool full_check);

/* Stop counting; the port stays claimed and its frames are dropped, not passed to the stack. */
void rxb_stop(void);

/* Totals, the hook's counts and the ring's high water mark (which it then resets). */
void rxb_snapshot(struct rxb_snap *s);

#endif /* RXB_H_ */
