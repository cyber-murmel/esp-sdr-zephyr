/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The waterfall's picture (esdr_wf.h). Row by row: the lines are blended while the row they cross
 * is still in the cache (at the WHY badge's 1440 bytes a row, a column at a time would miss on
 * nearly every pixel of the PSRAM).
 */

#include <stddef.h>
#include <string.h>

#include "esdr_wf.h"

#define LINE_COLUMN 0x7FFFU

/* Half way from px to colour: a line never hides what it crosses. */
static inline uint16_t lean(uint16_t px, uint16_t colour)
{
	return (uint16_t)(((px & 0xF7DEU) >> 1) + ((colour & 0xF7DEU) >> 1));
}

static void copy_row(uint16_t *dst, const uint16_t *src, int w, int shift, uint16_t fill)
{
	int from = shift > 0 ? shift : 0;
	int to = shift < 0 ? w + shift : w;
	int c;

	for (c = 0; c < from; c++) {
		dst[c] = fill;
	}
	if (to > from) {
		memcpy(&dst[from], &src[from - shift], (size_t)(to - from) * sizeof(*dst));
	}
	for (c = to > from ? to : from; c < w; c++) {
		dst[c] = fill;
	}
}

void esdr_wf_compose(uint16_t *dst, int stride, const struct esdr_wf_src *s)
{
	const int w = s->w;
	int r = s->head;

	for (int row = 0; row < s->rows; row++, dst += stride) {
		/* Clamped: a row a whole width away or more shows nothing, however far. */
		int64_t far = (int64_t)s->col0[r] - (int64_t)s->view;
		int shift = far <= -(int64_t)w ? -w : far >= (int64_t)w ? w : (int)far;

		copy_row(dst, &s->img[(size_t)r * (size_t)w], w, shift, s->fill);

		if (s->marks[r] == ESDR_WF_MARK_COARSE) {
			for (int c = 0; c < w; c++) {
				dst[c] = lean(dst[c], s->coarse);
			}
		} else if (s->marks[r] == ESDR_WF_MARK_FINE) {
			for (int c = 0; c < w; c += 2) {
				dst[c] = lean(dst[c], s->fine);
			}
		}
		for (int k = 0; k < s->nlines; k++) {
			uint16_t line = s->lines[k];
			int c = (int)(line & LINE_COLUMN);

			if (c >= w) {
				continue;
			}
			if ((line & ESDR_WF_LINE_COARSE) != 0U) {
				dst[c] = lean(dst[c], s->coarse);
			} else if ((row & 1) == 0) {
				dst[c] = lean(dst[c], s->fine);
			}
		}
		r = r + 1 == s->rows ? 0 : r + 1;
	}
}
