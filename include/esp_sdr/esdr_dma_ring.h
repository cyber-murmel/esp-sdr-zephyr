/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file
 * @brief The bookkeeping of a DMA receive ring whose memory can be handed away in slabs.
 *
 * A ring of descriptors, one buffer each, is cut into slabs of consecutive nodes: the memory of
 * one slab is contiguous, so a consumer that needs a run of nodes (one USB transfer, one block
 * write) can take it as it is. esdr_dma_ring_swap() re-points a slab's descriptors at a spare slab
 * and returns the old memory: the consumer keeps it until it gives it back, the ring goes on in
 * the spare. No sample is copied.
 *
 * The unit knows nothing of a DMA's registers or descriptor layout: the owner of the descriptors
 * supplies set_buf(), the unit decides what goes where and who owns what. A slab is in exactly one
 * of three places: the ring (one position each), the free pool, or out (taken by
 * esdr_dma_ring_spare_take() or returned by esdr_dma_ring_swap(), not yet given back). Out is the
 * state of the memory a consumer holds, and of a spare between take and swap.
 *
 * Thread safety: the free pool is an atomic mask, so esdr_dma_ring_put() may come from any thread
 * or interrupt while the ring's owner takes and swaps. take, swap and relink are the owner's alone
 * (they run under one lock, or in one thread).
 */

#ifndef ESP_SDR_ESDR_DMA_RING_H_
#define ESP_SDR_ESDR_DMA_RING_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Most slabs, ring and spare together: the free pool is one atomic word. */
#define ESDR_DMA_RING_SLABS_MAX 16U

struct esdr_dma_ring_ops {
	/** Point the descriptor of ring node @p node at @p buf (its first byte, hole included). */
	void (*set_buf)(void *ctx, unsigned int node, uint8_t *buf);
	/** Make the descriptor writes visible to the DMA, before anything that relies on them. */
	void (*fence)(void *ctx);
};

struct esdr_dma_ring {
	const struct esdr_dma_ring_ops *ops;
	void *ctx;
	uint8_t *mem[ESDR_DMA_RING_SLABS_MAX];
	/** The slab at each ring position, positions 0 to n_ring - 1. */
	uint8_t ring[ESDR_DMA_RING_SLABS_MAX];
	uint32_t node_bytes, slab_bytes;
	uint16_t nodes, slab_nodes;
	uint8_t n_ring, n_slabs;
	/*
	 * Concurrency, field by field. free_mask: atomic read-modify-write only (sequentially consistent
	 * builtins), from any core or interrupt; bits are set by put() (the consumer's side) and
	 * cleared by spare_take() (the owner's), so a clear that finds its bit set is the owner's.
	 * ring[], mem[], the geometry and quar_mask/quar_stamp: the owner's alone (one thread under one
	 * lock, here the run's); mem[] and the geometry never change after init. put() on another core
	 * reads ring[] only to refuse a slab the ring owns, a slab the consumer holds is in no ring
	 * position whatever the order of the owner's concurrent writes to other positions, and a byte
	 * read is atomic.
	 */
	atomic_t free_mask;
	/** Slabs held back until the DMA is provably past them (owner's). */
	uint32_t quar_mask;
	uint64_t quar_stamp[ESDR_DMA_RING_SLABS_MAX];
};

/**
 * Set up the ring over @p ring_mem (@p nodes nodes of @p node_bytes, in order) and @p spares more
 * slabs at @p spare_mem (consecutive, may be NULL for none). The first call only: a second one
 * would forget who holds what.
 *
 * @return 0, or -EINVAL when @p nodes is not a whole number of slabs, or there are too many.
 */
int esdr_dma_ring_init(struct esdr_dma_ring *r, const struct esdr_dma_ring_ops *ops, void *ctx,
		       unsigned int nodes, unsigned int slab_nodes, uint32_t node_bytes,
		       uint8_t *ring_mem, uint8_t *spare_mem, unsigned int spares);

/** @return The address of ring node @p node, from the slab it is in now. */
uint8_t *esdr_dma_ring_node(const struct esdr_dma_ring *r, unsigned int node);

