/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The slab bookkeeping of lib/esp_sdr/src/esdr_dma_ring.c against a fake descriptor table: who owns
 * which slab, in what order the descriptors change, a restart with a slab out, no spare.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <esp_sdr/esdr_dma_ring.h>

/* The S31's shape: 32 nodes of 4096 B, slabs of 8, three spares. */
#define NODES      32U
#define SLAB_NODES 8U
#define NODE_BYTES 4096U
#define SPARES     3U
#define SLAB_BYTES (SLAB_NODES * NODE_BYTES)
#define RING_SLABS (NODES / SLAB_NODES)
#define PAYLOAD    (NODE_BYTES - 16U)

static uint8_t ring_mem[NODES * NODE_BYTES] __aligned(64);
static uint8_t spare_mem[SPARES * SLAB_BYTES] __aligned(64);

static uint8_t *desc[NODES];
static unsigned int writes, fences;
static unsigned int order[64];
static unsigned int n_order;
static struct esdr_dma_ring r;

static void set_buf(void *ctx, unsigned int node, uint8_t *buf)
{
	ARG_UNUSED(ctx);
	desc[node] = buf;
	if (n_order < ARRAY_SIZE(order)) {
		order[n_order++] = node;
	}
	writes++;
}

static void fence(void *ctx)
{
	ARG_UNUSED(ctx);
	fences++;
}

static const struct esdr_dma_ring_ops ops = {.set_buf = set_buf, .fence = fence};

static void before(void *f)
{
	ARG_UNUSED(f);
	memset(desc, 0, sizeof(desc));
	writes = fences = n_order = 0U;
	zassert_ok(esdr_dma_ring_init(&r, &ops, NULL, NODES, SLAB_NODES, NODE_BYTES, ring_mem,
				      spare_mem, SPARES));
	esdr_dma_ring_relink(&r);
}

ZTEST_SUITE(dma_ring, NULL, NULL, before, NULL, NULL);

ZTEST(dma_ring, test_init_layout)
{
	zassert_equal(esdr_dma_ring_free_count(&r), SPARES);
	zassert_equal(esdr_dma_ring_check(&r), 0);
	for (unsigned int n = 0; n < NODES; n++) {
		zassert_equal(desc[n], ring_mem + n * NODE_BYTES);
	}
	zassert_equal(fences, 1U, "relink fences once");
	zassert_equal(esdr_dma_ring_init(&r, &ops, NULL, 30U, 8U, NODE_BYTES, ring_mem, spare_mem,
					 SPARES), -EINVAL, "nodes not whole slabs");
	zassert_equal(esdr_dma_ring_init(&r, &ops, NULL, NODES, SLAB_NODES, NODE_BYTES, ring_mem,
					 spare_mem, 13U), -EINVAL, "too many slabs");
}

ZTEST(dma_ring, test_swap_moves_only_its_slab_and_returns_the_old_one)
{
	uint8_t *spare = esdr_dma_ring_spare_take(&r);
	uint8_t *old;

	zassert_not_null(spare);
	zassert_true(spare >= spare_mem && spare < spare_mem + sizeof(spare_mem));
	zassert_equal(esdr_dma_ring_free_count(&r), SPARES - 1U);
	n_order = 0U;
	fences = 0U;
	/* Any node of the slab names it: node 19 is in the slab that starts at 16. */
	old = esdr_dma_ring_swap(&r, 19U, spare);
	zassert_equal(old, ring_mem + 2U * SLAB_BYTES);
	for (unsigned int k = 0; k < SLAB_NODES; k++) {
		zassert_equal(order[k], 16U + k, "oldest node first");
		zassert_equal(desc[16U + k], spare + k * NODE_BYTES);
	}
	zassert_equal(n_order, SLAB_NODES);
	zassert_equal(fences, 1U, "one fence after the last descriptor");
	for (unsigned int n = 0; n < NODES; n++) {
		if (n / SLAB_NODES != 2U) {
			zassert_equal(desc[n], ring_mem + n * NODE_BYTES, "node %u moved", n);
		}
	}
	/* The old one and nothing else is out, and it is not the ring's any more. */
	zassert_equal(esdr_dma_ring_check(&r), 1);
	zassert_equal(esdr_dma_ring_put(&r, old + 5000U), 0, "an address inside gives it back");
	zassert_equal(esdr_dma_ring_put(&r, old), -EALREADY, "never twice");
	zassert_equal(esdr_dma_ring_free_count(&r), SPARES);
	zassert_equal(esdr_dma_ring_check(&r), 0);
}

