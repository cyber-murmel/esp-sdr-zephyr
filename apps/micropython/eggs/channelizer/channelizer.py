# SPDX-License-Identifier: GPL-3.0-or-later
"""Channelizer: the WHY2025 badge's ESP32-C6 watches the power of 80 MHz around the tuned
frequency and sends a snapshot of 205 us at 80 MS/s (esdr.trig_get()) whenever a burst rises
past the threshold; the P4 cuts each into 64 channels of 1.25 MHz (esdr.channelize()).

Three bands: each channel's peak (blue) and mean (red) power in the newest snapshot, the
channels' peaks over time (a row per snapshot shown, newest on top), and the newest snapshot as
time (top to bottom, 205 us, the line marks the trigger) by frequency. The knob tunes, sets the
threshold over the quiet power and the top of the colour scale. The bands redraw ten times a
second; the radio sends some tens of snapshots a second in a busy band, and the header counts
them (#) and those shown."""

import array

import esdr
from gnuradio import _gui, qtgui
from PyQt5 import QtCore

NFFT = 64
ROWS = 64
SPAN = esdr.TRIG_RATE


class _Snapshots:
    """The newest snapshot, channelized; trig_get() at every pass of the loop."""

    def __init__(self):
        self.buf = bytearray(2 * esdr.TRIG_SAMPLES)
        self.mean = array.array("f", bytes(4 * NFFT))
        self.peak = array.array("f", bytes(4 * NFFT))
        self.image = bytearray(ROWS * NFFT)
        self.seq = 0
        self.trig = 0
        self.taken = 0
        self.level = -30
        self.fresh = [False, False, False]  # per band: not drawn yet
        self.rows = []  # peaks for the occupancy band, oldest first

    def poll(self):
        r = esdr.trig_get(self.buf, self.seq)
        if r is None:
            return
        self.seq, self.trig = r
        self.taken += 1
        esdr.channelize(self.buf, NFFT, self.mean, self.peak, self.image,
                        self.level - 50, self.level)
        self.rows.append(array.array("f", self.peak))
        del self.rows[:-8]
        self.fresh = [True, True, True]


class _Band(qtgui._sink):
    _draws = True

    def __init__(self, snaps, k, title, update):
        super().__init__("channelizer", "c", title)
        self.snaps = snaps
        self.k = k
        self._update = update
        self.cols = array.array("f", [0.0] * _gui.W)

    def _columns(self, w):
        self.cols = array.array("f", [0.0] * w)


class _Power(_Band):
    """Channel power in the newest snapshot: peak and mean."""

    def _label(self):
        s = self.snaps
        return "{} {}  #{} ({} shown)".format(self._title, _gui.eng(_centre[0], "Hz"), s.seq,
                                              s.taken)

    def _draw(self):
        s = self.snaps
        if not s.fresh[self.k]:
            return
        s.fresh[self.k] = False
        self._head()
        fb, buf = self._fbo, self._buf
        x, y, w, h = self._plot()
        lo, hi = s.level - 60, s.level + 10
        fb.fill_rect(x, y, w, h, _gui.BG)
        for k in range(1, 4):
            fb.vline(x + k * w // 4, y, h, _gui.DIM)
        for vec, colour in ((s.mean, _gui.TRACE[1]), (s.peak, _gui.TRACE[0])):
            esdr.stretch(vec, self.cols)
            esdr.fb_trace(buf, self._w, x, y, w, h, self.cols, lo, hi, colour)


class _Occupancy(_Band):
    """Each snapshot's channel peaks, newest on top."""

    def _draw(self):
        s = self.snaps
        x, y, w, h = self._plot()
        while s.rows:
            esdr.stretch(s.rows.pop(0), self.cols)
            esdr.fb_scroll(self._buf, self._w, x, y, w, h)
            esdr.fb_wf_row(self._buf, self._w, x, y, w, 1, self.cols, s.level - 50, s.level,
                           _gui.PALETTE)


class _Burst(_Band):
    """The newest snapshot: time down, frequency across; the line marks the trigger."""

    def _draw(self):
        s = self.snaps
        if not s.fresh[self.k]:
            return
        s.fresh[self.k] = False
        x, y, w, h = self._plot()
        esdr.fb_image(self._buf, self._w, x, y, w, h, s.image, NFFT, _gui.PALETTE)
        self._fbo.hline(x, y + s.trig * h // esdr.TRIG_SAMPLES, w, _gui.FG)


_centre = [0.0]
_state = {"freq": 2.44e9, "thr": 9.0}


def _restart():
    esdr.trig_stop()
    _centre[0] = esdr.tune(_state["freq"], offset=False)
    esdr.trig_start(ratio=10 ** (_state["thr"] / 10))


def main():
    _gui.reset()
    _gui.title("Channelizer")
    esdr.init()
    esdr.channel_bw(2)
    esdr.rx_gain(30)
    snaps = _Snapshots()

    def set_freq(f):
        _state["freq"] = f
        _restart()

    def set_thr(t):
        _state["thr"] = t
        _restart()

    def set_level(v):
        snaps.level = v

    qtgui.RangeWidget(qtgui.Range(1.8e9, 2.8e9, 1e6, _state["freq"], 200), set_freq,
                      "Frequency", "counter_slider", float, QtCore.Qt.Horizontal)
    qtgui.RangeWidget(qtgui.Range(3, 30, 1, _state["thr"], 200), set_thr, "Threshold dB",
                      "counter_slider", float, QtCore.Qt.Horizontal)
    qtgui.RangeWidget(qtgui.Range(-80, 0, 1, snaps.level, 200), set_level, "Level dB",
                      "counter_slider", float, QtCore.Qt.Horizontal)
    _Power(snaps, 0, "Channels", 0.1)
    _Occupancy(snaps, 1, "Occupancy", 0.1)
    _Burst(snaps, 2, "Burst {} us".format(esdr.TRIG_SAMPLES * 1000000 // SPAN), 0.1)
    _restart()
    _gui.poll(snaps.poll)
    _gui.show()
    try:
        _gui.run()
    finally:
        esdr.trig_stop()
