/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The slab bookkeeping of a DMA receive ring, see esdr_dma_ring.h. No hardware in here: the
 * descriptors are the caller's, through set_buf().
 */

#include <errno.h>
#include <stdbool.h>

#include <esp_sdr/esdr_dma_ring.h>

static inline atomic_val_t bit(unsigned int id)
{
	return (atomic_val_t)1 << id;
}

static int slab_of(const struct esdr_dma_ring *r, const uint8_t *addr)
{
	for (unsigned int id = 0; id < r->n_slabs; id++) {
		if (addr >= r->mem[id] && addr < r->mem[id] + r->slab_bytes) {
			return (int)id;
		}
	}
	return -1;
}

static bool in_ring(const struct esdr_dma_ring *r, unsigned int id)
{
	for (unsigned int pos = 0; pos < r->n_ring; pos++) {
		if (r->ring[pos] == id) {
			return true;
		}
	}
	return false;
}

int esdr_dma_ring_init(struct esdr_dma_ring *r, const struct esdr_dma_ring_ops *ops, void *ctx,
		       unsigned int nodes, unsigned int slab_nodes, uint32_t node_bytes,
		       uint8_t *ring_mem, uint8_t *spare_mem, unsigned int spares)
{
	unsigned int n_ring;

	if (r == NULL || ops == NULL || ops->set_buf == NULL || slab_nodes == 0U ||
	    nodes % slab_nodes != 0U || ring_mem == NULL || node_bytes == 0U ||
	    (spares != 0U && spare_mem == NULL)) {
		return -EINVAL;
	}
	n_ring = nodes / slab_nodes;
	if (n_ring + spares > ESDR_DMA_RING_SLABS_MAX) {
		return -EINVAL;
	}
	*r = (struct esdr_dma_ring){
		.ops = ops,
		.ctx = ctx,
		.node_bytes = node_bytes,
		.slab_bytes = slab_nodes * node_bytes,
		.nodes = (uint16_t)nodes,
		.slab_nodes = (uint16_t)slab_nodes,
		.n_ring = (uint8_t)n_ring,
		.n_slabs = (uint8_t)(n_ring + spares),
	};
	for (unsigned int id = 0; id < r->n_slabs; id++) {
		r->mem[id] = id < n_ring ? ring_mem + (size_t)id * r->slab_bytes :
					   spare_mem + (size_t)(id - n_ring) * r->slab_bytes;
		if (id < n_ring) {
			r->ring[id] = (uint8_t)id;
		} else {
			atomic_or(&r->free_mask, bit(id));
		}
	}
	return 0;
}

uint8_t *esdr_dma_ring_node(const struct esdr_dma_ring *r, unsigned int node)
{
	return r->mem[r->ring[node / r->slab_nodes]] + (size_t)(node % r->slab_nodes) * r->node_bytes;
}

void esdr_dma_ring_relink(const struct esdr_dma_ring *r)
{
	for (unsigned int n = 0; n < r->nodes; n++) {
		r->ops->set_buf(r->ctx, n, esdr_dma_ring_node(r, n));
	}
	if (r->ops->fence != NULL) {
		r->ops->fence(r->ctx);
	}
}

uint8_t *esdr_dma_ring_spare_take(struct esdr_dma_ring *r)
{
	for (;;) {
		atomic_val_t m = atomic_get(&r->free_mask);

		if (m == 0) {
			return NULL;
		}
		for (unsigned int id = 0; id < r->n_slabs; id++) {
			/* put() only ever adds bits: a clear that finds the bit set is ours. */
			if ((m & bit(id)) != 0 && (atomic_and(&r->free_mask, ~bit(id)) & bit(id)) != 0) {
				return r->mem[id];
			}
		}
	}
}

