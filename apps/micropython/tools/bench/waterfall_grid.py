# SPDX-License-Identifier: GPL-3.0-or-later
# The waterfall widget on the board, in the Waterfall app as the launcher starts it: the C picture
# against a reference, the picture through a retune by the real slider and back, every rate of the
# chooser, the grid's spacings, and what a redraw costs. Prints a line per check, then "WATERFALL
# ok" or "WATERFALL FAILED". HOLD=1 also compares the spectrum before and after a retune: anything
# on the air should stay on its frequency (needs a signal around the frequency).
import array
import gc
import sys
import time

sys.path[:0] = [".frozen", ".frozen/eggs/waterfall"]
import esdr  # noqa: E402
from gnuradio import _gui  # noqa: E402

HOLD = globals().get("HOLD", 0)
FAILED = []


def check(name, ok, detail=""):
    print("{} {} {}".format("ok  " if ok else "FAIL", name, detail))
    if not ok:
        FAILED.append(name)


def pump(s, rows):
    lv = _gui.lv
    for _ in range(rows):
        s._draw()
        s._canvas.invalidate()
        lv.task_handler()
        _gui._dispatch()
        time.sleep_ms(1)


def row(s, r):
    """A row of the composed plot, as bytes."""
    x, y, w, h = s._plot()
    return bytes(memoryview(s._buf)[(y + r) * s._w * 2:(y + r + 1) * s._w * 2])


def lean(px, colour):
    return ((px & 0xF7DE) >> 1) + ((colour & 0xF7DE) >> 1)


