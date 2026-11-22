/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <string.h>

#include <esp_sdr/esp_sdr_place.h>

#include "netframe.h"
#include "vrt_cells.h"

void vrt_cells_init(struct vrt_cells *c, uint8_t *mem, struct vrt_cell_meta *meta,
		    uint32_t ncells, uint32_t stride)
{
	memset(c, 0, sizeof(*c));
	c->mem = mem;
	c->meta = meta;
	c->ncells = ncells;
	c->stride = stride;
}

void vrt_cells_start(struct vrt_cells *c, uint32_t pl, uint32_t rate, uint64_t freq_hz, int gain,
		     uint8_t epoch, bool sum_on, bool loss)
{
	c->pl = pl;
	c->rate = rate;
	c->freq_hz = freq_hz;
	c->gain = (int16_t)gain;
	c->epoch = epoch;
	c->run++;
	c->sum_on = sum_on;
	c->wr = c->wr_c = (uint32_t)atomic_get(&c->ready);
	c->off = c->off_c = 0U;
	c->have_next = false;
	c->loss_pending = c->loss_pending_c = loss;
}

/*
 * Copy n words and add them up as 64 bits (lo, with a carry counted in hi): eight words in
 * registers before the first store, no branch in the loop, 5 instructions a word.
 */
/* Once per sample word: at -O3, the app is -Os. */
static ESP_SDR_CODE(PACED_SAMPLE, copy_sum) __attribute__((optimize("O3")))
uint64_t copy_sum(uint32_t *restrict d, const uint32_t *restrict s, size_t n)
{
	uint32_t lo = 0, hi = 0;
	size_t k = 0;

#define STEP(v)                                                                                    \
	do {                                                                                       \
		lo += (v);                                                                         \
		hi += lo < (v);                                                                    \
	} while (0)
	for (; k + 8U <= n; k += 8U) {
		uint32_t a0 = s[k], a1 = s[k + 1U], a2 = s[k + 2U], a3 = s[k + 3U];
		uint32_t a4 = s[k + 4U], a5 = s[k + 5U], a6 = s[k + 6U], a7 = s[k + 7U];

		d[k] = a0;
		d[k + 1U] = a1;
		d[k + 2U] = a2;
		d[k + 3U] = a3;
		d[k + 4U] = a4;
		d[k + 5U] = a5;
		d[k + 6U] = a6;
		d[k + 7U] = a7;
		STEP(a0);
		STEP(a1);
		STEP(a2);
		STEP(a3);
		STEP(a4);
		STEP(a5);
		STEP(a6);
		STEP(a7);
	}
	for (; k < n; k++) {
		uint32_t v = s[k];

		d[k] = v;
		STEP(v);
	}
#undef STEP
	return ((uint64_t)hi << 32) | lo;
}

static void gap(struct vrt_cells *c)
{
	/* The cell under construction is not a packet: its samples are lost too. */
	c->wr = c->wr_c;
	c->off = 0U;
	c->off_c = 0U;
	c->loss_pending = true;
	c->loss_pending_c = true;
	c->gaps++;
}

ESP_SDR_CODE(PACED_SAMPLE, vrt_cells_push) __attribute__((optimize("O3")))
int vrt_cells_push(struct vrt_cells *c, const uint8_t *data, size_t len, uint64_t index)
{
	uint32_t need;
	size_t pos = 0;

	if (c->have_next && index != c->next_index) {
		/* The ring skipped nodes. */
		gap(c);
	}
	c->next_index = index + len / 2U;
	c->have_next = true;
	need = (uint32_t)((c->off + len + c->pl - 1U) / c->pl);
	if (c->wr + need - (uint32_t)atomic_get(&c->released) > c->ncells) {
		gap(c);
		c->dropped_nodes++;
		return -ENOSPC;
	}
	while (pos < len) {
		uint32_t room = c->pl - c->off;
		size_t chunk = len - pos < room ? len - pos : room;
		uint8_t *cell = vrt_cell_ptr(c, c->wr);

		if (c->off == 0U) {
			struct vrt_cell_meta *m = &c->meta[c->wr & (c->ncells - 1U)];

			m->index = index + pos / 2U;
			m->loss = c->loss_pending;
			m->bytes = (uint16_t)c->pl;
			m->rate = c->rate;
			m->freq_hz = c->freq_hz;
			m->gain = c->gain;
			m->epoch = c->epoch;
			m->run = c->run;
			m->sum = 0;
			m->sum_valid = c->sum_on;
			c->loss_pending = false;
		}
		if (c->sum_on) {
			/* one pass: copy and the UDP checksum's sum of the words */
			c->meta[c->wr & (c->ncells - 1U)].sum +=
				copy_sum((uint32_t *)__builtin_assume_aligned(cell + c->off, 4),
					 (const uint32_t *)__builtin_assume_aligned(data + pos, 4),
					 chunk / 4U);
		} else {
			memcpy(cell + c->off, data + pos, chunk);
		}
		c->off += (uint32_t)chunk;
		pos += chunk;
		if (c->off == c->pl) {
			c->off = 0U;
			c->wr++;
		}
	}
	return 0;
}

void vrt_cells_commit(struct vrt_cells *c)
{
	c->wr_c = c->wr;
	c->off_c = c->off;
	c->loss_pending_c = c->loss_pending;
	atomic_set(&c->ready, (atomic_val_t)c->wr);
}

void vrt_cells_abort(struct vrt_cells *c)
{
	(void)c->off_c;
	gap(c);
}