ZTEST(dma_ring, test_no_spare_changes_nothing)
{
	uint8_t *held[SPARES];

	for (unsigned int k = 0; k < SPARES; k++) {
		held[k] = esdr_dma_ring_spare_take(&r);
		zassert_not_null(held[k]);
		for (unsigned int j = 0; j < k; j++) {
			zassert_not_equal(held[j], held[k], "one slab given twice");
		}
	}
	writes = 0U;
	zassert_is_null(esdr_dma_ring_spare_take(&r));
	zassert_equal(writes, 0U);
	zassert_equal(esdr_dma_ring_free_count(&r), 0U);
	/* Giving them back, any order, restores the pool. */
	for (unsigned int k = SPARES; k-- > 0;) {
		zassert_ok(esdr_dma_ring_put(&r, held[k]));
	}
	zassert_equal(esdr_dma_ring_free_count(&r), SPARES);
}

ZTEST(dma_ring, test_swap_refuses_what_is_not_out)
{
	uint8_t *spare = esdr_dma_ring_spare_take(&r);

	/* A slab in the ring, a free one and a foreign address are no spares. */
	zassert_is_null(esdr_dma_ring_swap(&r, 0U, ring_mem + SLAB_BYTES));
	zassert_is_null(esdr_dma_ring_swap(&r, 0U, spare + 64U), "not a slab's start");
	zassert_is_null(esdr_dma_ring_swap(&r, 0U, NULL));
	zassert_is_null(esdr_dma_ring_swap(&r, NODES, spare), "node out of range");
	zassert_ok(esdr_dma_ring_put(&r, spare));
	writes = 0U;
	zassert_is_null(esdr_dma_ring_swap(&r, 0U, spare), "a free slab is not out");
	zassert_equal(writes, 0U, "a refused swap writes no descriptor");
	zassert_equal(esdr_dma_ring_put(&r, ring_mem), -EINVAL, "the ring's own slab");
	zassert_equal(esdr_dma_ring_put(&r, (uint8_t *)&r), -EINVAL, "no slab at all");
	zassert_equal(esdr_dma_ring_check(&r), 0);
}

/* A restart while a consumer holds slabs: relink must not point the DMA into them. */
ZTEST(dma_ring, test_restart_with_slabs_out)
{
	uint8_t *out[2];

	for (unsigned int k = 0; k < 2U; k++) {
		uint8_t *spare = esdr_dma_ring_spare_take(&r);

		out[k] = esdr_dma_ring_swap(&r, k * SLAB_NODES, spare);
		zassert_not_null(out[k]);
	}
	/* The ring now runs in the first two spares and the slabs 2 and 3 of its own. */
	zassert_equal(esdr_dma_ring_check(&r), 2);
	memset(desc, 0, sizeof(desc));
	esdr_dma_ring_relink(&r);
	for (unsigned int n = 0; n < NODES; n++) {
		zassert_equal(desc[n], esdr_dma_ring_node(&r, n));
		for (unsigned int k = 0; k < 2U; k++) {
			zassert_false(desc[n] >= out[k] && desc[n] < out[k] + SLAB_BYTES,
				      "node %u in a slab the consumer holds", n);
		}
	}
	/* The consumer's give-back after the restart joins the pool, and is usable at once. */
	zassert_ok(esdr_dma_ring_put(&r, out[1]));
	zassert_equal(esdr_dma_ring_check(&r), 1);
	{
		uint8_t *again = esdr_dma_ring_spare_take(&r);
		uint8_t *got = esdr_dma_ring_swap(&r, 3U * SLAB_NODES, again);

		zassert_not_null(got);
		zassert_equal(esdr_dma_ring_check(&r), 2);
		zassert_ok(esdr_dma_ring_put(&r, got));
	}
	zassert_ok(esdr_dma_ring_put(&r, out[0]));
	zassert_equal(esdr_dma_ring_check(&r), 0);
	zassert_equal(esdr_dma_ring_free_count(&r), SPARES);
}

/*
 * Ownership never doubles, whatever the order of takes, swaps, puts, restarts, quarantines, reaps
 * and the mistakes a caller can make. The books are checked against a model every step.
 */
