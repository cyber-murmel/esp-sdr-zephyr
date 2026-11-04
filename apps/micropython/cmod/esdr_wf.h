/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The waterfall's picture (waterfall_sink_c in modules/gnuradio/qtgui.py): a ring of rows that
 * hold only power, each with the view it was drawn in, put on the screen with the grid over them.
 * A retune changes the view and nothing else, the grid never enters the history. Plain C, no
 * Zephyr: the host test builds it too (tests/host/test_waterfall.py).
 */

#ifndef ESDR_WF_H_
#define ESDR_WF_H_

#include <stdint.h>

/* In an entry of lines: a coarse line (else fine) at the column in the bits below. */
#define ESDR_WF_LINE_COARSE 0x8000U

/* An entry of marks: the row's time line. */
#define ESDR_WF_MARK_FINE   1U
#define ESDR_WF_MARK_COARSE 2U

struct esdr_wf_src {
	const uint16_t *img;   /* rows of w pixels, RGB565 */
	const int32_t *col0;   /* per row: the view it was drawn in */
	const uint8_t *marks;  /* per row: its time line, or 0 */
	const uint16_t *lines; /* the frequency lines */
	int w, rows;
	int head;              /* the newest row, on top; older ones follow, wrapping */
	int nlines;
	int32_t view;
	uint16_t fill;         /* what no row knows: 0 power */
	uint16_t coarse, fine; /* the colours lines lean towards, half way */
};

/* The picture into dst, whose rows lie stride pixels apart. */
void esdr_wf_compose(uint16_t *dst, int stride, const struct esdr_wf_src *s);

#endif /* ESDR_WF_H_ */
