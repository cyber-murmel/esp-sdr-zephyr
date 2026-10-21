# SPDX-License-Identifier: GPL-3.0-or-later
"""Bursts: 80 MHz around the tuned frequency in channels, from either of two sources.

Gapless (esdr.gapless_start(), the WHY2025 badge's ESP32-C6): the C6 watches the whole 80 MHz
without a gap and sends the 10 us windows that stand out over its quiet power, as many as the
link budget holds; the P4 channelizes them (a polyphase filterbank of 40 channels of 2 MHz, the
BLE grid at 2.44 GHz) and a detector per channel opens on a burst and closes after it
(esdr.chan_power(), esdr.chan_bursts()). Three bands: each channel's mean power (blue), its
floor (green) and its loudest block (red) over the last tenth of a second, the channels'
loudest blocks over time (newest on top), and the bursts that ended, newest first: offset from
the centre, length, level. The second slider sets the detector's opening level over each
channel's floor; the header counts the bursts and shows the channelizer's load.

Snapshots (esdr.trig_start()): the radio sends a snapshot of 205 us at 80 MS/s whenever a burst
rises past the threshold over the quiet power (the WHY2025 badge's C6 watches continuously, the
MCH2022 badge's ESP32 checks a burst every few milliseconds), and each is cut into 64 channels of
1.25 MHz (esdr.channelize()). Three bands: each channel's peak (blue) and mean (red) power in
the newest snapshot, the channels' peaks over time (a row per snapshot shown, newest on top),
and the newest snapshot as time (top to bottom, the line marks the trigger) by frequency. The
second slider sets the threshold; the radio sends some tens of snapshots a second in a busy
band, the header counts them (#) and those shown.

Gapless where the radio has it, else snapshots; with both, a chooser switches between them. The
knob tunes and sets the top of the colour scale."""

import array
import time

import esdr
from gnuradio import _gui, qtgui
from PyQt5 import Qt, QtCore

LIST = 24


class _Gapless:
    """The channelizer's numbers, taken ten times a second."""

    NAME = "Gapless"
    THRESHOLD = ("Opening dB", 3, 20)
    SPAN = 70  # the power band's range below the level
    CHANNELS = 40
    # Bytes a second the radio may send: some 1 M samples, a fifth of the P4's channelizer;
    # its threshold no lower than RATIO over the quiet power (its servo raises it until the
    # loudest windows fit): quiet windows come too, the detector's floors learn from them.
    BUDGET = 2000000
    RATIO = 2

    def __init__(self):
        z = bytes(4 * self.CHANNELS)
        self.power = array.array("f", z)
        self.floor = array.array("f", z)
        self.peak = array.array("f", z)
        self.open = array.array("f", z)
        self.thr = 6.0
        self.bursts = []  # newest last
        self.count = 0
        self.rows = []  # loudest blocks for the occupancy band, oldest first
        self.fresh = [False, False, False]
        self.load = ""
        self._t = time.ticks_ms()
        self._st = 0

    def start(self):
        esdr.gapless_start(ratio=self.RATIO, budget=self.BUDGET, monitor=False,
                           channels=self.CHANNELS, open_db=self.thr)

    def stop(self):
        esdr.gapless_stop()

    def poll(self):
        now = time.ticks_ms()
        if time.ticks_diff(now, self._t) < 100:
            return
        self._t = now
        esdr.chan_power(self.power, self.floor, self.peak, self.open)
        new = esdr.chan_bursts()
        self.count += len(new)
        self.bursts.extend(new)
        del self.bursts[:-LIST]
        self.rows.append(array.array("f", self.peak))
        del self.rows[:-8]
        self.fresh = [True, True, True]
        self._st += 1
        if self._st % 10 == 0:
            st = esdr.gapless_stats()
            pairs = st["chan_pairs"]
            self.load = "{:.0f} cyc/S, {} dropped".format(
                st["chan_cycles"] / pairs if pairs else 0, st["chan_dropped"])

    def label(self):
        return "{} bursts, {}".format(self.count, self.load)

    def traces(self):
        return ((self.floor, _gui.TRACE[2]), (self.power, _gui.TRACE[0]),
                (self.peak, _gui.TRACE[1]))

    def third(self):
        return _List(self, 2, "Bursts", 0.2)


