# SPDX-License-Identifier: GPL-3.0-or-later
# A bundled app (EGG) for SECS seconds on the MCH2022 badge's FPGA: the gateware's frames,
# lines, renders and dropped samples meanwhile, and how often each STATUS value was seen (bit 3
# set for long: the renderer holds the bus).
import sys
import time

EGG = globals().get("EGG", "spectrum")
SECS = globals().get("SECS", 6)
sys.path[:0] = [".frozen", ".frozen/eggs/" + EGG]
import fpga
from gnuradio import _gui

STATUS = 0x04
t0 = time.ticks_ms()
c0 = fpga.counters()
hist = {}


def stop():
    st = fpga.reg(STATUS)
    hist[st] = hist.get(st, 0) + 1
    if time.ticks_diff(time.ticks_ms(), t0) > SECS * 1000:
        c = fpga.counters()
        print("FPGA frames lines renders dropped", [(b - a) & 0xFFFF for a, b in zip(c0, c)])
        print("STATUS", hist)
        _gui.quit()


run = _gui.run


def timed_run():
    _gui.poll(stop)
    return run()


_gui.run = timed_run
__import__("eggs." + EGG)
print("EGG", EGG, _gui.stats())