/** Point every descriptor at the ring's present slabs (a restart: the slabs held out stay out). */
void esdr_dma_ring_relink(const struct esdr_dma_ring *r);

/** @return A free slab, now out, or NULL when there is none. Nothing else changes. */
uint8_t *esdr_dma_ring_spare_take(struct esdr_dma_ring *r);

/**
 * Put @p spare, a slab out, into the ring where the slab of ring node @p first_node is, and
 * return that slab's memory, now out.
 *
 * Re-points the slab's descriptors from its oldest node to its newest and fences, so a DMA that
 * has not yet come to the slab sees all of them new. One that has (a consumer too slow) may still
 * write the old memory: the caller must prove from the ring's own progress that it had not. If it
 * cannot, esdr_dma_ring_put() takes the returned memory back.
 *
 * @param r The ring.
 * @param first_node A ring node, rounded down to the start of its slab.
 * @param spare What esdr_dma_ring_spare_take() gave.
 * @return The old slab, or NULL when @p spare is not out or @p first_node is out of range
 *         (nothing changes then).
 */
uint8_t *esdr_dma_ring_swap(struct esdr_dma_ring *r, unsigned int first_node, uint8_t *spare);

/**
 * Give a slab back to the free pool: a swap's old memory when it cannot be used, a taken spare
 * not swapped in, or what a consumer is done with. Any address inside the slab does.
 *
 * @return 0, -EINVAL for an address in no slab or the ring's own slab, -EALREADY when it is free,
 *         -EBUSY when it is held back (only the owner lets those go).
 */
int esdr_dma_ring_put(struct esdr_dma_ring *r, const uint8_t *addr);

/**
 * Whether a slab may still be swapped, by the ring's progress: the DMA takes a node's descriptor as
 * it begins the node before, i.e. when @p lead (bytes finished since the slab's first node began,
 * as the interrupt counts them, which can be a node or two behind the DMA) reaches @p nodes - 1
 * nodes; with @p margin nodes of slack the swap must have been seen at a lead of at most
 * @p nodes - @p margin nodes. A node is @p payload bytes of the count.
 */
static inline bool esdr_dma_ring_swap_safe(int64_t lead, unsigned int nodes, uint32_t payload,
					   unsigned int margin)
{
	return lead < (int64_t)(nodes - margin + 1U) * (int64_t)payload;
}

/**
 * Hold @p addr's slab (a swap's old memory) back: the DMA may still be writing it, so it must not
 * be a spare yet. It stays out of the pool until esdr_dma_ring_reap() or
 * esdr_dma_ring_quarantine_clear(). Owner's call.
 *
 * @param r The ring.
 * @param addr The slab: the memory esdr_dma_ring_swap() returned.
 * @param stamp A count that only grows, as the ring's byte count, for reap() to compare.
 * @return 0, -EINVAL when the slab is not out or already held back.
 */
int esdr_dma_ring_quarantine(struct esdr_dma_ring *r, const uint8_t *addr, uint64_t stamp);

/**
 * Release the held-back slabs whose stamp is @p horizon or more behind @p now.
 *
 * @return How many.
 */
unsigned int esdr_dma_ring_reap(struct esdr_dma_ring *r, uint64_t now, uint64_t horizon);

/**
 * Release every held-back slab: only with the DMA stopped, which is the one thing that proves
 * nothing is in flight.
 *
 * @return How many.
 */
unsigned int esdr_dma_ring_quarantine_clear(struct esdr_dma_ring *r);

/** @return Slabs held back. */
unsigned int esdr_dma_ring_quarantined(const struct esdr_dma_ring *r);

/** @return Free slabs. */
unsigned int esdr_dma_ring_free_count(const struct esdr_dma_ring *r);

/**
 * Self-check: every slab is in exactly one place, no ring position repeats.
 *
 * @return The slabs out, or -EFAULT when the books are wrong.
 */
int esdr_dma_ring_check(const struct esdr_dma_ring *r);

#ifdef __cplusplus
}
#endif

#endif /* ESP_SDR_ESDR_DMA_RING_H_ */