def kernel():
    """fb_wf_compose as built for this chip, against the rule, on small known buffers."""
    w, rows, stride, fill, coarse, fine = 16, 8, 21, 0x2088, 0xDEFB, 0x8C52
    seed = [12345]

    def next_():
        seed[0] = (seed[0] * 1103515245 + 12345) & 0x7FFFFFFF
        return seed[0] >> 8

    wrong = 0
    for trial in range(40):
        img = array.array("H", [next_() & 0xFFFF for _ in range(w * rows)])
        col0 = array.array("i", [(next_() % (3 * w)) - w - w // 2 for _ in range(rows)])
        marks = bytearray(next_() % 5 // 3 for _ in range(rows))
        lines = array.array("H", [(next_() % (w + 3)) | (next_() & 1) << 15
                                  for _ in range(next_() % 5)])
        head = next_() % rows
        view = (next_() % w) - w // 2
        fb = array.array("H", [0xAAAA] * (stride * rows))
        esdr.fb_wf_compose(fb, stride, 0, 0, w, rows, img, head, col0, marks, view, fill, lines,
                           coarse, fine)
        for k in range(rows):
            r = (head + k) % rows
            shift = col0[r] - view
            want = [img[r * w + c - shift] if 0 <= c - shift < w else fill for c in range(w)]
            if marks[r] == 2:
                want = [lean(p, coarse) for p in want]
            elif marks[r] == 1:
                want = [lean(p, fine) if c % 2 == 0 else p for c, p in enumerate(want)]
            for e in lines:
                c = e & 0x7FFF
                if c < w:
                    if e & 0x8000:
                        want[c] = lean(want[c], coarse)
                    elif k % 2 == 0:
                        want[c] = lean(want[c], fine)
            if list(fb[k * stride:k * stride + w]) != want:
                wrong += 1
            if any(p != 0xAAAA for p in fb[k * stride + w:(k + 1) * stride]):
                wrong += 1
    check("compose matches the reference", not wrong, "{} rows wrong".format(wrong))


def retune(tb, s):
    """1 MHz at 2 MS/s is half the plot, through the slider as the knob moves it, and back."""
    tb.set_samp_rate(2e6)
    s.enable_grid(False)
    pump(s, 40)
    w = s._w
    half = w // 2
    fill = bytes([s._palette[0] & 0xFF, s._palette[0] >> 8]) * half
    win = tb._freq_win
    i = win.index
    view = s._view
    composed = [row(s, r) for r in range(4)]
    ring = bytes(s._img)
    t0 = time.ticks_us()
    win.set_index(i + 1)
    t_slot = time.ticks_diff(time.ticks_us(), t0)
    check("the slider moved", win.index == i + 1)
    head = s._rhead
    t0 = time.ticks_us()
    s._apply()
    t_apply = time.ticks_diff(time.ticks_us(), t0)
    s._dirty = True  # as if _apply had not run; the view is the same either way
    pump(s, 1)
    check("view moved half a width", s._view - view == half, "{}".format(s._view - view))
    check("first row after the retune is unknown", row(s, 0)[half * 2:] == fill and
          row(s, 0)[:half * 2] == fill)
    good = 0
    for r in range(4):
        got = row(s, r + 1)
        if got[:half * 2] == composed[r][half * 2:] and got[half * 2:] == fill:
            good += 1
    check("rows moved left, 0 power on the new side", good == 4, "{}/4".format(good))
    same = True
    for r in range(s._rows):
        if r != s._rhead and r != head:
            a = r * w * 2
            if bytes(memoryview(s._img)[a:a + w * 2]) != bytes(memoryview(ring)[a:a + w * 2]):
                same = False
    check("the retune changed no stored row", same)
    win.set_index(i)
    pump(s, 1)
    check("view back", s._view == view)
    good = sum(1 for r in range(4) if row(s, r + 2) == composed[r])
    check("a tune back shows the same rows", good == 4, "{}/4".format(good))
    print("     slot {} us, apply {} us".format(t_slot, t_apply))
    s.enable_grid(True)


def rates(tb, s):
    src = tb.soapy_hackrf_source_0
    want = sorted(tb._samp_rate_options)
    check("chooser sorted, with 40 MS/s", list(tb._samp_rate_options) == want and 40e6 in want)
    for r in tb._samp_rate_options:
        tb.set_samp_rate(r)
        pump(s, 4)
        before = esdr.rx_latest(s._src)
        t0 = time.ticks_ms()
        pump(s, 20)
        ms = time.ticks_diff(time.ticks_ms(), t0)
        moved = (esdr.rx_latest(s._src) - before) & 0xFFFFFFFF
        check("{} S/s".format(int(r)), s._span == int(r) and len(s._lines) > 0 and moved > 0,
              "radio {} rows/s {:.1f} lines {} coarse {} samples {} title{}".format(
                  src._rate, 20000 / ms, len(s._lines),
                  sum(1 for x in s._lines if x & 0x8000), moved, s._notes))
        if int(r) == 40000000:
            # A display's bursts come per read: back to back they starved the display (3.6 rows/s).
            bursts = moved / 8190
            check("a burst per read at 40 MS/s", bursts <= 30, "{:.0f} in 20 rows".format(bursts))
        # Where the picture sits on the colour scale: the middle and the top of a held spectrum.
        held = sorted(maxhold(s, 10))
        print("     levels: median {:.0f} dB, top {:.0f} dB, scale {:.0f} to {:.0f} dB".format(
            held[len(held) // 2], held[-1], s._imin, s._imax))
    tb.set_samp_rate(2e6)
    pump(s, 2)


def spacing(tb, s):
    s.set_grid_spacing(100e3, 1e6, 0.5, 2)
    pump(s, 2)
    check("given spacing", s._notes == " 100k/1MHz 500m/2s" and s._time.fine == 500, s._notes)
    s.set_grid_spacing(0, 500e3, 0, 5)
    pump(s, 2)
    check("a level off", s._notes == " 500kHz 5s" and
          all(x & 0x8000 for x in s._lines), s._notes)
    t0 = time.ticks_us()
    s.set_grid_spacing(1, 1, 0, 0)
    pump(s, 2)
    ms = time.ticks_diff(time.ticks_us(), t0) // 1000
    check("a silly spacing draws no line and does not hang", len(s._lines) == 0 and ms < 2000,
          "{} ms".format(ms))
    s.set_grid_spacing()
    pump(s, 2)
    check("back to automatic", len(s._lines) == 20, s._notes)
    s.enable_grid(False)
    pump(s, 2)
    w = s._w
    a = s._rhead * w * 2
    check("grid off draws no line", len(s._lines) == 0 and not any(s._marks) and
          row(s, 0) == bytes(memoryview(s._img)[a:a + w * 2]))
    s.enable_grid(True)
    pump(s, 2)
    a = s._rhead * w * 2
    check("grid on leaves the rows alone", row(s, 0) != bytes(memoryview(s._img)[a:a + w * 2]))


def cost(tb, s):
    """What a row costs, by part, with the grid on and off, over 30 rows."""
    for grid in (True, False):
        s.enable_grid(grid)
        pump(s, 2)
        total = comp = lvgl = 0
        for _ in range(30):
            t0 = time.ticks_us()
            s._draw()
            t1 = time.ticks_us()
            s._canvas.invalidate()
            _gui.lv.task_handler()
            t2 = time.ticks_us()
            total += time.ticks_diff(t1, t0)
            lvgl += time.ticks_diff(t2, t1)
        t0 = time.ticks_us()
        s._compose()
        comp = time.ticks_diff(time.ticks_us(), t0)
        print("     grid {}: draw {} us, LVGL {} us; the compose alone {} us".format(
            "on " if grid else "off", total // 30, lvgl // 30, comp))
    s.enable_grid(True)
    print("     plot {}x{}, {} frequency lines, free heap {}".format(
        s._w, s._rows, len(s._lines), gc.mem_free()))


def cost_at(tb, s, rate):
    """Where the time goes at a rate of the chooser: the spectrum, the rest of a redraw, LVGL,
    and whatever else ran (a burst's work on the radio link) in between."""
    tb.set_samp_rate(rate)
    pump(s, 6)
    draw = lvgl = 0
    t_all = time.ticks_us()
    for _ in range(20):
        t0 = time.ticks_us()
        s._draw()
        t1 = time.ticks_us()
        s._canvas.invalidate()
        _gui.lv.task_handler()
        t2 = time.ticks_us()
        draw += time.ticks_diff(t1, t0)
        lvgl += time.ticks_diff(t2, t1)
    span = time.ticks_diff(time.ticks_us(), t_all)
    t0 = time.ticks_us()
    for _ in range(5):
        esdr.rx_latest(s._src)
    latest = time.ticks_diff(time.ticks_us(), t0) // 5
    print("     {} S/s: a row {} us: draw {} us, LVGL {} us; rx_latest {} us".format(
        int(rate), span // 20, draw // 20, lvgl // 20, latest))


def maxhold(s, rows):
    out = array.array("f", [-200.0] * s._w)
    for _ in range(rows):
        pump(s, 1)
        cols = s._cols
        for c in range(s._w):
            if cols[c] > out[c]:
                out[c] = cols[c]
    return out


def score(a, b, lag):
    # Mean difference of a against b moved left by lag columns; the plot's edges are the filter's.
    w = len(a)
    margin = w // 10
    n = t = 0
    for c in range(margin, w - margin):
        k = c + lag
        if margin <= k < w - margin:
            t += abs(a[c] - b[k])
            n += 1
    return t / n if n else 999.0


def hold(tb, s):
    """Anything on the air stays on its frequency. At 80 MS/s the rows are the channelizer's bursts
    over flat noise, without the receiver's own shape: the max-hold after a 10 MHz retune should
    match the one before it best 90 columns (at 720) to the left, and not where it was."""
    tb.set_samp_rate(80e6)
    win = tb._freq_win
    i = win.index
    pump(s, 10)
    a = maxhold(s, 80)
    win.set_index(i + 10)
    pump(s, 10)
    b = maxhold(s, 80)
    win.set_index(i)
    want = -10 * s._w // 80
    results = [(score(a, b, lag), lag) for lag in range(want - 40, 41, 2)]
    results.sort()
    print("     want lag {}; best {}".format(want, results[:4]))
    print("     at the wanted lag {:.2f}, at lag 0 {:.2f}".format(score(a, b, want),
                                                                   score(a, b, 0)))
    print("     spread before {:.1f} dB, after {:.1f} dB".format(max(a) - min(a), max(b) - min(b)))
    tb.set_samp_rate(2e6)


def run_all():
    if _gui.lv is None or _gui._head is None:
        _gui.show()
    tb = _gui.root
    s = tb.qtgui_waterfall_sink_x_0
    try:
        check("the export turned the grid on", s._grid is True)
        kernel()
        cost(tb, s)
        retune(tb, s)
        rates(tb, s)
        for rate in (250e3, 2e6, 40e6, 80e6):
            cost_at(tb, s, rate)
        tb.set_samp_rate(2e6)
        spacing(tb, s)
        if HOLD:
            hold(tb, s)
    except Exception as e:
        sys.print_exception(e)
        FAILED.append("exception")
    _gui._close_window()
    print("WATERFALL", "FAILED {}".format(FAILED) if FAILED else "ok")
    return 0


_gui.run = run_all
__import__("eggs.waterfall")
