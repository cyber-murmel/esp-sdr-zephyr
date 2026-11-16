# SPDX-License-Identifier: GPL-3.0-or-later
"""Geometry for the Qt layouts of GRC exports on the board's display; pure Python, no display.

A node is a leaf (class attribute _make; instance _flex and _h: a fixed height, or a share
of what is left) or a container with _need() -> (fixed height, flex units) and
_place(x, y, w, h, out), which appends (leaf, x, y, w, h) rectangles to out. Boxes stack
(vertical) or split the width equally (horizontal); grids split the width by column and give
each row what its widgets need; flex units share the height that is left over equally. fit()
places a whole flowgraph: on a short display, its plots beside the rows."""

# Less height than this leaves a plot little more than its label (_gui.LABEL).
PLOT_MIN = 30


def _plot(rect):
    return getattr(type(rect[0]), "_draws", False)


def fit(root, x, y, w, h, out):
    """Place root in the rectangle. When its rows leave a plot too little height, the plots share
    the whole height in the left 2/5 and the rows keep their order at the right. A row that still
    does not fit is scrolled to when it takes the focus."""
    root._place(x, y, w, h, out)
    heights = [r[4] for r in out if _plot(r)]
    if not heights or min(heights) >= PLOT_MIN:
        return
    plots = [r[0] for r in out if _plot(r)]
    left = w * 2 // 5
    rows = []
    root._place(x + left, y, w - left, 0, rows)
    out.clear()
    out.extend(r for r in rows if not _plot(r))
    for i, plot in enumerate(plots):
        y0 = h * i // len(plots)
        y1 = h * (i + 1) // len(plots)
        out.append((plot, x, y + y0, left, y1 - y0))


def need(item):
    t = type(item)
    if hasattr(t, "_need"):
        return item._need()
    if hasattr(t, "_make"):
        return (0, 1) if item._flex else (item._h, 0)
    return (0, 0)


def place(item, x, y, w, h, out):
    t = type(item)
    if hasattr(t, "_place"):
        item._place(x, y, w, h, out)
    elif hasattr(t, "_make"):
        out.append((item, x, y, w, h))


def stack(items, x, y, w, h, out):
    """Items top to bottom; the fixed parts first, the rest of h shared among the flex units."""
    parts = [(it, need(it)) for it in items]
    parts = [p for p in parts if p[1] != (0, 0)]
    flex_left = sum(n for _, (f, n) in parts)
    remaining = max(0, h - sum(f for _, (f, n) in parts))
    for it, (f, n) in parts:
        add = 0
        if n:
            add = remaining * n // flex_left
            remaining -= add
            flex_left -= n
        place(it, x, y, w, f + add, out)
        y += f + add


class Box:
    horizontal = False

    def __init__(self):
        self._items = []

    def add(self, item, *args):
        self._items.append(item)

    def _parts(self):
        parts = [(it, need(it)) for it in self._items]
        return [p for p in parts if p[1] != (0, 0)]

    def _need(self):
        parts = self._parts()
        if self.horizontal:
            return (max([f for _, (f, n) in parts] or [0]), max([n for _, (f, n) in parts] or [0]))
        return (sum(f for _, (f, n) in parts), sum(n for _, (f, n) in parts))

    def _place(self, x, y, w, h, out):
        if not self.horizontal:
            stack([it for it, _ in self._parts()], x, y, w, h, out)
            return
        parts = self._parts()
        for i, (it, _) in enumerate(parts):
            x0 = w * i // len(parts)
            x1 = w * (i + 1) // len(parts)
            place(it, x + x0, y, x1 - x0, h, out)


class HBox(Box):
    horizontal = True


class Grid:
    def __init__(self):
        self._cells = []  # (item, row, col, rowspan, colspan)

    def add(self, item, row=None, col=0, rowspan=1, colspan=1, *args):
        if row is None:
            row = max([c[1] for c in self._cells] or [-1]) + 1
        self._cells.append((item, int(row), int(col), int(rowspan), int(colspan)))

    def _rows(self):
        rows = {}
        for cell in self._cells:
            if need(cell[0]) != (0, 0):
                rows.setdefault(cell[1], []).append(cell)
        return [rows[r] for r in sorted(rows)]

    def _need(self):
        f = n = 0
        for row in self._rows():
            f += max(need(c[0])[0] for c in row)
            n += max(need(c[0])[1] for c in row)
        return (f, n)

    def _place(self, x, y, w, h, out):
        rows = self._rows()
        if not rows:
            return
        ncols = max(c[2] + c[4] for r in rows for c in r)
        heights = [(max(need(c[0])[0] for c in r), max(need(c[0])[1] for c in r)) for r in rows]
        flex_left = sum(n for _, n in heights)
        remaining = max(0, h - sum(f for f, _ in heights))
        for row, (f, n) in zip(rows, heights):
            add = 0
            if n:
                add = remaining * n // flex_left
                remaining -= add
                flex_left -= n
            for item, _, col, _, cs in row:
                x0 = w * col // ncols
                x1 = w * (col + cs) // ncols
                place(item, x + x0, y, x1 - x0, f + add, out)
            y += f + add
