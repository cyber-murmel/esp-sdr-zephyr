# SPDX-License-Identifier: GPL-3.0-or-later
# A bundled app (EGG) for SECS seconds with every slider moved through its range part way (the
# values the knob gives, floats), as the launcher imports it. Prints "SLIDERS <egg> ok" or the
# exception.
import sys
import time

EGG = globals().get("EGG", "channelizer")
SECS = globals().get("SECS", 6)

sys.path[:0] = [".frozen", ".frozen/eggs/" + EGG]
from gnuradio import _gui

t0 = time.ticks_ms()
moved = []


def poll():
    t = time.ticks_diff(time.ticks_ms(), t0)
    for k, w in enumerate(v for v in _gui._vars if hasattr(v, "slot")):
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
    print("SLIDERS", EGG, "ok, moved", len(moved), _gui.stats().get("frames"))
except Exception as e:
    sys.print_exception(e)
    print("SLIDERS", EGG, "FAILED")
