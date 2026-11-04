# SPDX-License-Identifier: GPL-3.0-or-later
"""qtgui for GRC Qt GUI exports on the board. freq_sink, waterfall_sink and time_sink draw
on the board's display (_gui.py); the other sinks accept their samples and draw nothing.
Range and RangeWidget become variables the encoder changes. Line colours, labels and other
styling calls are accepted and ignored, grids except a waterfall's and a frequency sink's. While the radio streams
gapless with the channelizer (soapy.source at 80 and 40 MS/s), the spectrum sinks draw the bursts
it found, each at its channel, over quiet noise (esdr.chan_spectrum()), instead of the latest
samples' spectrum."""

import array
import framebuf
import time

import dusk
import esdr
from gnuradio import _fpga, _gui, _wfgrid, gr, soapy
from PyQt5._nothing import Nothing

TRIG_MODE_FREE = 0
TRIG_MODE_AUTO = 1
TRIG_MODE_NORM = 2
TRIG_MODE_TAG = 3
TRIG_SLOPE_POS = 0
TRIG_SLOPE_NEG = 1
NUM_GRAPH_NONE = 0
NUM_GRAPH_HORIZ = 1
NUM_GRAPH_VERT = 2
INTENSITY_COLOR_MAP_TYPE_MULTI_COLOR = 0
INTENSITY_COLOR_MAP_TYPE_WHITE_HOT = 1
INTENSITY_COLOR_MAP_TYPE_BLACK_HOT = 2
INTENSITY_COLOR_MAP_TYPE_INCANDESCENT = 3
INTENSITY_COLOR_MAP_TYPE_USER_DEFINED = 4


class util:
    @staticmethod
    def check_set_qss():
        pass


def _itemsize(kind):
    return gr.sizeof_gr_complex if kind == "c" else gr.sizeof_float


