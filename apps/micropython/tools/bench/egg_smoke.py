# SPDX-License-Identifier: GPL-3.0-or-later
# A bundled app (EGG, an eggs/ folder) for SECS seconds, imported as the launcher imports it,
# then the frames each display band drew. Prints "SMOKE <egg> ok" or the exception.
import sys
import time

EGG = globals().get("EGG", "spectrum")
SECS = globals().get("SECS", 6)

sys.path[:0] = [".frozen", ".frozen/eggs/" + EGG]
from gnuradio import _gui

t0 = time.ticks_ms()


def stop():
    if time.ticks_diff(time.ticks_ms(), t0) > SECS * 1000:
        _gui.quit()


run = _gui.run


def timed_run():
    _gui.poll(stop)
    return run()


_gui.run = timed_run
try:
    __import__("eggs." + EGG)
    print("SMOKE", EGG, "ok", _gui.stats().get("frames"))
except Exception as e:
    sys.print_exception(e)
    print("SMOKE", EGG, "FAILED")