uint8_t *esdr_dma_ring_swap(struct esdr_dma_ring *r, unsigned int first_node, uint8_t *spare)
{
	unsigned int pos, base;
	int id;
	uint8_t *old;

	if (first_node >= r->nodes || spare == NULL) {
		return NULL;
	}
	id = slab_of(r, spare);
	/* A slab out is not free and not in the ring, and the spare is a slab's start. */
	if (id < 0 || spare != r->mem[id] || (r->quar_mask & (1U << id)) != 0U || (atomic_get(&r->free_mask) & bit((unsigned int)id)) != 0 ||
	    in_ring(r, (unsigned int)id)) {
		return NULL;
	}
	pos = first_node / r->slab_nodes;
	base = pos * r->slab_nodes;
	old = r->mem[r->ring[pos]];
	/* The oldest node first: the DMA, if it comes at all, comes to that one first. */
	for (unsigned int k = 0; k < r->slab_nodes; k++) {
		r->ops->set_buf(r->ctx, base + k, spare + (size_t)k * r->node_bytes);
	}
	if (r->ops->fence != NULL) {
		r->ops->fence(r->ctx);
	}
	r->ring[pos] = (uint8_t)id;
	return old;
}

int esdr_dma_ring_put(struct esdr_dma_ring *r, const uint8_t *addr)
{
	int id = slab_of(r, addr);

	if (id < 0 || in_ring(r, (unsigned int)id)) {
		return -EINVAL;
	}
	if ((r->quar_mask & (1U << id)) != 0U) {
		return -EBUSY;
	}
	if ((atomic_or(&r->free_mask, bit((unsigned int)id)) & bit((unsigned int)id)) != 0) {
		return -EALREADY;
	}
	return 0;
}

int esdr_dma_ring_quarantine(struct esdr_dma_ring *r, const uint8_t *addr, uint64_t stamp)
{
	int id = slab_of(r, addr);

	if (id < 0 || in_ring(r, (unsigned int)id) ||
	    (atomic_get(&r->free_mask) & bit((unsigned int)id)) != 0 ||
	    (r->quar_mask & (1U << id)) != 0U) {
		return -EINVAL;
	}
	r->quar_mask |= 1U << id;
	r->quar_stamp[id] = stamp;
	return 0;
}

static unsigned int quar_release(struct esdr_dma_ring *r, uint64_t now, uint64_t horizon, bool all)
{
	unsigned int n = 0;

	for (unsigned int id = 0; id < r->n_slabs; id++) {
		if ((r->quar_mask & (1U << id)) != 0U && (all || now - r->quar_stamp[id] >= horizon)) {
			r->quar_mask &= ~(1U << id);
			atomic_or(&r->free_mask, bit(id));
			n++;
		}
	}
	return n;
}

unsigned int esdr_dma_ring_reap(struct esdr_dma_ring *r, uint64_t now, uint64_t horizon)
{
	return quar_release(r, now, horizon, false);
}

unsigned int esdr_dma_ring_quarantine_clear(struct esdr_dma_ring *r)
{
	return quar_release(r, 0U, 0U, true);
}

unsigned int esdr_dma_ring_quarantined(const struct esdr_dma_ring *r)
{
	return (unsigned int)__builtin_popcount(r->quar_mask);
}

unsigned int esdr_dma_ring_free_count(const struct esdr_dma_ring *r)
{
	atomic_val_t m = atomic_get(&r->free_mask);
	unsigned int n = 0;

	for (unsigned int id = 0; id < r->n_slabs; id++) {
		n += (m & bit(id)) != 0 ? 1U : 0U;
	}
	return n;
}

int esdr_dma_ring_check(const struct esdr_dma_ring *r)
{
	atomic_val_t m = atomic_get(&r->free_mask);
	int out = 0;

	if ((m >> r->n_slabs) != 0) {
		return -EFAULT;
	}
	for (unsigned int id = 0; id < r->n_slabs; id++) {
		unsigned int places = ((m & bit(id)) != 0 ? 1U : 0U);

		for (unsigned int pos = 0; pos < r->n_ring; pos++) {
			places += r->ring[pos] == id ? 1U : 0U;
		}
		if (places > 1U) {
			return -EFAULT;
		}
		if ((r->quar_mask & (1U << id)) != 0U && places != 0U) {
			return -EFAULT;
		}
		out += places == 0U ? 1 : 0;
	}
	return out;
}
