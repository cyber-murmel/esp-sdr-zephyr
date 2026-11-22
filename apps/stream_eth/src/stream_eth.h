/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The pieces of the stream over Ethernet: the radio and its ring consumer (radio.c), the sender
 * and the MAC (txpath.c), the commands and the subscriber (ctl.c).
 */

#ifndef STREAM_ETH_H_
#define STREAM_ETH_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>

#include "peer.h"
#include "vrt_cells.h"

/* radio.c */
struct radio_state {
	uint64_t freq_hz;
	int gain; /* -1 auto */
	uint32_t rate_hz;
	bool on;
	/** Settings epoch, see vrt_pkt.h */
	uint8_t epoch;
};

int radio_init(void);
/** The ring consumer: runs forever. */
void radio_run(void);
void radio_get(struct radio_state *s);
/** Each returns 0 or -EINVAL (not the radio's range). */
int radio_set_freq(uint64_t hz);
int radio_set_gain(int gain);
int radio_set_rate(uint32_t hz);
int radio_gain_max(void);
bool radio_rate_ok(uint32_t hz);
bool radio_freq_ok(uint64_t hz);
/** The settings epoch now (after the command that was just executed). */
uint8_t radio_epoch(void);
/** Is the gain automatic? */
bool radio_agc(void);
/** The stream runs while a subscriber is there. */
void radio_enable(bool on);
/** Frequency the LO really runs at, in Hz, or 0. */
uint64_t radio_center_hz(uint64_t want_hz);
uint32_t radio_ring_skips(void);
uint32_t radio_nodes(void);
/** Cycles the sink spends a node (copy, checksum, checks), since the last call. */
struct radio_prof {
	uint32_t node_total, node_n, node_max;
};
void radio_prof_take(struct radio_prof *p);
/** The last error of the tuning and of the ring (0: none). */
void radio_errors(int *tune, int *ring);
/** Bumped when the stream is replanned (frequency, gain, rate): a context packet is due. */
uint32_t radio_change_count(void);
/** Restart the ring (the packet size changed). */
void radio_replan(void);

extern struct vrt_cells cells;
/** The payload size packets get (set by txpath for the subscriber). */
uint32_t txpath_payload_bytes(void);

/* txpath.c */
struct tx_stats {
	uint32_t packets;
	uint64_t bytes;
	uint32_t context;
	uint32_t acks;
	uint32_t desc_waits;
	uint32_t errors;
	uint32_t underflow_base;
	/** cycles, summed since the last status: building frames, handing them to the MAC */
	uint32_t build_cycles, build_n, send_cycles, send_n;
};
int txpath_init(struct net_if *iface);
/** Subscribe (or renew) a peer. Returns true if the stream's destination changed. */
bool txpath_subscribe(const struct peer *p);
void txpath_unsubscribe(void);
bool txpath_active(void);
bool txpath_peer(struct peer *p);
/** The frame that answers a command to any peer (control frames; the MAC copies nothing). */
int txpath_send_ctl(const struct peer *to, const void *payload, size_t len);
void txpath_get_stats(struct tx_stats *s);
void txpath_set_mtu(unsigned int mtu);
unsigned int txpath_mtu(void);
bool txpath_v6(void);
bool txpath_zero_csum(void);
/** Wake the sender: new cells. */
void txpath_kick(void);
/** The sender noted a stop of the ring: rate changed etc. Starts a new context packet. */
void txpath_context_due(void);
/** Benchmark: returns 0 once the report is printed through the callback. */
struct bench_result {
	uint32_t pkt_bytes;
	uint32_t seconds;
	uint32_t packets;
	uint64_t cycles;
	uint32_t desc_waits;
	uint32_t late;
	uint32_t underflows;
	uint32_t cpu_free_pct;
	uint32_t cycles_per_sec;
};
int txpath_bench(uint32_t pkt_bytes, uint32_t seconds, uint32_t rate_pps,
		 struct bench_result *out);
void txpath_bench_abort(void);
const struct device *txpath_dev(void);
uint32_t txpath_underflows(void);
uint32_t txpath_cpu_free_pct(void);

/* ctl.c */
int ctl_init(struct net_if *iface);
struct ctl_stats {
	uint32_t commands, subscribes, bad, errors, dropped, rejected;
};
void ctl_get_stats(struct ctl_stats *o);
void ctl_run(void);
/** After a bench: the stream goes on if the subscriber is still the one of generation gen and not stopped. */
void ctl_resume(uint32_t gen);
uint32_t txpath_sub_gen(void);
void ctl_block(void);
void ctl_unblock(void);
bool ctl_blocked(void);

/** A VRT IF context packet for the receive stream, big endian, ts_valid for the time fields. */
int ctl_build_context(uint32_t *buf, size_t words, uint32_t rate, uint64_t freq_hz, int gain,
		      uint8_t epoch, uint32_t sec, uint64_t ps, bool changed, uint8_t count);

#endif /* STREAM_ETH_H_ */
