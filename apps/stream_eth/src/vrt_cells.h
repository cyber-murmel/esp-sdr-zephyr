/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The staging ring between the ring consumer and the sender: one cell per packet, [payload][room
 * for the trailer]. The consumer copies the 4080 byte nodes in, cut at packet boundaries, so a
 * packet may span two nodes; the sender takes a finished cell, adds the headers in a buffer of
 * its own and hands both to the MAC, which reads the cell in place. A cell is the consumer's
 * again once the MAC says it is sent. Whole nodes go when there is no room: a gap, the sample
 * loss of the next packet. No Zephyr in here except the atomics, so it runs on the host.
 */

#ifndef STREAM_ETH_VRT_CELLS_H_
#define STREAM_ETH_VRT_CELLS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/atomic.h>

struct vrt_cell_meta {
	/** Sample index of the first sample. */
	uint64_t index;
	/** Samples were lost just before this packet. */
	bool loss;
	/** Payload bytes. */
	uint16_t bytes;
	/** Samples per second, centre frequency and gain index of what the packet holds. */
	uint32_t rate;
	uint64_t freq_hz;
	int16_t gain;
	/** Settings epoch of the run (vrt_pkt.h) and the number of the run: each start is one. */
	uint8_t epoch;
	uint32_t run;
	/** nf_sum() of the payload, when the cells sum while they copy (IPv6). */
	uint64_t sum;
	bool sum_valid;
};

struct vrt_cells {
	uint8_t *mem;
	struct vrt_cell_meta *meta;
	/** A power of two: the counters wrap at 2^32. */
	uint32_t ncells;
	/** Bytes between cells: at least a payload and the trailer. */
	uint32_t stride;
	/** Payload bytes of the cells being filled. */
	uint32_t pl;
	/** Sample rate, centre frequency and gain of what is being pushed. */
	uint32_t rate;
	uint64_t freq_hz;
	int16_t gain;
	uint8_t epoch;
	uint32_t run;
	/** Sum the payload words while copying (the UDP checksum over IPv6). */
	bool sum_on;
	/** Gaps: skipped nodes, nodes dropped for want of room. */
	uint32_t gaps;

	/* the consumer's */
	uint32_t wr;
	uint32_t off;
	uint32_t wr_c;
	uint32_t off_c;
	uint64_t next_index;
	bool have_next;
	bool loss_pending;
	bool loss_pending_c;
	uint32_t dropped_nodes;

	/** Cells finished and kept (the sender's to take), and cells it is done with. */
	atomic_t ready;
	atomic_t released;
};

void vrt_cells_init(struct vrt_cells *c, uint8_t *mem, struct vrt_cell_meta *meta,
		    uint32_t ncells, uint32_t stride);

/**
 * Start filling cells of `pl` payload bytes (a multiple of 4, below the stride). What was half
 * filled goes. `loss` marks the first packet as after a loss. Cells in flight are untouched.
 */
void vrt_cells_start(struct vrt_cells *c, uint32_t pl, uint32_t rate, uint64_t freq_hz, int gain,
		     uint8_t epoch, bool sum_on, bool loss);

/**
 * Copy `len` bytes (a multiple of 4) of samples whose first sample has this index.
 *
 * @return 0, -ENOSPC when the sender is too far behind: the bytes are dropped, the half filled
 *         cell goes and the next packet carries the loss.
 */
int vrt_cells_push(struct vrt_cells *c, const uint8_t *data, size_t len, uint64_t index);

/** The nodes pushed since the last commit are good: let the sender have the cells they finished. */
void vrt_cells_commit(struct vrt_cells *c);

/** The nodes pushed since the last commit are not (the ring wrote over them): forget them. */
void vrt_cells_abort(struct vrt_cells *c);

/** Cells the sender may take, counted from its own position `rd`. */
static inline uint32_t vrt_cells_avail(const struct vrt_cells *c, uint32_t rd)
{
	return (uint32_t)atomic_get(&c->ready) - rd;
}

static inline uint8_t *vrt_cell_ptr(const struct vrt_cells *c, uint32_t n)
{
	return c->mem + (size_t)(n & (c->ncells - 1U)) * c->stride;
}

static inline const struct vrt_cell_meta *vrt_cell_meta(const struct vrt_cells *c, uint32_t n)
{
	return &c->meta[n & (c->ncells - 1U)];
}

/** The MAC has sent the frame of cell n (in order). */
static inline void vrt_cells_done(struct vrt_cells *c, uint32_t n)
{
	atomic_set(&c->released, (atomic_val_t)(n + 1U));
}

#endif /* STREAM_ETH_VRT_CELLS_H_ */
