/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The chain: hook, sender, burst timer, counters, the exchange ring. See ../README.rst.
 */
#ifndef CHAIN_H_
#define CHAIN_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/net/net_if.h>

#include "chain_math.h"

#define CHAIN_DIGITAL IS_ENABLED(CONFIG_PTP_CLOCK_DWC_MAC_DIGITAL_ROLLOVER)

enum chain_role {
	CHAIN_IDLE,  /* answers, never starts */
	CHAIN_SEME,  /* starts bursts */
	CHAIN_UKE,   /* has seen a seme */
};

/* Written by the interrupt and the burst timer (CPU 0), read by the thread: only ever counting up */
struct chain_counters {
	uint32_t tx_sent;
	uint32_t tx_busy;           /* slot still queued: ring overflow, frame not sent */
	uint32_t tx_no_peer;        /* no peer MAC known: frame not sent */
	uint32_t tx_fail;           /* the driver took no frame */
	uint32_t tx_stamp_missing;
	uint32_t rx_chain;
	uint32_t rx_nostamp;
	uint32_t rx_stale;
	uint32_t rx_lost;           /* peer frames missing by sequence gap */
	uint32_t exchanges;
	uint32_t ring_overflow;     /* exchange ring full */
	uint32_t reentry;
	uint32_t bursts;
	uint32_t chain_end;         /* bursts that ended here (hop count 0) */
	uint32_t burst_skipped;     /* timer ticks skipped: the previous burst was still running */
	uint32_t rejected;          /* exchanges with an implausible delay (stats thread) */
};

extern struct chain_counters chain_cnt;
extern const struct device *chain_eth;

int chain_init(const struct device *eth, struct net_if *iface);
/** Start bursts. Returns 0, or -ENOTCONN when no peer MAC is known (chain_peer_set() first). */
int chain_start(uint32_t interval_us, uint32_t rounds);
/** Peer MAC (destination of our frames), learned from the first chain frame or set by the user.
 *  Set and clear return -EBUSY unless the role is CHAIN_IDLE.
 */
int chain_peer_set(const uint8_t mac[6]);
int chain_peer_clear(void);
bool chain_peer_get(uint8_t mac[6]);
void chain_stop(void);
enum chain_role chain_role_get(void);
const uint8_t *chain_mac(void);
uint32_t chain_interval_us(void);
uint32_t chain_rounds(void);

/** Next exchange of the ring (thread side). */
bool chain_ring_pop(struct chain_xchg *x);

/* stats.c */
void chain_stats_init(void);
extern atomic_t chain_stat_on;
extern atomic_t chain_servo_on;
uint32_t chain_log_count(void);
bool chain_log_get(uint32_t idx, struct chain_xchg *x);
uint32_t chain_servo_steps(void);
/** Settings of the next 'servo on': period, loop bandwidth in mHz, step clamp in ns. */
void chain_servo_config(uint32_t period_ms, uint32_t bw_mhz, uint32_t clamp_ns);

/* The log entry that marks a servo step: t_ps[0] the step in ps */
#define CHAIN_LOG_STEP 0xffffffffU

/* pps.c */
int chain_pps_init(void);
int chain_pps_set(uint32_t n);
/** Flexible mode: a pulse of width_us every second, rising at a PTP second boundary. */
struct chain_pps_info {
	uint32_t tick_ps;        /* the unit of interval and width */
	uint32_t interval_ticks;
	uint32_t width_ticks;
	uint64_t start_sec;      /* the PTP second the first pulse rises at */
};

int chain_pps_flex(uint32_t width_us, bool ref_clk, struct chain_pps_info *info);
int chain_pps_flex_stop(void);

#endif /* CHAIN_H_ */
