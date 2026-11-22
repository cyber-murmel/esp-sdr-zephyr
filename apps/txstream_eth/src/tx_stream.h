/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The DAC stream: packets in, whole-group ownership flips out, on one thread pinned to
 * CPU 1, with the txs module (lib/esp_sdr/src/txs) doing the position model, the flip schedule, the
 * jitter cursor and the credits. CPU 0 puts packets in (tx_stream_put(): the receive hook, or the
 * injector in tx_inject.c) and reads the status.
 */

#ifndef TX_STREAM_H_
#define TX_STREAM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/net_buf.h>

#include "txs_credit.h"
#include "txs_expand.h"
#include "txs_wire.h"

#define TX_GROUP1 0x2F020000U
#define TX_GROUP2 0x2F040000U
/* The UDP port of the sample stream, and the session's stream ID (a random one comes with the protocol) */
#define TX_PORT 4994U
#define TX_STREAM_ID 0x5ab3c0deU
/* TX_HOLD, TX_BURST, TX_DEPTH_PKTS and TX_BUFFERS: the buffers the stream holds, the host's burst and the credit depth */
#include "tx_hold.h"

/* Where the packets come from: the injector (tx_inject.c, a shell test) or the network (tx_net.c). */
enum tx_src { TX_SRC_INJECT, TX_SRC_NET };

struct tx_cfg {
	enum tx_src src;
	/* the kernel to use when `kern` fails its golden check at the start (NULL: the run fails) */
	const struct txs_kern *fallback;
	/* TX_SRC_NET: the session's stream ID, which the parser checks (the injector uses TX_STREAM_ID) */
	uint32_t stream_id;
	uint32_t msps, seconds;
	bool rf, verify;
	enum txs_fmt fmt;
	const struct txs_kern *kern;
	/* flip: cycles the ownership write takes (0: measured before the start, the mean), and words it may
	 * land late before it counts */
	uint32_t write_lat, tol;
	/* cycles a step costs besides its words (poll, fit, jit): the margins are made of it */
	uint32_t overhead;
};

#define TX_ERR_N 10U

/* The worst late flip: what the thread was doing before it (kinds: 1 fill step, 2 zeroing, 3 idle or
 * verify, 4 nothing to do; bit 8: the status or the statistics were published in that turn). */
struct tx_flip_ev {
	uint64_t flip;
	uint32_t ms, late;
	/* cycles past the open spin's start point (aim - 2 us) when the flip was entered, when the open spin
	 * ended, and past the aim when the lock was taken */
	int32_t entry, open, lock;
	uint32_t prev_kind, prev_cyc, prev_ago;
};

struct tx_stats {
	bool active, rf, dead, verify;
	int cpu_start, cpu_end, ret;
	const char *kern;
	/* the kernel asked for failed its golden check and the fallback ran */
	bool kern_fb;
	enum txs_fmt fmt;
	uint32_t msps, elapsed_ms, sys_hz, pass_cyc;
	/* what the flip scheduler runs with, in cycles and words */
	uint32_t cost, zcost, margin, lock_lead, chunk, overhead, write_lat, tol, depth;
	/* flip scheduler (txs_flip) */
	uint64_t flips, late_flips, slack_flips, early_flips, missed_flips, stale_words, late_worst,
		early_worst;
	int reason;
	/* the ownership write in cycles (least, mean, worst) and the spin's exit past the due time */
	uint32_t write_min, write_max, write_n, spin_worst;
	uint64_t write_sum;
	/* the write measured before the start (min, mean, max), the one used, and flips over 64 cycles late */
	uint32_t cal_min, cal_mean, cal_max, spin_over64;
	bool lat_auto;
	struct tx_flip_ev ev;
	/* jitter cursor (txs_jit) */
	uint64_t placed, lost, recovered, dup, late, far, zeroed, unfilled;
	uint32_t underruns;
	/* the first window starts at a multiple of 351: the words before it (not a loss), and the underruns by
	 * what was there at the close: packet words waiting, packets in the ring, nothing; the age of the
	 * oldest waiting packet in cycles when its window closed */
	uint64_t align_gap;
	uint32_t ur_pend, ur_ring, ur_none, ur_age;
	/* packets: taken from the ring and done, parse errors by txs_wire_err, the ring full */
	uint64_t pk_taken, pk_done, pk_flushed;
	uint32_t inflight_end;
	bool thread_ended;
	uint32_t pk_err[TX_ERR_N];
	/* cycles: all the fill steps, the kernel alone and the words it expanded, the worst step */
	uint64_t fill_cyc, kern_cyc, kern_words, zero_words;
	uint32_t worst_step, steps, worst_kern_call, loops;
	/* cycles by part: poll, parse and queue, fit, jitter cursor, zeroing, the status record */
	uint64_t c_poll, c_pull, c_fit, c_jit, c_zero, c_pub;
	/* the CPU counter against the systimer */
	uint64_t cpu_cyc, sys_cyc;
	/* the bank read back after it was written */
	uint64_t ver_words, ver_bad, ver_zero;
	/* the wrong words by what they look like: the window's previous content (stale: not written yet), and by place
	 * in the window (512 word steps, the last one the rest); the first three: index, offset in the window, got, want */
	uint64_t ver_stale;
	uint32_t ver_pos[8];
	uint32_t ver_ex[3][4];
	uint32_t ver_nex;
	struct txs_status status;
};

typedef void (*tx_done_t)(const struct tx_stats *s);

/** Prepare the groups, the chain (RF as asked), the injector, the deadman and the flip thread, in
 * that order; an error leaves everything off. */
int tx_stream_start(const struct tx_cfg *cfg, tx_done_t done);
/** Producer (CPU 0, one at a time): hand over the UDP payload at buf->data + off, len bytes. false: no
 * run, or the ring is full: the caller keeps the buffer (and its inflight count). */
bool tx_stream_put(struct net_buf *buf, uint16_t off, uint16_t len);
/** The status for the sender, as the stream last published it (retries inside). */
bool tx_stream_status(struct txs_status *st);
/** The same record as the 64 bytes of txs_status_put(), for a packet; false: retry. */
bool tx_stream_status_raw(uint8_t out[TXS_STATUS_BYTES]);
/** Counts the statuses published after a flip: a new value is a reason to send a status now. */
uint32_t tx_stream_flips(void);
/** The systimer cycle count (k_cycle_get_32()) when the stream published the record that follows the last flip. */
uint32_t tx_stream_flip_ts(void);
/** The engine plays (TX_SRC_NET: the first window is in and the engine started). */
bool tx_stream_ready(void);
/** DRAIN: zeros for a window, then engine and chain off; now: at once. TX_SRC_NET before the engine
 * started: the run is ended without playing. */
int tx_stream_stop(bool now);
/** Test: the flip thread stops flipping (the deadman must fire); lock: it spins with interrupts
 * locked (not with RF on). */
int tx_stream_hang(bool lock);
void tx_stream_stats(struct tx_stats *s);
bool tx_stream_active(void);
/** The wire parameters of the session (for the sender). */
const struct txs_wire_cfg *tx_stream_wire(void);
/** The injector's count of buffers in flight (for the statistics). */
void tx_stream_inflight_add(int d);
/** The control thread: the caller (the main thread) never returns. Chain on and off and the end of a
 * run happen here, on one thread. */
void tx_stream_ctl_run(void) FUNC_NORETURN;
/** Bytes of the flip thread's and the control thread's stack never used so far. */
void tx_stream_stacks(size_t *flip_unused, size_t *ctl_unused);
/** The flip thread's stack, for the boot checks. */
void *tx_stream_stack(size_t *size);

#endif
