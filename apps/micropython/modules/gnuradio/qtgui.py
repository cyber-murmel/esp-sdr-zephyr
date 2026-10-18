# SPDX-License-Identifier: GPL-3.0-or-later
"""qtgui for GRC Qt GUI exports on the board. freq_sink, waterfall_sink and time_sink draw
on the board's display (_gui.py); the other sinks accept their samples and draw nothing.
Range and RangeWidget become variables the encoder changes. Line colours, labels, grids and
other styling calls are accepted and ignored."""

import array
import framebuf

import esdr
from gnuradio import _gui, gr
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
        self._buf = bytearray(w * h * 2)
        self._fbo = framebuf.FrameBuffer(self._buf, w, h, framebuf.RGB565)
        self._canvas = lv.canvas(parent)
        self._canvas.set_buffer(self._buf, w, h, lv.COLOR_FORMAT.RGB565)
        self._canvas.set_pos(x, y)
        self._columns(w)
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


class _spectrum(_sink):
    def __init__(self, name, kind, size, wintype, fc, bw, title):
        super().__init__(name, kind, title)
        # The power of two at or above size, 16 to 4096.
        n = 16
        while n < int(size) and n < 4096:
            n *= 2
        self._size = n
        self._win = int(wintype)
        self._fc = float(fc)
        self._bw = float(bw)
        self._avg = 1.0
        self._src = bytearray(4 * self._size)
        self._state = array.array("f", [0.0] * self._size)
        self._cols = array.array("f", [0.0] * _gui.W)

    def _columns(self, w):
        self._cols = array.array("f", [0.0] * w)

    def set_frequency_range(self, fc, bw):
        self._fc = float(fc)
        self._bw = float(bw)
        if self._h:
            self._setup()
            self._canvas.invalidate()

    def set_fft_size(self, n):
        pass

    def set_fft_average(self, a):
        self._avg = float(a)

    def _label(self):
        return "{} {}".format(self._title, _gui.eng(self._fc, "Hz"))

    def _spectrum(self):
        esdr.rx_latest(self._src)
        esdr.psd(self._src, self._cols, self._state, self._avg, self._win)


class freq_sink_c(_spectrum):
    _draws = True

    def __init__(self, size, wintype, fc, bw, name, nconnections=1, parent=None):
        super().__init__("freq_sink_c", "c", size, wintype, fc, bw, name)
        self._ymin = -140.0
        self._ymax = 10.0

    def set_y_axis(self, ymin, ymax):
        self._ymin = float(ymin)
        self._ymax = float(ymax)

    def _draw(self):
        self._spectrum()
        fb, buf = self._fbo, self._buf
        x, y, w, h = self._plot()
        fb.fill_rect(x, y, w, h, _gui.BG)
        # A grid line every 20 dB.
        step = 20.0
        level = step * int(self._ymin / step)
        while level < self._ymax:
            if level > self._ymin:
                fb.hline(x, y + h - 1 - int((level - self._ymin) * (h - 1) / (self._ymax - self._ymin)), w, _gui.DIM)
            level += step
        fb.vline(x + w // 2, y, h, _gui.DIM)
        esdr.fb_trace(buf, self._w, x, y, w, h, self._cols, self._ymin, self._ymax, _gui.TRACE[0])


class waterfall_sink_c(_spectrum):
    _draws = True

    def __init__(self, size, wintype, fc, bw, name, nconnections=1, parent=None):
        super().__init__("waterfall_sink_c", "c", size, wintype, fc, bw, name)
        self._imin = -140.0
        self._imax = 10.0

    def set_intensity_range(self, imin, imax):
        self._imin = float(imin)
        self._imax = float(imax)

    def set_frequency_range(self, fc, bw):
        # A retune keeps the history: only the label changes.
        self._fc = float(fc)
        self._bw = float(bw)
        if self._h:
            self._head()
            self._canvas.invalidate()

    def _setup(self):
        super()._setup()
        fb = self._fbo
        x, y, w, h = self._plot()
        fb.fill_rect(x, y, w, h, _gui.BG)

    def _draw(self):
        self._spectrum()
        buf = self._buf
        x, y, w, h = self._plot()
        esdr.fb_scroll(buf, self._w, x, y, w, h)
        esdr.fb_wf_row(buf, self._w, x, y, w, 1, self._cols, self._imin, self._imax, _gui.PALETTE)


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
        # The knob moved the slider to step i since the last pass of the display loop.
        n = int(i) - self.index
        if self.steps > self.ACCEL and abs(n) > 1:
            n *= abs(n)
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
