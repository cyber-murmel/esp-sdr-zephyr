# SPDX-License-Identifier: GPL-3.0-or-later
"""The waterfall's view and grid, free of the board (qtgui.waterfall_sink_c draws them).

Frequencies are integers in Hz: a float here has 256 Hz steps at 2.4 GHz. Frequency lines sit at
multiples of their spacing, so they stay on their frequency as the radio retunes; a time line
belongs to the first row at or after a multiple of its spacing and scrolls with it, so an uneven
row rate shows as uneven lines rather than as a wrong time."""

import array

# In lines(): a coarse line (ESDR_WF_LINE_COARSE, cmod/esdr_wf.h).
COARSE = 0x8000
# A row's time line (ESDR_WF_MARK_*).
MARK_FINE = 1
MARK_COARSE = 2
# Frequency lines closer than this are not drawn: a solid plot, and a long loop.
MIN_PX = 6
# The fewest rows between time lines: a spacing near the row rate gives no solid band.
GAP_FINE = 3
GAP_COARSE = 2
# Automatic time lines: about 6 coarse ones on the plot, at least this many rows apart.
AUTO_ROWS = 16
# A retune's unknown rows at most, should the samples not come back.
SETTLE_ROWS = 8


def view(hz, anchor, w, bw):
    """How many columns hz lies above anchor, rounded: from the frequency alone, so that small
    steps cannot add up to an error and a tune back returns exactly."""
    return (2 * (hz - anchor) * w + bw) // (2 * bw)


def _auto(x):
    """(fine, coarse): coarse the smallest of 1, 2, 5 x 10^n at or above x, fine a fifth of it or,
    for a 2, a quarter (the lines of the two levels then coincide)."""
    p = 1
    while True:
        for m in (1, 2, 5):
            if m * p >= x:
                return m * p // (4 if m == 2 else 5), m * p
        p *= 10


def spacing(value, scale=1):
    """A spacing as given to set_grid_spacing(), in integer units: None stays (automatic), at
    or below 0 is 0 (off)."""
    return None if value is None else max(0, int(value * scale))


def freq_levels(fine, coarse, bw, w):
    """The frequency lines' (fine, coarse) in Hz; a level too dense to draw is 0."""
    auto = _auto((bw + 7) // 8)
    fine = auto[0] if fine is None else fine
    coarse = auto[1] if coarse is None else coarse
    return (0 if fine * w < MIN_PX * bw else fine, 0 if coarse * w < MIN_PX * bw else coarse)


def time_levels(fine, coarse, rows, update):
    """The time lines' (fine, coarse) in ms, for rows drawn every update seconds."""
    auto = _auto(max(rows // 6, AUTO_ROWS) * int(update * 1000 + 0.5))
    return (auto[0] if fine is None else fine, auto[1] if coarse is None else coarse)


def lines(hz, bw, w, n, fine, coarse):
    """The columns of the lines around hz over w columns of an n-point spectrum, in order, COARSE
    on the coarse ones. A column is where esdr.psd draws the frequency: a bin's centre lies half a
    bin (w / 2n columns) to the right of the plain mapping."""
    mask = bytearray(w)
    den = 2 * n * bw
    for level, s in ((MARK_FINE, fine), (MARK_COARSE, coarse)):
        if s <= 0 or s * w < MIN_PX * bw:
            continue
        f = (hz - bw // 2 - bw // (2 * n) - 1) // s * s
        while True:
            x = w * (2 * n * (f - hz) + (n + 1) * bw) // den
            if x >= w:
                break
            if x >= 0:
                mask[x] = level
            f += s
    out = array.array("H")
    for x in range(w):
        if mask[x]:
            out.append(x | COARSE if mask[x] == MARK_COARSE else x)
    return out


class TimeMarks:
    """Which rows get a time line. It counts the ms between rows, so a wrap of the ticks does not
    matter; a gap over several multiples gives one line, and coarse wins."""

    def __init__(self):
        self.fine = 0
        self.coarse = 0
        self._tf = 0
        self._tc = 0
        self._gap = 0

    def set(self, fine, coarse):
        """The spacings in ms (0: off); whether they changed."""
        if (fine, coarse) == (self.fine, self.coarse):
            return False
        self.fine = fine
        self.coarse = coarse
        self._tf = 0
        self._tc = 0
        return True

    def row(self, dt):
        """The mark of a row dt ms after the last."""
        self._gap += 1
        hit_fine = hit_coarse = False
        if self.fine > 0:
            self._tf += dt
            hit_fine = self._tf >= self.fine
            self._tf %= self.fine
        if self.coarse > 0:
            self._tc += dt
            hit_coarse = self._tc >= self.coarse
            self._tc %= self.coarse
        if hit_coarse and self._gap >= GAP_COARSE:
            self._gap = 0
            return MARK_COARSE
        if hit_fine and self._gap >= GAP_FINE:
            self._gap = 0
            return MARK_FINE
        return 0


class Settle:
    """Whether a row's samples all came after the last retune. The monitor is a running ring, so a
    window read then can hold the old tune or both. The first row is always unknown; the next ones
    are until size new samples have come since it. A source without a running total (total None,
    the channelizer's rows) gets the one row."""

    def __init__(self):
        self._rows = 0
        self._total = None

    def retuned(self):
        self._rows = SETTLE_ROWS
        self._total = None

    def known(self, total, size):
        if not self._rows:
            return True
        self._rows -= 1
        if total is None:
            self._rows = 0
            return False
        if self._total is None:
            self._total = total
            return False
        if (total - self._total) & 0xFFFFFFFF >= size:
            self._rows = 0
            return True
        return False