ZTEST(dma_ring, test_random_ownership)
{
	uint8_t *held[SPARES + RING_SLABS];
	uint8_t *taken[SPARES];	/* taken, not swapped in */
	uint8_t *quar[SPARES + RING_SLABS];
	uint64_t quar_at[SPARES + RING_SLABS];
	unsigned int n_held = 0U, n_taken = 0U, n_quar = 0U;
	uint64_t now = 0U;

	srand(1234);
	for (unsigned int step = 0; step < 40000U; step++) {
		now += (unsigned int)rand() % 5U;
		switch (rand() % 10) {
		case 0:
		case 1: {
			/* The sink's step: swap a slab, keep the old memory. */
			uint8_t *spare = esdr_dma_ring_spare_take(&r);
			uint8_t *old;

			if (spare == NULL) {
				zassert_equal(n_held + n_taken + n_quar, SPARES, "no spare, fewer out");
				break;
			}
			old = esdr_dma_ring_swap(&r, (unsigned int)rand() % NODES, spare);
			zassert_not_null(old);
			for (unsigned int k = 0; k < n_held; k++) {
				zassert_not_equal(held[k], old, "a slab out twice");
			}
			held[n_held++] = old;
			break;
		}
		case 2:
			/* The completion: any held one, in any order, at an inner address. */
			if (n_held != 0U) {
				unsigned int k = (unsigned int)rand() % n_held;

				zassert_ok(esdr_dma_ring_put(&r, held[k] + (unsigned int)rand() % SLAB_BYTES));
				held[k] = held[--n_held];
			}
			break;
		case 3:
			esdr_dma_ring_relink(&r);
			break;
		case 4: {
			/* A take that is not swapped in (a pre-check that said no, after the take). */
			uint8_t *spare = esdr_dma_ring_spare_take(&r);

			if (spare != NULL) {
				taken[n_taken++] = spare;
			}
			break;
		}
		case 5:
			if (n_taken != 0U) {
				unsigned int k = (unsigned int)rand() % n_taken;

				zassert_ok(esdr_dma_ring_put(&r, taken[k]));
				taken[k] = taken[--n_taken];
			}
			break;
		case 6:
			/* A stale swap: its old memory is held back, a later reap lets it go. */
			if (n_held != 0U) {
				unsigned int k = (unsigned int)rand() % n_held;

				zassert_ok(esdr_dma_ring_quarantine(&r, held[k], now));
				quar[n_quar] = held[k];
				quar_at[n_quar++] = now;
				held[k] = held[--n_held];
			}
			break;
		case 7: {
			unsigned int gone = esdr_dma_ring_reap(&r, now, 12U);
			unsigned int want = 0U;

			for (unsigned int k = 0; k < n_quar;) {
				if (now - quar_at[k] >= 12U) {
					want++;
					quar[k] = quar[--n_quar];
					quar_at[k] = quar_at[n_quar];
				} else {
					k++;
				}
			}
			zassert_equal(gone, want, "reap released %u, model %u", gone, want);
			break;
		}
		case 8:
			/* A stopped DMA lets everything go. */
			if (rand() % 8 == 0) {
				zassert_equal(esdr_dma_ring_quarantine_clear(&r), n_quar);
				n_quar = 0U;
			}
			break;
		default: {
			/* Mistakes: all refused, none changes anything. */
			uint8_t *any = n_held != 0U ? held[0] : esdr_dma_ring_node(&r, 0U);

			zassert_equal(esdr_dma_ring_put(&r, esdr_dma_ring_node(&r, (unsigned int)rand() % NODES) +
							    (unsigned int)rand() % NODE_BYTES),
				      -EINVAL, "a ring slab put");
			zassert_equal(esdr_dma_ring_put(&r, (uint8_t *)&r), -EINVAL);
			zassert_is_null(esdr_dma_ring_swap(&r, NODES + (unsigned int)rand() % 9U, any));
			zassert_is_null(esdr_dma_ring_swap(&r, 0U, esdr_dma_ring_node(&r, 0U)),
					"the ring's own slab as a spare");
			if (n_quar != 0U) {
				zassert_equal(esdr_dma_ring_put(&r, quar[0]), -EBUSY);
				zassert_equal(esdr_dma_ring_quarantine(&r, quar[0], now), -EINVAL);
				zassert_is_null(esdr_dma_ring_swap(&r, 0U, quar[0]),
						"a held-back slab as a spare");
			}
			break;
		}
		}
		/* The books against the model. */
		zassert_equal(esdr_dma_ring_check(&r), (int)(n_held + n_taken + n_quar), "step %u", step);
		zassert_equal(esdr_dma_ring_quarantined(&r), n_quar, "step %u", step);
		zassert_equal(esdr_dma_ring_free_count(&r), SPARES - n_held - n_taken - n_quar,
			      "step %u", step);
		for (unsigned int n = 0; n < NODES; n++) {
			uint8_t *p = esdr_dma_ring_node(&r, n);

			/* After a relink every descriptor says what the books say: checked at once. */
			for (unsigned int k = 0; k < n_held; k++) {
				zassert_false(p >= held[k] && p < held[k] + SLAB_BYTES,
					      "ring node %u inside a held slab", n);
			}
			for (unsigned int k = 0; k < n_quar; k++) {
				zassert_false(p >= quar[k] && p < quar[k] + SLAB_BYTES,
					      "ring node %u inside a held-back slab", n);
			}
			for (unsigned int k = 0; k < n_taken; k++) {
				zassert_false(p >= taken[k] && p < taken[k] + SLAB_BYTES,
					      "ring node %u inside a taken spare", n);
			}
		}
		if (step % 7U == 0U) {
			esdr_dma_ring_relink(&r);
			for (unsigned int n = 0; n < NODES; n++) {
				zassert_equal(desc[n], esdr_dma_ring_node(&r, n),
					      "descriptor %u differs from the books", n);
			}
		}
	}
}

