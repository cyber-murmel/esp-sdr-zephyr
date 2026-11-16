# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/micropython/modules/gnuradio/_layout.py: fit() on the boards' displays, with the rows of the eggs."""

from conftest import load

layout = load("apps/micropython/modules/gnuradio/_layout.py", "esdr_gui_layout")

ROW = 26
HEADER = 18


class Row:
    _flex = False
    _h = ROW

    def __init__(self, name=""):
        self.name = name

    def _make(self):
        pass


class Plot:
    _flex = True
    _h = 0
    _draws = True

    def __init__(self, name=""):
        self.name = name

    def _make(self):
        pass


def grid(*rows):
    g = layout.Grid()
    for r, cells in enumerate(rows):
        for c, cell in enumerate(cells):
            g.add(cell, r, c)
    return g


def box(*items):
    b = layout.Box()
    for item in items:
        b.add(item)
    return b


def fit(root, w, h):
    out = []
    layout.fit(root, 0, HEADER, w, h - HEADER, out)
    return {getattr(r[0], "name", ""): r[1:] for r in out}, [r[0] for r in out]


def waterfall():
    """The waterfall egg: two rows of a grid, three rows, one plot."""
    return box(grid((Row("gain"), Row("agc")), (Row("rate"), Row("fft"))),
               Row("upper"), Row("lower"), Row("freq"), Plot("waterfall"))


def spectrum():
    """The spectrum egg: two rows of a grid, a row, three plots."""
    return box(grid((Row("gain"), Row("agc")), (Row("rate"), Row("fft"))),
               Row("freq"), Plot("waterfall"), Plot("samples"), Plot("spectrum"))


def test_a_plot_with_no_room_goes_beside_the_rows():
    # On the Communicator its five rows take 130 of 124 pixels: no height for the plot at all.
    at, _ = fit(waterfall(), 428, 142)
    assert at["waterfall"] == (0, HEADER, 171, 124)
    # The rows in order in the rest of the width, none of them left a gap by the plot.
    assert at["gain"] == (171, HEADER, 128, ROW)
    assert at["agc"][0] == 171 + 128
    assert at["fft"][1] == HEADER + ROW
    assert [at[k][1] for k in ("upper", "lower", "freq")] == [HEADER + 2 * ROW, HEADER + 3 * ROW,
                                                             HEADER + 4 * ROW]


def test_plots_share_the_height_beside_the_rows():
    at, _ = fit(spectrum(), 428, 142)
    rects = [at[k] for k in ("waterfall", "samples", "spectrum")]
    assert all(r[0] == 0 and r[2] == 171 for r in rects)
    assert sum(r[3] for r in rects) == 124
    assert rects[1][1] == rects[0][1] + rects[0][3]
    assert at["freq"] == (171, HEADER + 2 * ROW, 257, ROW)


def test_the_rows_keep_the_focus_order():
    _, leaves = fit(waterfall(), 428, 142)
    names = [leaf.name for leaf in leaves if isinstance(leaf, Row)]
    assert names == ["gain", "agc", "rate", "fft", "upper", "lower", "freq"]


def test_a_tall_display_keeps_the_stack():
    # The XIAO's panel: both eggs as they always were.
    at, _ = fit(waterfall(), 320, 240)
    assert at["waterfall"] == (0, HEADER + 5 * ROW, 320, 222 - 5 * ROW)
    at, _ = fit(spectrum(), 320, 240)
    assert at["waterfall"] == (0, HEADER + 3 * ROW, 320, 48)
    assert at["freq"] == (0, HEADER + 2 * ROW, 320, ROW)


def test_enough_height_on_a_short_display_keeps_the_stack():
    # The Communicator's constellation: two rows, plots of 46 pixels side by side.
    h = layout.HBox()
    h.add(Plot("spectrum"))
    h.add(Plot("iq"))
    at, _ = fit(box(Row("gain"), Row("freq"), Row("rate"), h), 428, 142)
    assert at["spectrum"] == (0, HEADER + 3 * ROW, 214, 46)
    assert at["iq"] == (214, HEADER + 3 * ROW, 214, 46)


def test_rows_alone_are_left_alone():
    at, _ = fit(box(*[Row(str(k)) for k in range(8)]), 428, 142)
    assert at["7"] == (0, HEADER + 7 * ROW, 428, ROW)