class _sink(gr.basic_block):
    """A qtgui sink: a flowgraph sink that wants the monitor ring rather than a buffer."""

    _kind = "sink"
    _wants_monitor = True
    _draws = False
    _flex = True
    # The MCH2022 badge's FPGA can draw this sink's plot area as a "waterfall" or a "trace"
    # (_fpga.py).
    _accel = None
    _fpga = False

    def __init__(self, name, kind, title):
        super().__init__(name)
        if kind != "c":
            raise ValueError(name + ": only the complex (_c) sinks run on the board")
        self.in_itemsize = _itemsize(kind)
        self.out_itemsize = 0
        self._title = title or name
        self._update = 0.1
        self._h = 0
        self._w = _gui.W
        if self._draws:
            _gui.register(self)

    def _want_buffer(self):
        return False

    def qwidget(self):
        return self

    def set_update_time(self, t):
        self._update = max(0.05, float(t))

    def __getattr__(self, name):
        return Nothing()

    # Layout and drawing, called by _gui. The sink draws into its own RGB565 buffer, which an
    # lv.canvas shows at y, h rows high.
    def _place(self, x, y, w, h, out):
        if self._draws:
            out.append((self, x, y, w, h))

    def _need(self):
        return (0, 1) if self._draws else (0, 0)

    def _make(self, parent, x, y, w, h):
        import lvgl as lv

        self._w = w
        self._h = h
        self._fpga = self._accel is not None and _fpga.claim(self)
        # With the FPGA the canvas is only the label row; the FPGA draws the plot area.
        rows = LABEL if self._fpga else h
        self._buf = bytearray(w * rows * 2)
        self._fbo = framebuf.FrameBuffer(self._buf, w, rows, framebuf.RGB565)
        self._canvas = lv.canvas(parent)
        self._canvas.set_buffer(self._buf, w, rows, lv.COLOR_FORMAT.RGB565)
        self._canvas.set_pos(x, y)
        self._columns(w)
        if self._fpga:
            px, py, pw, ph = self._plot()
            self._fpga_area = (x + px, y + py, pw, ph)
            self._head()
            _fpga.configure()
        else:
            self._setup()

    def _columns(self, w):
        pass

    def _plot(self):
        return 0, LABEL, self._w, self._h - LABEL

    def _setup(self):
        self._fbo.fill(_gui.BG)
        self._head()

    def _head(self):
        fb = self._fbo
        fb.fill_rect(0, 0, self._w, LABEL, _gui.BG)
        fb.text(self._label()[: self._w // 8], 0, 1, _gui.FG)
        fb.hline(0, LABEL - 1, self._w, _gui.DIM)

    def _label(self):
        return self._title

    def _draw(self):
        pass


LABEL = _gui.LABEL

# Rows of the bursts' spectrum: the bins, their count (0: no channelizer), when they were taken.
# Each call takes what came since the last, so sinks drawing within _ROW_MS share one row.
_ROW_MS = 40
_row = [None, 0, 0]


def _composed(cols):
    """A row of the bursts' spectrum over cols; False without a channelizer."""
    if not hasattr(esdr, "chan_spectrum"):
        return False
    now = time.ticks_ms()
    if _row[0] is None:
        _row[0] = array.array("f", [0.0] * esdr.CHAN_SPEC_MAX)
    if not _row[1] or time.ticks_diff(now, _row[2]) >= _ROW_MS:
        _row[1] = esdr.chan_spectrum(_row[0])
        _row[2] = now
    if not _row[1]:
        return False
    esdr.stretch(memoryview(_row[0])[: _row[1]], cols)
    return True


def _fft_points(size):
    # The power of two at or above size, 16 to 4096.
    n = 16
    while n < int(size) and n < 4096:
        n *= 2
    return n


def _pair(fine, coarse, unit):
    # " 100k/500kHz", one value with one level, "" with none.
    parts = [_gui.eng(v) for v in (fine, coarse) if v]
    return " " + "/".join(parts) + unit if parts else ""


class _spectrum(_sink):
    """A sink of the radio's spectrum: its axis, its frequency lines and its FFT."""

    def __init__(self, name, kind, size, wintype, fc, bw, title):
        super().__init__(name, kind, title)
        self._size = _fft_points(size)
        self._win = int(wintype)
        self._fc = float(fc)
        self._bw = float(bw)
        self._avg = 1.0
        self._src = bytearray(4 * self._size)
        self._state = array.array("f", [0.0] * self._size)
        self._cols = array.array("f", [0.0] * _gui.W)
        # What the export asks for, integers before a float can round them; what the plot shows.
        self._hz = int(fc)
        self._asked = max(1, int(bw))
        self._span = self._asked
        self._dirty = True
        self._grid = False
        self._spacing = (None, None)
        self._lines = array.array("H")
        self._notes = ""

    def _columns(self, w):
        self._cols = array.array("f", [0.0] * w)
        self._dirty = True

    def enable_grid(self, on=True):
        self._grid = bool(on)
        self._dirty = True

    def set_frequency_range(self, fc, bw):
        # An export calls this before it tunes, and RangeWidget.set_index() takes any exception
        # for a refused tune and tries other values: this only notes the request, the next
        # _draw() applies the last one, which is the one the radio took.
        self._fc = float(fc)
        self._bw = float(bw)
        self._hz = int(fc)
        self._asked = max(1, int(bw))
        self._dirty = True
        if self._fpga:
            self._head()
            self._canvas.invalidate()
            _fpga.configure()

    def set_fft_size(self, n):
        """GRC has no callback for it: a flowgraph's Python Snippet connects a chooser here. At the
        rates the channelizer draws (esdr.chan_spectrum()) its bins set the resolution instead."""
        n = _fft_points(n)
        if n != self._size:
            self._size = n
            self._src = bytearray(4 * n)
            self._state = array.array("f", [0.0] * n)
            self._dirty = True
            if self._fpga:
                _fpga.configure()

    def _freq_grid(self):
        # The rows are the radio's: its rate, which can be below the one asked for (a chooser's
        # 2 MS/s on the S3 runs at 500 kS/s), sets the axis and the lines. (fine, coarse) in Hz.
        self._span = soapy.radio_rate() or self._asked
        fine = coarse = 0
        if self._grid:
            fine, coarse = _wfgrid.freq_levels(self._spacing[0], self._spacing[1], self._span,
                                               self._w)
        self._lines = _wfgrid.lines(self._hz, self._span, self._w, self._size, fine, coarse)
        return fine, coarse

    def set_fft_average(self, a):
        self._avg = float(a)
        if self._fpga:
            _fpga.configure()

    def _label(self):
        return "{} {}{}".format(self._title, _gui.eng(self._fc, "Hz"),
                                "" if self._fpga else self._notes)

    def _spectrum(self):
        """The columns of the latest spectrum; how many samples have come in all, or None for a
        row of the channelizer's."""
        if _composed(self._cols):
            return None
        total = esdr.rx_latest(self._src)
        esdr.psd(self._src, self._cols, self._state, self._avg, self._win)
        return total


class freq_sink_c(_spectrum):
    _draws = True
    _accel = "trace"

    def __init__(self, size, wintype, fc, bw, name, nconnections=1, parent=None):
        super().__init__("freq_sink_c", "c", size, wintype, fc, bw, name)
        self._ymin = -140.0
        self._ymax = 10.0

    def set_y_axis(self, ymin, ymax):
        self._ymin = float(ymin)
        self._ymax = float(ymax)
        if self._fpga:
            _fpga.configure()

    def set_grid_spacing(self, freq_fine=None, freq_coarse=None):
        """As waterfall_sink_c's, for frequency: its lines fall on the same frequencies."""
        self._spacing = (_wfgrid.spacing(freq_fine), _wfgrid.spacing(freq_coarse))
        self._dirty = True

    def _apply(self):
        self._dirty = False
        fine, coarse = self._freq_grid()
        self._notes = _pair(fine, coarse, "Hz")
        self._head()

    def _paint_grid(self, x, y, w, h):
        fb = self._fbo
        for line in self._lines:
            fb.vline(x + (line & ~_wfgrid.COARSE), y, h,
                     _gui.DIM if line & _wfgrid.COARSE else _gui.FAINT)
        # Fine lines every 10 dB where they fit, coarse ones every 20.
        range_db = self._ymax - self._ymin
        for step, colour in ((10.0, _gui.FAINT), (20.0, _gui.DIM)):
            if step * (h - 1) < _wfgrid.MIN_PX * range_db:
                continue
            level = step * int(self._ymin / step)
            while level < self._ymax:
                if level > self._ymin:
                    fb.hline(x, y + h - 1 - int((level - self._ymin) * (h - 1) / range_db), w, colour)
                level += step

    def _draw(self):
        if self._fpga:
            _fpga.feed()
            return
        if self._dirty:
            self._apply()
        self._spectrum()
        fb, buf = self._fbo, self._buf
        x, y, w, h = self._plot()
        fb.fill_rect(x, y, w, h, _gui.BG)
        if self._grid:
            self._paint_grid(x, y, w, h)
        esdr.fb_trace(buf, self._w, x, y, w, h, self._cols, self._ymin, self._ymax, _gui.TRACE[0])


class waterfall_sink_c(_spectrum):
    """The rows hold only power, in a ring (_img, newest at _rhead), each with the view it was
    drawn in (_col0) and its time line (_marks). Every redraw puts them on the canvas with the grid
    over them, which on the WHY badge beats scrolling the canvas in place (160 frames against 156
    in 15 s). A retune changes the view and no row: the picture moves with the frequency, 0 power
    where no row knows it, and a tune back finds the rows again. A new span clears them: old rows
    are at a scale the new axis cannot show. The FPGA draws none of this (no grid, no move)."""

    _draws = True
    _accel = "waterfall"

    def __init__(self, size, wintype, fc, bw, name, nconnections=1, parent=None):
        super().__init__("waterfall_sink_c", "c", size, wintype, fc, bw, name)
        self._imin = -140.0
        self._imax = 10.0
        self._palette = _gui.PALETTE
        self._img = None
        self._fbi = None
        self._shape = None
        self._rows = 0
        self._rhead = 0
        self._col0 = None
        self._marks = None
        self._last = 0
        self._drawn = (self._hz, self._span)
        self._anchor = self._hz
        self._view = 0
        self._spacing = (None, None, None, None)
        self._time = _wfgrid.TimeMarks()
        self._settle = _wfgrid.Settle()

    def set_grid_spacing(self, freq_fine=None, freq_coarse=None, time_fine=None, time_coarse=None):
        """Not in GNU Radio, GRC never calls it: the grid's fine and coarse lines, in Hz and in
        seconds. None follows the span and the plot's height in 1, 2, 5 steps, 0 turns a level off.
        Frequency lines closer than 6 columns are not drawn. Rows already drawn keep the time lines
        only while the time spacings stay the same."""
        self._spacing = (_wfgrid.spacing(freq_fine), _wfgrid.spacing(freq_coarse),
                         _wfgrid.spacing(time_fine, 1000), _wfgrid.spacing(time_coarse, 1000))
        self._dirty = True

    def set_intensity_range(self, imin, imax):
        # An upper level at or below the lower one (two sliders crossed) leaves 1 dB.
        self._imin = float(imin)
        self._imax = max(float(imax), self._imin + 1.0)
        if self._fpga:
            _fpga.configure()

    def set_gamma(self, gamma):
        """Not in GNU Radio: the heat scale's gamma for this sink (_gui.GAMMA by default)."""
        self._palette = dusk.heat(gamma=float(gamma))
        if self._fpga:
            _fpga.configure()

    def _make(self, parent, x, y, w, h):
        super()._make(parent, x, y, w, h)
        if self._fpga:
            self._img = self._fbi = self._col0 = self._marks = None
            self._shape = None

    def _setup(self):
        super()._setup()
        x, y, w, rows = self._plot()
        if rows <= 0:
            self._img = self._fbi = None
            self._shape = None
            return
        # The ring stays through a new layout of the same size (a tab change).
        if self._shape != (w, rows):
            self._shape = (w, rows)
            self._rows = rows
            self._rhead = 0
            self._img = None
            self._img = bytearray(w * rows * 2)
            self._fbi = framebuf.FrameBuffer(self._img, w, rows, framebuf.RGB565)
            self._forget()
        self._last = time.ticks_ms()
        self._apply()
        self._compose()

    def _forget(self):
        # All history unknown: the cold end of the scale, which is 0 power whatever the gamma.
        self._fbi.fill(self._palette[0])
        self._col0 = array.array("i", [0] * self._rows)
        self._marks = bytearray(self._rows)
        self._anchor = self._hz
        self._view = 0
        self._drawn = (self._hz, self._span)

    def _apply(self):
        self._dirty = False
        ff, fc = self._freq_grid()
        wanted = (self._hz, self._span)
        if self._span != self._drawn[1]:
            self._forget()
            self._settle.retuned()
        elif wanted != self._drawn:
            self._drawn = wanted
            self._settle.retuned()
        self._view = _wfgrid.view(self._hz, self._anchor, self._w, self._span)
        tf = tc = 0
        if self._grid:
            tf, tc = _wfgrid.time_levels(self._spacing[2], self._spacing[3], self._rows,
                                         self._update)
        if self._time.set(tf, tc):
            self._marks = bytearray(self._rows)
        self._notes = _pair(ff, fc, "Hz") + _pair(tf / 1000, tc / 1000, "s")
        self._head()

    def _compose(self):
        x, y, w, h = self._plot()
        esdr.fb_wf_compose(self._buf, self._w, x, y, w, h, self._img, self._rhead, self._col0,
                           self._marks, self._view, self._palette[0], self._lines, _gui.FG,
                           _gui.MUTED)

    def _draw(self):
        if self._fpga:
            _fpga.feed()
            return
        if self._img is None:
            return
        total = self._spectrum()
        if self._dirty:
            self._apply()
        now = time.ticks_ms()
        self._rhead = r = (self._rhead - 1) % self._rows
        self._col0[r] = self._view
        self._marks[r] = self._time.row(time.ticks_diff(now, self._last))
        self._last = now
        if self._settle.known(total, self._size):
            esdr.fb_wf_row(self._img, self._w, 0, r, self._w, 1, self._cols, self._imin,
                           self._imax, self._palette)
        else:
            self._fbi.fill_rect(0, r, self._w, 1, self._palette[0])
        self._compose()


class time_sink_c(_sink):
    _draws = True

    def __init__(self, size, samp_rate, name, nconnections=1, parent=None):
        super().__init__("time_sink_c", "c", name)
        self._size = max(16, min(esdr.MON_SAMPLES, int(size)))
        self._rate = float(samp_rate)
        self._ymin = -1.0
        self._ymax = 1.0
        self._src = bytearray(4 * self._size)
        self._i = array.array("f", [0.0] * _gui.W)
        self._q = array.array("f", [0.0] * _gui.W)

    def _columns(self, w):
        self._i = array.array("f", [0.0] * w)
        self._q = array.array("f", [0.0] * w)

    def set_y_axis(self, ymin, ymax):
        self._ymin = float(ymin)
        self._ymax = float(ymax)

    def set_samp_rate(self, r):
        self._rate = float(r)

    def set_nsamps(self, n):
        self._size = max(16, min(esdr.MON_SAMPLES, int(n)))
        self._src = bytearray(4 * self._size)

    def _label(self):
        return "{} {} S".format(self._title, self._size)

    def _draw(self):
        esdr.rx_latest(self._src)
        esdr.iq_cols(self._src, self._i, self._q)
        fb, buf = self._fbo, self._buf
        x, y, w, h = self._plot()
        fb.fill_rect(x, y, w, h, _gui.BG)
        fb.hline(x, y + h // 2, w, _gui.DIM)
        esdr.fb_trace(buf, self._w, x, y, w, h, self._i, self._ymin, self._ymax, _gui.TRACE[0])
        esdr.fb_trace(buf, self._w, x, y, w, h, self._q, self._ymin, self._ymax, _gui.TRACE[1])


class const_sink_c(_sink):
    """A constellation of the latest samples, square, centred in the band."""

    _draws = True

    def __init__(self, size, name, nconnections=1, parent=None):
        super().__init__("const_sink_c", "c", name)
        self._size = max(16, min(esdr.MON_SAMPLES, int(size)))
        self._src = bytearray(4 * self._size)
        self._x = (-2.0, 2.0)
        self._y = (-2.0, 2.0)

    def set_x_axis(self, xmin, xmax):
        self._x = (float(xmin), float(xmax))

    def set_y_axis(self, ymin, ymax):
        self._y = (float(ymin), float(ymax))

    def set_nsamps(self, n):
        self._size = max(16, min(esdr.MON_SAMPLES, int(n)))
        self._src = bytearray(4 * self._size)

    def _draw(self):
        esdr.rx_latest(self._src)
        fb, buf = self._fbo, self._buf
        x, y, w, h = self._plot()
        fb.fill_rect(x, y, w, h, _gui.BG)
        side = min(w, h)
        x0 = x + (w - side) // 2
        fb.hline(x0, y + side // 2, side, _gui.DIM)
        fb.vline(x0 + side // 2, y, side, _gui.DIM)
        fs = esdr.FULL_SCALE
        esdr.fb_points(buf, self._w, x0, y, side, side, self._src, self._x[0] * fs,
                       self._x[1] * fs, self._y[0] * fs, self._y[1] * fs, _gui.TRACE[0])


def _quiet(name):
    class sink(_sink):
        def __init__(self, *args, **kwargs):
            super().__init__(name, "c", name)

    return sink


number_sink = _quiet("number_sink")
histogram_sink_f = _quiet("histogram_sink_f")
vector_sink_f = _quiet("vector_sink_f")
eye_sink_c = _quiet("eye_sink_c")
sink_c = _quiet("sink_c")


class Range:
    def __init__(self, start, stop, step, default, min_length=200):
        self.min = start
        self.max = stop
        self.step = step
        self.default = default


class RangeWidget:
    """A QT GUI Range variable: a label and a slider; the encoder edits the slider. On a slider
    of more than ACCEL steps (a frequency in 1 MHz steps), n steps of the knob between two
    passes of the display loop move n * n steps. A value the slot refuses (a frequency the
    radio cannot tune) is not taken: the slider stops at the last value towards it that works."""

    ACCEL = 100
    SHIFT = 10

    _flex = False
    _h = _gui.ROW

    def __init__(self, ranges, slot, label, style="counter_slider", rangeType=float,
                 orientation=None, *args):
        self.r = ranges
        self.slot = slot
        self.label = label
        self.type = rangeType
        self.steps = max(1, int(round((ranges.max - ranges.min) / ranges.step)))
        self.index = max(0, min(self.steps, int(round((ranges.default - ranges.min) / ranges.step))))
        self.value = ranges.default
        _gui.variable(self)

    def set_index(self, i):
        """Step i: the value changes, the slot runs."""
        i = max(0, min(self.steps, int(i)))
        if i != self.index:
            good = self.index
            try:
                self._take(i)
            except Exception as e:
                print("{}: {} refused: {!r}".format(self.label, _gui.eng(self._at(i)), e))
                # The furthest step towards i the slot takes.
                bad = i
                ok = False
                while abs(bad - good) > 1:
                    mid = (good + bad) // 2
                    try:
                        self._take(mid)
                        good = mid
                        ok = True
                    except Exception:
                        bad = mid
                        ok = False
                if not ok:
                    self._take(good)
        if self._slider is not None:
            self._slider.set_value(self.index, False)

    def _turned(self, i):
        # The knob (or the arrow keys) moved the slider to step i since the last pass of the
        # display loop. With a keyboard's Shift held each step counts SHIFT times (10 MHz on a
        # frequency in 1 MHz steps); boards without a keyboard never report Shift.
        n = int(i) - self.index
        if self.steps > self.ACCEL and abs(n) > 1:
            n *= abs(n)
        if _gui.shift():
            n *= self.SHIFT
        self.set_index(self.index + n)

    def _at(self, i):
        return self.type(self.r.min + i * self.r.step)

    def _take(self, i):
        self.index = i
        self.value = self._at(i)
        self._show()
        self.slot(self.value)

    def _show(self):
        if self._text is not None:
            self._text.set_text("{}: {}".format(self.label, _gui.eng(self.value)))

    def _make(self, parent, x, y, w, h):
        lv = _gui.lv
        self._text = lv.label(parent)
        self._text.set_pos(x + 2, y + 5)
        sl = lv.slider(parent)
        sl.set_size(max(40, w // 2 - 10), 8)
        sl.set_pos(x + w - max(40, w // 2 - 10) - 10, y + 9)
        sl.set_range(0, self.steps)
        sl.set_value(self.index, False)
        for part in (lv.PART.INDICATOR, lv.PART.KNOB):
            sl.set_style_bg_color(lv.color_hex(dusk.ACCENT), part)
        _gui.on(sl, lv.EVENT.VALUE_CHANGED, self._turned, sl.get_value, latest=True)
        _gui.add_to_group(sl)
        self._slider = sl
        self._show()

    _text = None
    _slider = None

    def __getattr__(self, name):
        return Nothing()


def _msg_push_button_base():
    from PyQt5 import Qt

    return Qt.QPushButton


class MsgPushButton(_msg_push_button_base(), gr.basic_block):
    """QT GUI Msg Push Button: a button that publishes (msgName . value) on its "pressed" port
    when clicked; set_new_msg() changes the value (a Chooser variable behind it, say)."""

    def __init__(self, lbl, msgName, msgValue, relBackColor="default", relFontColor="default"):
        gr.basic_block.__init__(self, "MsgPushButton")
        _msg_push_button_base().__init__(self, lbl)
        self.lbl = lbl
        self.msgName = msgName
        self.msgValue = msgValue
        self.message_port_register_out("pressed")
        self.clicked.connect(self._clicked)

    def _clicked(self, *args):
        self._send(self.msgValue)

    def _send(self, v):
        import pmt

        if isinstance(v, bool):
            p = pmt.from_bool(v)
        elif isinstance(v, int):
            p = pmt.from_long(v)
        elif isinstance(v, float):
            p = pmt.from_double(v)
        else:
            p = pmt.intern(str(v))
        self.message_port_pub("pressed", pmt.cons(pmt.intern(self.msgName), p))

    def set_new_msg(self, msg):
        self.msgValue = msg