/* The proof after a swap: the lead the library may swap at, and the first it may not. */
ZTEST(dma_ring, test_swap_safe_boundary)
{
	/* 32 nodes, margin 4: a lead of 28 nodes is fine, 29 is not. */
	zassert_true(esdr_dma_ring_swap_safe(0, NODES, PAYLOAD, 4U));
	zassert_true(esdr_dma_ring_swap_safe(28 * (int64_t)PAYLOAD + PAYLOAD - 1, NODES, PAYLOAD, 4U));
	zassert_false(esdr_dma_ring_swap_safe(29 * (int64_t)PAYLOAD, NODES, PAYLOAD, 4U));
	/* A slab that began before the run: a negative first index only adds to the lead. */
	zassert_true(esdr_dma_ring_swap_safe(-(int64_t)PAYLOAD, NODES, PAYLOAD, 4U));
	/* The normal case: a batch of 8 just finished, a node or two on. */
	zassert_true(esdr_dma_ring_swap_safe(10 * (int64_t)PAYLOAD, NODES, PAYLOAD, 4U));
}

/* A swap the DMA may have raced: the old memory is not a spare until it is provably safe. */
ZTEST(dma_ring, test_quarantine_holds_a_slab_back)
{
	uint8_t *spare = esdr_dma_ring_spare_take(&r);
	uint8_t *old = esdr_dma_ring_swap(&r, 0U, spare);
	uint8_t *others[SPARES - 1U];

	zassert_not_null(old);
	zassert_ok(esdr_dma_ring_quarantine(&r, old + 100U, 1000U));
	zassert_equal(esdr_dma_ring_quarantine(&r, old, 1000U), -EINVAL, "held back twice");
	zassert_equal(esdr_dma_ring_quarantined(&r), 1U);
	zassert_equal(esdr_dma_ring_check(&r), 1, "a held-back slab is out");
	/* The consumer's give-back cannot free it, and the pool cannot hand it out as a spare. */
	zassert_equal(esdr_dma_ring_put(&r, old), -EBUSY);
	for (unsigned int k = 0; k < SPARES - 1U; k++) {
		others[k] = esdr_dma_ring_spare_take(&r);
		zassert_not_null(others[k]);
		zassert_not_equal(others[k], old);
	}
	zassert_is_null(esdr_dma_ring_spare_take(&r), "the quarantined slab is no spare");
	/* Not yet a lap behind: nothing goes. */
	zassert_equal(esdr_dma_ring_reap(&r, 1000U + 63U, 64U), 0U);
	zassert_equal(esdr_dma_ring_quarantined(&r), 1U);
	zassert_equal(esdr_dma_ring_reap(&r, 1000U + 64U, 64U), 1U);
	zassert_equal(esdr_dma_ring_quarantined(&r), 0U);
	zassert_equal(esdr_dma_ring_free_count(&r), 1U);
	zassert_equal(esdr_dma_ring_spare_take(&r), old, "now a spare");
	zassert_equal(esdr_dma_ring_check(&r), 3);
}

/* Only a stopped DMA lets go of everything at once; a slab in the ring or free cannot be held back. */
ZTEST(dma_ring, test_quarantine_clear_and_refusals)
{
	uint8_t *a = esdr_dma_ring_swap(&r, 0U, esdr_dma_ring_spare_take(&r));
	uint8_t *b = esdr_dma_ring_swap(&r, SLAB_NODES, esdr_dma_ring_spare_take(&r));

	zassert_equal(esdr_dma_ring_quarantine(&r, ring_mem + 2U * SLAB_BYTES, 0U), -EINVAL,
		      "a ring slab");
	zassert_equal(esdr_dma_ring_quarantine(&r, spare_mem + 2U * SLAB_BYTES, 0U), -EINVAL,
		      "a free slab");
	zassert_ok(esdr_dma_ring_quarantine(&r, a, 5U));
	zassert_ok(esdr_dma_ring_quarantine(&r, b, 9U));
	/* A consumer's slab and a held-back one are both out for the books. */
	zassert_equal(esdr_dma_ring_check(&r), 2);
	zassert_equal(esdr_dma_ring_reap(&r, 20U, 12U), 1U, "only the older one");
	/* A restart: relink leaves held-back slabs out of the ring too. */
	esdr_dma_ring_relink(&r);
	for (unsigned int n = 0; n < NODES; n++) {
		zassert_false(desc[n] >= b && desc[n] < b + SLAB_BYTES);
	}
	zassert_equal(esdr_dma_ring_quarantine_clear(&r), 1U);
	zassert_equal(esdr_dma_ring_free_count(&r), SPARES);
	zassert_equal(esdr_dma_ring_check(&r), 0);
}