class _Snapshots:
    """The newest snapshot, channelized; trig_get() at every pass of the loop."""

    NAME = "Snapshots"
    THRESHOLD = ("Threshold dB", 3, 30)
    SPAN = 60
    NFFT = 64
    ROWS = 64

    def __init__(self):
        self.buf = bytearray(2 * esdr.TRIG_SAMPLES)
        self.mean = array.array("f", bytes(4 * self.NFFT))
        self.peak = array.array("f", bytes(4 * self.NFFT))
        self.image = bytearray(self.ROWS * self.NFFT)
        self.thr = 9.0
        self.seq = 0
        self.trig = 0
        self.taken = 0
        self.fresh = [False, False, False]  # per band: not drawn yet
        self.rows = []  # peaks for the occupancy band, oldest first

    def start(self):
        esdr.trig_start(ratio=10 ** (self.thr / 10))

    def stop(self):
        esdr.trig_stop()

    def poll(self):
        r = esdr.trig_get(self.buf, self.seq)
        if r is None:
            return
        self.seq, self.trig = r
        self.taken += 1
        level = _state["level"]
        esdr.channelize(self.buf, self.NFFT, self.mean, self.peak, self.image, level - 50, level)
        self.rows.append(array.array("f", self.peak))
        del self.rows[:-8]
        self.fresh = [True, True, True]

    def label(self):
        return "#{} ({} shown)".format(self.seq, self.taken)

    def traces(self):
        return ((self.mean, _gui.TRACE[1]), (self.peak, _gui.TRACE[0]))

    def third(self):
        return _Burst(self, 2, "Burst {} us".format(esdr.TRIG_SAMPLES * 1000000 // esdr.TRIG_RATE),
                      0.1)


class _Band(qtgui._sink):
    _draws = True

    def __init__(self, src, k, title, update):
        super().__init__("bursts", "c", title)
        self.src = src
        self.k = k
        self._update = update
        self.cols = array.array("f", [0.0] * _gui.W)

    def _columns(self, w):
        self.cols = array.array("f", [0.0] * w)


class _Power(_Band):
    """Each channel's power: the source's traces."""

    def _label(self):
        return "{} {}  {}".format(self._title, _gui.eng(_centre[0], "Hz"), self.src.label())

    def _draw(self):
        s = self.src
        if not s.fresh[self.k]:
            return
        s.fresh[self.k] = False
        self._head()
        fb, buf = self._fbo, self._buf
        x, y, w, h = self._plot()
        level = _state["level"]
        fb.fill_rect(x, y, w, h, _gui.BG)
        for k in range(1, 4):
            fb.vline(x + k * w // 4, y, h, _gui.DIM)
        for vec, colour in s.traces():
            esdr.stretch(vec, self.cols)
            esdr.fb_trace(buf, self._w, x, y, w, h, self.cols, level - s.SPAN, level + 10, colour)


class _Occupancy(_Band):
    """The channels' loudest blocks or snapshot peaks, newest on top."""

    def _draw(self):
        s = self.src
        x, y, w, h = self._plot()
        level = _state["level"]
        while s.rows:
            esdr.stretch(s.rows.pop(0), self.cols)
            esdr.fb_scroll(self._buf, self._w, x, y, w, h)
            esdr.fb_wf_row(self._buf, self._w, x, y, w, 1, self.cols, level - 50, level,
                           _gui.PALETTE)


class _List(_Band):
    """The bursts that ended, newest first: offset, length, level."""

    def _draw(self):
        s = self.src
        if not s.fresh[self.k]:
            return
        s.fresh[self.k] = False
        fb = self._fbo
        x, y, w, h = self._plot()
        level = _state["level"]
        fb.fill_rect(x, y, w, h, _gui.BG)
        rows = h // 10
        for r, (off, start, length, peak) in enumerate(reversed(s.bursts[-rows:])):
            fb.text("{:+6.1f} MHz {:8.1f} us {:6.1f} dB".format(off / 1e6, length * 1e6, peak),
                    x + 2, y + 1 + 10 * r, _gui.FG if peak > level - 50 else _gui.DIM)


class _Burst(_Band):
    """The newest snapshot: time down, frequency across; the line marks the trigger."""

    def _draw(self):
        s = self.src
        if not s.fresh[self.k]:
            return
        s.fresh[self.k] = False
        x, y, w, h = self._plot()
        esdr.fb_image(self._buf, self._w, x, y, w, h, s.image, s.NFFT, _gui.PALETTE)
        self._fbo.hline(x, y + s.trig * h // esdr.TRIG_SAMPLES, w, _gui.FG)


_centre = [0.0]
_state = {"freq": 2.44e9, "level": -30}


def _restart(src):
    src.stop()
    _centre[0] = esdr.tune(_state["freq"], offset=False)
    src.start()


def _show(srcs, at, pick):
    """The GUI of the source srcs[at]; pick(i) when the chooser (with both) selects another."""
    src = srcs[at]

    def set_freq(f):
        _state["freq"] = f
        _restart(src)

    def set_thr(t):
        src.thr = t
        _restart(src)

    def set_level(v):
        _state["level"] = v

    _gui.reset()
    _gui.title("Bursts")
    if len(srcs) > 1:
        bar = Qt.QToolBar()
        bar.addWidget(Qt.QLabel("Source: "))
        box = Qt.QComboBox()
        bar.addWidget(box)
        for s in srcs:
            box.addItem(s.NAME)
        box.setCurrentIndex(at)
        box.currentIndexChanged.connect(pick)
        _gui.variable(bar)
    name, lo, hi = src.THRESHOLD
    qtgui.RangeWidget(qtgui.Range(1.8e9, 2.8e9, 1e6, _state["freq"], 200), set_freq,
                      "Frequency", "counter_slider", float, QtCore.Qt.Horizontal)
    qtgui.RangeWidget(qtgui.Range(lo, hi, 1, src.thr, 200), set_thr, name, "counter_slider",
                      float, QtCore.Qt.Horizontal)
    qtgui.RangeWidget(qtgui.Range(-80, 0, 1, _state["level"], 200), set_level, "Level dB",
                      "counter_slider", float, QtCore.Qt.Horizontal)
    _Power(src, 0, "Channels", 0.1)
    _Occupancy(src, 1, "Occupancy", 0.1)
    src.third()
    _gui.poll(src.poll)


def main():
    esdr.init()
    esdr.channel_bw(2)
    esdr.rx_gain(30)
    # Gapless first; a radio without the stream (a C6 not built for it) refuses it at the start.
    kinds = ([_Gapless] if hasattr(esdr, "gapless_start") else []) + [_Snapshots]
    srcs = [k() for k in kinds]
    at = [0]
    switch = [None]

    def pick(i):
        switch[0] = i
        _gui.quit()

    while True:
        src = srcs[at[0]]
        try:
            _restart(src)
        except OSError:
            if not isinstance(src, _Gapless):
                raise
            print("Bursts: the radio streams no gapless 80 MHz: snapshots")
            del srcs[at[0]]
            at[0] = 0
            continue
        _show(srcs, at[0], pick)
        _gui.show()
        switch[0] = None
        try:
            _gui.run()
        finally:
            src.stop()
        if switch[0] is None:
            break
        at[0] = switch[0]
