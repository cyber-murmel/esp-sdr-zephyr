# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/micropython/modules/gnuradio/_fpga.py: which sinks get the MCH2022 FPGA and what it is
told, against a stand-in fpga module."""

import sys
import types

import pytest

from conftest import load


class FakeFPGA:
    FFT_MAX = 512
    PALETTE_SIZE = 248
    PAL_BG = 248
    PAL_TRACE = 249
    PAL_CENTER = 250

    def __init__(self):
        self.calls = []

    def ok(self):
        return True

    def __getattr__(self, name):
        def call(*args):
            self.calls.append((name, args))
        return call


# The stand-in _gui.PALETTE: a waterfall_sink_c starts with it, set_gamma() gives it its own.
GUI_PALETTE = list(range(64))


class Sink:
    """What _fpga.py reads of a qtgui sink (the real ones answer every name: no hasattr)."""

    def __init__(self, accel, size=1024, win=5, area=(0, 30, 320, 100), palette=GUI_PALETTE):
        self._accel = accel
        self._size = size
        self._win = win
        self._avg = 1.0
        self._fpga_area = area
        self._imin, self._imax = -90.0, -40.0
        self._ymin, self._ymax = -120.0, 0.0
        self._palette = palette


@pytest.fixture
def fpga(monkeypatch):
    hw = FakeFPGA()
    monkeypatch.setitem(sys.modules, "fpga", hw)
    gui = types.ModuleType("gnuradio._gui")
    gui.PALETTE = GUI_PALETTE
    gui.BG, gui.DIM, gui.TRACE = 1, 2, (3,)
    pkg = types.ModuleType("gnuradio")
    pkg._gui = gui
    monkeypatch.setitem(sys.modules, "gnuradio", pkg)
    monkeypatch.setitem(sys.modules, "gnuradio._gui", gui)
    mod = load("apps/micropython/modules/gnuradio/_fpga.py", "gnuradio_fpga")
    return mod, hw


def test_one_waterfall_and_one_trace(fpga):
    mod, hw = fpga
    wf, tr = Sink("waterfall"), Sink("trace", area=(0, 140, 320, 60))
    assert mod.claim(wf)
    assert not mod.claim(Sink("waterfall"))           # the FPGA has one waterfall
    assert mod.claim(tr)
    assert not mod.claim(Sink("trace", size=256))     # and one FFT: same settings only
    mod.configure()
    names = [c[0] for c in hw.calls]
    assert names == ["palette", "waterfall", "spectrum", "dsp"]
    dsp = dict(hw.calls)["dsp"]
    # size, window, alpha, lines on demand, columns, waterfall and trace dB, trace rows
    assert dsp == (1024, 5, 1.0, 0, 320, -90.0, -40.0, 0.0, -120.0, 60)
    assert dict(hw.calls)["waterfall"] == wf._fpga_area


def test_palette_spreads_the_colours(fpga):
    mod, hw = fpga
    mod.claim(Sink("waterfall"))
    mod.configure()
    pal = dict(hw.calls)["palette"][0]
    assert pal[0] == 0 and pal[247] == 63
    assert (pal[248], pal[249], pal[250]) == (1, 3, 2)


def test_palette_is_the_waterfalls(fpga):
    mod, hw = fpga
    # A heat scale of its own (set_gamma()) reaches the FPGA.
    mod.claim(Sink("waterfall", palette=list(range(100, 164))))
    mod.configure()
    pal = dict(hw.calls)["palette"][0]
    assert pal[0] == 100 and pal[247] == 163


def test_palette_without_a_waterfall(fpga):
    mod, hw = fpga
    # A trace alone: the default scale (a real trace sink answers _palette with Nothing).
    mod.claim(Sink("trace", palette=list(range(100, 164))))
    mod.configure()
    pal = dict(hw.calls)["palette"][0]
    assert pal[0] == 0 and pal[247] == 63


def test_release_stops_and_frees(fpga):
    mod, hw = fpga
    wf = Sink("waterfall")
    mod.claim(wf)
    mod.release()
    assert ("stop", ()) in hw.calls
    assert mod.claim(Sink("waterfall"))
