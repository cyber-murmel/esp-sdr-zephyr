# SPDX-License-Identifier: GPL-3.0-or-later
"""Bursts: the WHY2025 badge's ESP32-C6 watches the whole 80 MHz around the tuned frequency
without a gap and sends the 10 us windows that stand out over its quiet power, as many as the
link budget holds (esdr.gapless_start()); the P4 channelizes them (a polyphase filterbank of
40 channels of 2 MHz, the BLE grid at 2.44 GHz) and a detector per channel opens on a burst and
closes after it (esdr.chan_power(), esdr.chan_bursts()).

Three bands: each channel's mean power (blue), its floor (green) and its loudest block (red)
over the last tenth of a second, the channels' loudest blocks over time (newest on top), and
the bursts that ended, newest first: offset from the centre, length, level. The knob tunes,
sets the detector's opening level over each channel's floor and the top of the colour scale.
The header counts the bursts and shows the channelizer's load."""

import array
import time

import esdr
from gnuradio import _gui, qtgui
from PyQt5 import QtCore

CHANNELS = 40
# Bytes a second the radio may send: some 1 M samples, a fifth of the P4's channelizer; its
# threshold no lower than RATIO over the quiet power (its servo raises it until the loudest
# windows fit): quiet windows come too, the detector's floors learn from them.
BUDGET = 2000000
RATIO = 2
LIST = 24


class _Channels:
    """The channelizer's numbers, taken ten times a second."""

    def __init__(self):
        z = bytes(4 * CHANNELS)
        self.power = array.array("f", z)
        self.floor = array.array("f", z)
        self.peak = array.array("f", z)
        self.open = array.array("f", z)
        self.blocks = 0
        self.bursts = []  # newest last
        self.count = 0
        self.rows = []  # loudest blocks for the occupancy band, oldest first
        self.fresh = [False, False, False]
        self.level = -30
        self.load = ""
        self._t = time.ticks_ms()
        self._st = 0

    def poll(self):
        now = time.ticks_ms()
        if time.ticks_diff(now, self._t) < 100:
            return
        self._t = now
        self.blocks = esdr.chan_power(self.power, self.floor, self.peak, self.open)
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


class _Band(qtgui._sink):
    _draws = True

    def __init__(self, ch, k, title, update):
        super().__init__("bursts", "c", title)
        self.ch = ch
        self.k = k
        self._update = update
        self.cols = array.array("f", [0.0] * _gui.W)

    def _columns(self, w):
        self.cols = array.array("f", [0.0] * w)


class _Power(_Band):
    """Each channel's mean power, floor and loudest block over the last tenth of a second."""

    def _label(self):
        c = self.ch
        return "{} {}  {} bursts, {}".format(self._title, _gui.eng(_centre[0], "Hz"), c.count,
                                             c.load)

    def _draw(self):
        c = self.ch
        if not c.fresh[self.k]:
            return
        c.fresh[self.k] = False
        self._head()
        fb, buf = self._fbo, self._buf
        x, y, w, h = self._plot()
        lo, hi = c.level - 70, c.level + 10
        fb.fill_rect(x, y, w, h, _gui.BG)
        for k in range(1, 4):
            fb.vline(x + k * w // 4, y, h, _gui.DIM)
        for vec, colour in ((c.floor, _gui.TRACE[2]), (c.power, _gui.TRACE[0]),
                            (c.peak, _gui.TRACE[1])):
            esdr.stretch(vec, self.cols)
            esdr.fb_trace(buf, self._w, x, y, w, h, self.cols, lo, hi, colour)


class _Occupancy(_Band):
    """The channels' loudest blocks, newest on top."""

    def _draw(self):
        c = self.ch
        x, y, w, h = self._plot()
        while c.rows:
            esdr.stretch(c.rows.pop(0), self.cols)
            esdr.fb_scroll(self._buf, self._w, x, y, w, h)
            esdr.fb_wf_row(self._buf, self._w, x, y, w, 1, self.cols, c.level - 50, c.level,
                           _gui.PALETTE)


class _List(_Band):
    """The bursts that ended, newest first: offset, length, level."""

    def _draw(self):
        c = self.ch
        if not c.fresh[self.k]:
            return
        c.fresh[self.k] = False
        fb = self._fbo
        x, y, w, h = self._plot()
        fb.fill_rect(x, y, w, h, _gui.BG)
        rows = h // 10
        for r, (off, start, length, peak) in enumerate(reversed(c.bursts[-rows:])):
            fb.text("{:+6.1f} MHz {:8.1f} us {:6.1f} dB".format(off / 1e6, length * 1e6, peak),
                    x + 2, y + 1 + 10 * r, _gui.FG if peak > c.level - 50 else _gui.DIM)


_centre = [0.0]
_state = {"freq": 2.44e9, "open": 6.0}


def _restart():
    esdr.gapless_stop()
    _centre[0] = esdr.tune(_state["freq"], offset=False)
    esdr.gapless_start(ratio=RATIO, budget=BUDGET, monitor=False, channels=CHANNELS,
                       open_db=_state["open"])


def main():
    _gui.reset()
    _gui.title("Bursts")
    esdr.init()
    esdr.channel_bw(2)
    esdr.rx_gain(30)
    ch = _Channels()

    def set_freq(f):
        _state["freq"] = f
        _restart()

    def set_open(t):
        _state["open"] = t
        _restart()

    def set_level(v):
        ch.level = v

    qtgui.RangeWidget(qtgui.Range(1.8e9, 2.8e9, 1e6, _state["freq"], 200), set_freq,
                      "Frequency", "counter_slider", float, QtCore.Qt.Horizontal)
    qtgui.RangeWidget(qtgui.Range(3, 20, 1, _state["open"], 200), set_open, "Opening dB",
                      "counter_slider", float, QtCore.Qt.Horizontal)
    qtgui.RangeWidget(qtgui.Range(-80, 0, 1, ch.level, 200), set_level, "Level dB",
                      "counter_slider", float, QtCore.Qt.Horizontal)
    _Power(ch, 0, "Channels", 0.1)
    _Occupancy(ch, 1, "Occupancy", 0.1)
    _List(ch, 2, "Bursts", 0.2)
    _restart()
    _gui.poll(ch.poll)
    _gui.show()
    try:
        _gui.run()
    finally:
        esdr.gapless_stop()
