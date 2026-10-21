# SPDX-License-Identifier: GPL-3.0-or-later
# A bundled app (EGG, an eggs/ folder) for SECS seconds, imported as the launcher imports it,
# with every slider moved part way through its range (the values the knob gives, floats; with
# SLIDERS=0 none, a smoke run), then the frames each display band drew. Prints "SLIDERS <egg>
# ok" ("SMOKE <egg> ok" without the sliders) or the exception.
import sys
import time

EGG = globals().get("EGG", "spectrum")
SECS = globals().get("SECS", 6)
SLIDERS = globals().get("SLIDERS", 1)
TAG = "SLIDERS" if SLIDERS else "SMOKE"

sys.path[:0] = [".frozen", ".frozen/eggs/" + EGG]
from gnuradio import _gui, qtgui

t0 = time.ticks_ms()
moved = []


def poll():
    t = time.ticks_diff(time.ticks_ms(), t0)
    ranges = [v for v in _gui._vars if isinstance(v, qtgui.RangeWidget)] if SLIDERS else []
    for k, w in enumerate(ranges):
        # Each slider in turn to a quarter, then three quarters of its range.
        for step, frac in ((1, 0.25), (2, 0.75)):
            if t > 1000 * step + 300 * k and (k, step) not in moved:
                moved.append((k, step))
                w.slot(w.type(w.r.min + frac * (w.r.max - w.r.min)))
    if t > SECS * 1000:
        _gui.quit()


run = _gui.run


def timed_run():
    _gui.poll(poll)
    return run()


_gui.run = timed_run
try:
    __import__("eggs." + EGG)
    print(TAG, EGG, "ok, moved", len(moved), _gui.stats().get("frames"))
except Exception as e:
    sys.print_exception(e)
    print(TAG, EGG, "FAILED")
