# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/micropython/eggs/bursts: the hand-written Bursts app with the board's gnuradio stand-ins
on a fake esdr module, its GUI loop replaced by a script: the gapless stream first where the
radio has it, the triggered snapshots on a radio that refuses it (a C6 not built for it) or
lacks it (the MCH2022 badge's ESP32), and the chooser between the two where both work."""

import importlib
import sys
import time
import types

import pytest

from conftest import REPO

MODULES = REPO / "apps/micropython/modules"
EGG = REPO / "apps/micropython/eggs/bursts"
OWN = ("gnuradio", "pmt", "PyQt5", "sip", "packaging", "esdr", "framebuf", "bursts")


class FakeRadio(types.ModuleType):
    """esdr as the app uses it: what runs, and the numbers it reads."""

    TRIG_SAMPLES = 16320
    TRIG_RATE = 80000000

    def __init__(self, gapless=True, refuse=False):
        super().__init__("esdr")
        self.running = None
        self.calls = []
        self.refuse = refuse
        if gapless:
            self.gapless_start = self._gapless_start
        self.snap = 0

    def init(self):
        pass

    def channel_bw(self, mode):
        pass

    def rx_gain(self, g):
        pass

    def tune(self, hz, offset=True):
        return hz

    def _gapless_start(self, **kw):
        if self.refuse:
            raise OSError(95)
        self.running = "gapless"
        self.calls.append(("gapless", kw))

    def gapless_stop(self):
        if self.running == "gapless":
            self.running = None

    def trig_start(self, ratio=8):
        self.running = "trig"
        self.calls.append(("trig", ratio))

    def trig_stop(self):
        if self.running == "trig":
            self.running = None

    def chan_power(self, power, floor, peak, open_):
        for k in range(len(power)):
            power[k], floor[k], peak[k] = -60.0, -70.0, -40.0
        return 1

    def chan_bursts(self):
        return [(1e6, 0.0, 1e-4, -40.0)]

    def gapless_stats(self):
        return {"chan_pairs": 100, "chan_cycles": 9000, "chan_dropped": 0}

    def trig_get(self, buf, after):
        self.snap += 1
        return (self.snap, 8000) if self.snap != after else None

    def channelize(self, snap, nfft, mean, peak, image, lo, hi):
        for k in range(nfft):
            mean[k], peak[k] = -70.0, -50.0
        return len(snap) // 2 // nfft


@pytest.fixture
def app(monkeypatch):
    def make(**kw):
        radio = FakeRadio(**kw)
        saved = {k: v for k, v in sys.modules.items() if k.split(".")[0] in OWN}
        for k in saved:
            del sys.modules[k]
        made.append(saved)
        framebuf = types.ModuleType("framebuf")
        framebuf.RGB565 = 1
        framebuf.FrameBuffer = object
        monkeypatch.setitem(sys.modules, "esdr", radio)
        monkeypatch.setitem(sys.modules, "framebuf", framebuf)
        clock = [1000]
        monkeypatch.setattr(time, "ticks_ms", lambda: clock[0], raising=False)
        monkeypatch.setattr(time, "ticks_add", lambda a, b: a + b, raising=False)
        monkeypatch.setattr(time, "ticks_diff", lambda a, b: a - b, raising=False)
        monkeypatch.syspath_prepend(str(EGG))
        monkeypatch.syspath_prepend(str(MODULES))
        gui = importlib.import_module("gnuradio._gui")
        gui._headless = True
        return types.SimpleNamespace(radio=radio, gui=gui, mod=importlib.import_module("bursts"),
                                     clock=clock)

    made = []
    yield make
    for saved in made:
        for k in [k for k in sys.modules if k.split(".")[0] in OWN]:
            del sys.modules[k]
        sys.modules.update(saved)


def chooser(gui):
    from PyQt5 import Qt

    bars = [v for v in gui._vars if isinstance(v, Qt.QToolBar)]
    return bars[0]._widgets[1] if bars else None


def sliders(gui):
    from gnuradio import qtgui

    return [v.label for v in gui._vars if isinstance(v, qtgui.RangeWidget)]


def passes(a, n=3):
    """Passes of the GUI loop: the pollers, 100 ms apart."""
    for _ in range(n):
        a.clock[0] += 100
        for fn in a.gui._pollers[:]:
            fn()


def test_gapless_first_then_snapshots_by_the_chooser(app, monkeypatch):
    a = app()
    seen = []

    def run():
        passes(a)
        seen.append((a.radio.running, sliders(a.gui), [type(s).__name__ for s in a.gui._sinks]))
        box = chooser(a.gui)
        assert box is not None and box._items == ["Gapless", "Snapshots"]
        if len(seen) == 1:
            box._picked(1)  # the user picks Snapshots: the loop ends, the GUI is built again
            assert a.gui._quit
        return 0

    monkeypatch.setattr(a.gui, "run", run)
    a.mod.main()
    assert seen == [
        ("gapless", ["Frequency", "Opening dB", "Level dB"], ["_Power", "_Occupancy", "_List"]),
        ("trig", ["Frequency", "Threshold dB", "Level dB"], ["_Power", "_Occupancy", "_Burst"]),
    ]
    assert a.radio.running is None
    assert a.radio.calls[0] == ("gapless", {"ratio": 2, "budget": 2000000, "monitor": False,
                                            "channels": 40, "open_db": 6.0})
    assert a.radio.calls[-1] == ("trig", pytest.approx(10 ** 0.9))


@pytest.mark.parametrize("kw", [{"refuse": True}, {"gapless": False}])
def test_snapshots_without_gapless(app, monkeypatch, kw):
    a = app(**kw)
    seen = []

    def run():
        passes(a)
        src = a.gui._sinks[0].src
        seen.append((a.radio.running, chooser(a.gui), src.fresh, src.label()))
        return 0

    monkeypatch.setattr(a.gui, "run", run)
    a.mod.main()
    assert seen == [("trig", None, [True, True, True], "#3 (3 shown)")]
    assert a.radio.running is None


def test_threshold_slider_restarts_the_source(app, monkeypatch):
    a = app()

    def run():
        from gnuradio import qtgui

        thr = [v for v in a.gui._vars if isinstance(v, qtgui.RangeWidget)][1]
        thr.slot(12.0)
        assert a.radio.running == "gapless"
        assert a.radio.calls[-1][1]["open_db"] == 12.0
        passes(a)
        assert a.gui._sinks[0].src.label().startswith("3 bursts, ")
        return 0

    monkeypatch.setattr(a.gui, "run", run)
    a.mod.main()
