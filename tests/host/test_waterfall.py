# SPDX-License-Identifier: GPL-3.0-or-later
"""The waterfall widget (apps/micropython/modules/gnuradio/qtgui.py, _wfgrid.py, cmod/esdr_wf.c):
the view and the grid's numbers, the C picture against a reference, and the unmodified Waterfall
export on a fake radio, whose picture comes from that same C. Frequencies go through float32 where
the board's would: CPython's doubles would hide its 256 Hz steps at 2.4 GHz."""

import array
import ctypes
import errno
import importlib
import random
import struct
import sys
import time
import types

import pytest

from conftest import REPO, load

MODULES = REPO / "apps/micropython/modules"
EGG = REPO / "apps/micropython/eggs/waterfall"
CMOD = REPO / "apps/micropython/cmod"
EGGS = REPO / "apps/micropython/eggs"
OWN = ("gnuradio", "pmt", "PyQt5", "sip", "packaging", "esdr", "framebuf", "lvgl", "waterfall",
       "spectrum", "constellation", "receiver", "dusk", "ltecal")

grid = load("apps/micropython/modules/gnuradio/_wfgrid.py", "wfgrid_under_test")

COARSE, FINE = 0xDEFB, 0x8C52
FILL = 0x2088


def f32(x):
    return struct.unpack("f", struct.pack("f", x))[0]


# -- the view


def test_view_rounds_and_returns():
    w, bw = 720, 2_000_000
    assert grid.view(2_440_000_000, 2_440_000_000, w, bw) == 0
    assert grid.view(2_441_000_000, 2_440_000_000, w, bw) == 360
    assert grid.view(2_439_000_000, 2_440_000_000, w, bw) == -360
    assert grid.view(2_440_000_000 + 2778, 2_440_000_000, w, bw) == 1
    assert grid.view(2_440_000_000 + 1388, 2_440_000_000, w, bw) == 0


def test_small_steps_do_not_add_up():
    # 100 Hz at 347 Hz a column: each step alone rounds to nothing.
    w, bw = 720, 250_000
    anchor = 2_440_000_000
    for k in range(1, 201):
        assert grid.view(anchor + 100 * k, anchor, w, bw) == (2 * 100 * k * w + bw) // (2 * bw)


def test_a_walk_comes_back_on_float32_frequencies():
    rng = random.Random(1)
    w, bw = 720, 2_000_000
    start = f32(2.44e9)
    anchor = int(start)
    f = start
    for _ in range(500):
        f = f32(f + rng.choice((-1e6, 1e6, -3e5, 3e5, -256.0, 256.0)))
        v = grid.view(int(f), anchor, w, bw)
        assert abs(v - (int(f) - anchor) * w / bw) <= 0.5
    assert grid.view(int(start), anchor, w, bw) == 0


# -- the spacings


@pytest.mark.parametrize("bw, fine, coarse", [
    (250_000, 10_000, 50_000),
    (500_000, 20_000, 100_000),
    (1_000_000, 50_000, 200_000),
    (2_000_000, 100_000, 500_000),
    (40_000_000, 1_000_000, 5_000_000),
    (80_000_000, 2_000_000, 10_000_000),
])
def test_automatic_frequency_spacing(bw, fine, coarse):
    assert grid.freq_levels(None, None, bw, 720) == (fine, coarse)
    # Fine lines on coarse ones, whatever the mantissa.
    assert coarse % fine == 0


def test_automatic_time_spacing():
    assert grid.time_levels(None, None, 589, 0.05) == (1000, 5000)
    # A short plot keeps its lines apart.
    assert grid.time_levels(None, None, 11, 0.05) == (200, 1000)


def test_given_spacing_and_off():
    assert grid.freq_levels(100_000, 1_000_000, 2_000_000, 720) == (100_000, 1_000_000)
    assert grid.freq_levels(0, 1_000_000, 2_000_000, 720) == (0, 1_000_000)
    assert grid.time_levels(500, 0, 589, 0.05) == (500, 0)
    assert grid.spacing(None) is None
    assert grid.spacing(0.5, 1000) == 500
    assert grid.spacing(-3) == 0


def test_too_dense_a_level_is_off():
    # 1 Hz at 80 MHz would be 80 million passes of a loop and a solid plot.
    fine, coarse = grid.freq_levels(1, 1, 80_000_000, 720)
    assert (fine, coarse) == (0, 0)
    assert len(grid.lines(2_440_000_000, 80_000_000, 720, 256, 1, 1)) == 0
    # 6 columns apart is drawn, under 6 is not.
    assert grid.freq_levels(100, 1000, 12_000, 720) == (100, 1000)
    assert grid.freq_levels(99, 1000, 12_000, 720) == (0, 1000)


# -- the lines


def columns(table):
    return [x & ~grid.COARSE for x in table]


@pytest.mark.parametrize("w", [720, 428, 320])
@pytest.mark.parametrize("bw", [250_000, 500_000, 1_000_000, 2_000_000, 40_000_000, 80_000_000])
def test_line_tables(w, bw):
    hz = int(f32(2.44e9))
    fine, coarse = grid.freq_levels(None, None, bw, w)
    table = grid.lines(hz, bw, w, 256, fine, coarse)
    xs = columns(table)
    assert xs == sorted(set(xs)) and all(0 <= x < w for x in xs)
    assert any(x & grid.COARSE for x in table)
    # About as many lines as the spacing says, however the frequency falls.
    if fine:
        assert abs(len(xs) - bw // fine) <= 2
    gaps = [b - a for a, b in zip(xs, xs[1:])]
    assert not gaps or min(gaps) >= grid.MIN_PX - 1


def test_lines_land_on_the_bins_psd_draws():
    # esdr.psd gives column c the bins from c * n / w: the line of a bin's frequency must fall in
    # the columns of that bin (the plain mapping is half a bin off, 1.41 columns here).
    n, w, bw, s = 256, 720, 256_000, 64_000
    hz = 2_440_000_000 - 2_440_000_000 % s
    for x in columns(grid.lines(hz, bw, w, n, 0, s)):
        b = x * n // w
        assert (b - n // 2) * bw // n % s == 0, x


def test_lines_move_with_the_frequency():
    n, w, bw = 256, 720, 2_000_000
    s = 500_000
    hz = 2_440_000_000
    moved = 100_000  # 36 columns
    a = columns(grid.lines(hz, bw, w, n, 0, s))
    b = columns(grid.lines(hz + moved, bw, w, n, 0, s))
    assert [x - 36 for x in a if x >= 36] == [x for x in b if x < a[-1] - 36 + 1]


def test_coarse_wins_where_lines_meet():
    table = grid.lines(2_440_000_000, 2_000_000, 720, 256, 100_000, 500_000)
    assert sum(1 for x in table if x & grid.COARSE) == 4
    assert len(table) == 20


# -- time lines


def test_time_marks_on_multiples():
    t = grid.TimeMarks()
    assert t.set(1000, 5000)
    assert not t.set(1000, 5000)
    marks = [t.row(50) for _ in range(400)]
    at = [k for k, m in enumerate(marks) if m]
    assert at[:3] == [19, 39, 59]
    assert [k for k, m in enumerate(marks) if m == grid.MARK_COARSE][:2] == [99, 199]


def test_time_marks_under_jitter_keep_time():
    rng = random.Random(2)
    t = grid.TimeMarks()
    t.set(1000, 5000)
    now = 0
    seen = []
    for _ in range(2000):
        dt = rng.randint(40, 120)
        now += dt
        if t.row(dt):
            seen.append(now)
    # Each line at most one row late of its multiple.
    assert seen and all(s % 1000 <= 120 for s in seen)
    assert abs(len(seen) - now // 1000) <= 1


def test_time_marks_no_solid_band():
    # Rows as slow as the spacing: a line per row would fill the plot.
    t = grid.TimeMarks()
    t.set(100, 500)
    marks = [t.row(100) for _ in range(100)]
    for k, m in enumerate(marks):
        if m == grid.MARK_FINE:
            assert not any(marks[max(0, k - grid.GAP_FINE + 1):k])
    assert not any(a and b for a, b in zip(marks, marks[1:]))
    assert sum(1 for m in marks if m) <= len(marks) // 2


def test_time_marks_keep_fine_lines_at_a_slow_row_rate():
    # 40 MS/s on the WHY badge: 3.6 rows a second, and a line every second all the same.
    t = grid.TimeMarks()
    t.set(1000, 5000)
    marks = [t.row(278) for _ in range(360)]
    assert abs(sum(1 for m in marks if m) - 360 * 278 // 1000) <= 1


def test_time_marks_a_long_gap_is_one_line():
    t = grid.TimeMarks()
    t.set(1000, 5000)
    for _ in range(4):
        t.row(50)
    assert t.row(60000) == grid.MARK_COARSE
    assert t.row(50) == 0


def test_time_marks_off():
    t = grid.TimeMarks()
    t.set(0, 0)
    assert not any(t.row(5000) for _ in range(20))


# -- rows around a retune


def test_settle_first_row_unknown_then_new_samples():
    s = grid.Settle()
    assert s.known(100, 256)
    s.retuned()
    assert not s.known(1000, 256)
    assert not s.known(1100, 256)
    assert s.known(1256, 256)
    assert s.known(1256, 256)


def test_settle_gives_up_when_no_samples_come():
    s = grid.Settle()
    s.retuned()
    known = [s.known(1000, 256) for _ in range(grid.SETTLE_ROWS + 1)]
    assert known == [False] * grid.SETTLE_ROWS + [True]


def test_settle_the_channelizer_gets_one_row():
    s = grid.Settle()
    s.retuned()
    assert not s.known(None, 256)
    assert s.known(None, 256)


def test_settle_a_total_that_wrapped():
    s = grid.Settle()
    s.retuned()
    assert not s.known(0xFFFFFFF0, 16)
    assert s.known(0x100000000, 16)


# -- the picture in C


class Src(ctypes.Structure):
    """struct esdr_wf_src of apps/micropython/cmod/esdr_wf.h."""
    _fields_ = [("img", ctypes.c_void_p), ("col0", ctypes.c_void_p), ("marks", ctypes.c_void_p),
                ("lines", ctypes.c_void_p), ("w", ctypes.c_int), ("rows", ctypes.c_int),
                ("head", ctypes.c_int), ("nlines", ctypes.c_int), ("view", ctypes.c_int32),
                ("fill", ctypes.c_uint16), ("coarse", ctypes.c_uint16), ("fine", ctypes.c_uint16)]


def addr(buf):
    return buf.buffer_info()[0] if len(buf) else None


def compose_c(lib, img, col0, marks, lines, w, rows, head, view, stride=None):
    stride = stride or w
    dst = array.array("H", [0xAAAA] * (stride * rows))
    keep = array.array("B", marks)
    src = Src(addr(img), addr(col0), addr(keep), addr(lines), w, rows, head, len(lines), view,
              FILL, COARSE, FINE)
    lib.esdr_wf_compose(addr(dst), stride, ctypes.byref(src))
    return dst


def lean(px, colour):
    return ((px & 0xF7DE) >> 1) + ((colour & 0xF7DE) >> 1)


def compose_py(img, col0, marks, lines, w, rows, head, view):
    out = []
    for row in range(rows):
        r = (head + row) % rows
        shift = col0[r] - view
        line = [img[r * w + c - shift] if 0 <= c - shift < w else FILL for c in range(w)]
        if marks[r] == grid.MARK_COARSE:
            line = [lean(p, COARSE) for p in line]
        elif marks[r] == grid.MARK_FINE:
            line = [lean(p, FINE) if c % 2 == 0 else p for c, p in enumerate(line)]
        for entry in lines:
            c = entry & ~grid.COARSE
            if c >= w:
                continue
            if entry & grid.COARSE:
                line[c] = lean(line[c], COARSE)
            elif row % 2 == 0:
                line[c] = lean(line[c], FINE)
        out.extend(line)
    return out


@pytest.fixture
def clib(wf_clib):
    if wf_clib is None:
        pytest.skip("no host C compiler")
    return wf_clib


@pytest.mark.parametrize("seed", range(40))
def test_compose_against_the_reference(clib, seed):
    rng = random.Random(seed)
    w, rows = rng.randint(1, 40), rng.randint(1, 24)
    img = array.array("H", [rng.randrange(0x10000) for _ in range(w * rows)])
    col0 = array.array("i", [rng.choice((0, 1, -1, w - 1, 1 - w, w, -w, w + 5, -w - 5,
                                         rng.randint(-2 * w, 2 * w))) for _ in range(rows)])
    marks = [rng.choice((0, 0, 0, 1, 2)) for _ in range(rows)]
    lines = array.array("H", [rng.randrange(w + 4) | rng.choice((0, grid.COARSE))
                              for _ in range(rng.randint(0, 6))])
    head = rng.randrange(rows)
    view = rng.randint(-w, w)
    got = compose_c(clib, img, col0, marks, lines, w, rows, head, view)
    assert list(got) == compose_py(img, col0, marks, lines, w, rows, head, view)


def test_compose_far_away_is_all_fill(clib):
    w, rows = 16, 4
    img = array.array("H", [0x1234] * (w * rows))
    col0 = array.array("i", [2_000_000_000, -2_000_000_000, 0, 0])
    marks = [0] * rows
    got = compose_c(clib, img, col0, marks, array.array("H"), w, rows, 0, -2_000_000_000)
    # 4e9 columns away in 32 bits would wrap to a small shift.
    assert list(got[:w]) == [FILL] * w
    assert list(got[w:2 * w]) == [0x1234] * w


def test_compose_stays_in_its_columns(clib):
    w, rows, stride = 8, 3, 13
    img = array.array("H", [0x1111] * (w * rows))
    col0 = array.array("i", [3, -3, 0])
    lines = array.array("H", [0, 7, 8, 200])
    got = compose_c(clib, img, col0, [2, 1, 0], lines, w, rows, 1, 0, stride)
    for row in range(rows):
        assert list(got[row * stride + w:(row + 1) * stride]) == [0xAAAA] * (stride - w)


def test_a_line_never_hides_a_signal():
    assert lean(0xFFFF, COARSE) not in (0xFFFF, COARSE)
    assert lean(0x0000, COARSE) != 0
    assert lean(0x0000, FINE) != lean(0x0000, COARSE)


# -- registration


def test_every_table_and_build_has_the_helper():
    for name in ("esdr_radio.c", "modesdr.c"):
        text = (CMOD / name).read_text()
        assert "esdr_fb_wf_compose_obj" in text.split("MP_ROM_QSTR(MP_QSTR_fb_wf_compose)")[0]
        assert "MP_ROM_PTR(&esdr_fb_wf_compose_obj)" in text
    cmake = (CMOD / "micropython.cmake").read_text()
    assert cmake.count("/esdr_wf.c") == cmake.count("/esdr_gui.c") == 2


# -- the unmodified export on a fake radio


class FakeRadio(types.ModuleType):
    """esdr as a waterfall uses it. Every spectrum differs (a row number and the column), so a
    pixel says which row and which column it came from."""

    RATES = (2000000, 1000000, 500000, 250000)
    BURST_RATES = (80000000, 40000000, 16000000)
    MON_SAMPLES = 8192
    FULL_SCALE = 16384

    def __init__(self, lib):
        super().__init__("esdr")
        self.lib = lib
        self.seq = 0
        self.total = 0
        self.advance = 1000
        self.top = None
        self.tuned = []
        self.gains = []
        self.gain_max = 79
        self.started = []
        self.composes = 0

    def init(self):
        pass

    def channel_bw(self, mode):
        pass

    def rx_gain(self, g):
        self.gains.append(g)

    def rx_gain_max(self):
        return self.gain_max

    def rx_bandwidth(self, bw):
        pass

    def rx_start(self, rate, *a, **kw):
        self.started.append(rate)

    def rx_stop(self):
        pass

    def tune(self, hz, offset=True):
        if self.top is not None and hz > self.top:
            raise ValueError("no lock")
        self.tuned.append(hz)
        return hz

    def rx_latest(self, buf):
        self.total += self.advance
        return self.total

    def psd(self, src, cols, state, avg, win):
        self.seq = (self.seq + 1) & 0x7F
        for c in range(len(cols)):
            cols[c] = 0x8000 | self.seq << 8 | c & 0xFF

    def fb_wf_row(self, fb, stride, x, y, w, h, values, vmin, vmax, palette):
        px = memoryview(fb).cast("H")
        for c in range(w):
            px[y * stride + x + c] = int(values[c])

    def fb_wf_compose(self, fb, stride, x, y, w, h, img, head, col0, marks, view, fill, lines,
                      coarse, fine):
        self.composes += 1
        dst = (ctypes.c_uint16 * (len(fb) // 2)).from_buffer(fb)
        src = Src(ctypes.addressof((ctypes.c_uint8 * len(img)).from_buffer(img)),
                  addr(col0), ctypes.addressof((ctypes.c_uint8 * len(marks)).from_buffer(marks)),
                  addr(lines), w, h, head, len(lines), view, fill, coarse, fine)
        self.lib.esdr_wf_compose(ctypes.addressof(dst) + (y * stride + x) * 2, stride,
                                 ctypes.byref(src))


class GaplessRadio(FakeRadio):
    """The WHY2025 badge's C6: the gapless stream, or a build that refuses it."""

    GAPLESS_RATE = 80000000
    CHAN_SPEC_MAX = 640

    def __init__(self, lib):
        super().__init__(lib)
        self.runs = []
        self.refuse = False

    def gapless_start(self, **kw):
        if self.refuse:
            raise OSError(errno.EOPNOTSUPP, "no gapless mode")
        self.runs.append(kw)

    def gapless_stop(self):
        pass

    def chan_spectrum(self, out):
        return 8 * self.runs[-1]["channels"]

    def stretch(self, src, dst):
        self.seq = (self.seq + 1) & 0x7F
        for c in range(len(dst)):
            dst[c] = 0x8000 | self.seq << 8 | c & 0xFF


class FakeFrameBuffer:
    def __init__(self, buf, w, h, fmt):
        self.px = memoryview(buf).cast("H")
        self.w = w
        self.texts = []

    def fill(self, colour):
        for k in range(len(self.px)):
            self.px[k] = colour

    def fill_rect(self, x, y, w, h, colour):
        for row in range(y, y + h):
            for c in range(x, x + w):
                self.px[row * self.w + c] = colour

    def text(self, s, x, y, colour):
        self.texts.append(s)

    def hline(self, x, y, w, colour):
        self.fill_rect(x, y, w, 1, colour)


class FakeCanvas:
    def __init__(self, parent):
        pass

    def set_buffer(self, buf, w, h, fmt):
        pass

    def set_pos(self, x, y):
        pass

    def invalidate(self):
        pass


@pytest.fixture
def make_board(monkeypatch, clib):
    """The unmodified export of an egg on a fake radio."""
    saved = {k: v for k, v in sys.modules.items() if k.split(".")[0] in OWN}
    for k in saved:
        del sys.modules[k]
    clock = [1000]
    monkeypatch.setattr(time, "ticks_ms", lambda: clock[0], raising=False)
    monkeypatch.setattr(time, "ticks_add", lambda a, b: a + b, raising=False)
    monkeypatch.setattr(time, "ticks_diff", lambda a, b: a - b, raising=False)
    monkeypatch.syspath_prepend(str(MODULES))
    framebuf = types.ModuleType("framebuf")
    framebuf.RGB565 = 1
    framebuf.FrameBuffer = FakeFrameBuffer
    lvgl = types.ModuleType("lvgl")
    lvgl.canvas = FakeCanvas
    lvgl.COLOR_FORMAT = types.SimpleNamespace(RGB565=1)
    monkeypatch.setitem(sys.modules, "framebuf", framebuf)
    monkeypatch.setitem(sys.modules, "lvgl", lvgl)

    def make(egg="waterfall", radio_type=FakeRadio):
        radio = radio_type(clib)
        monkeypatch.setitem(sys.modules, "esdr", radio)
        monkeypatch.syspath_prepend(str(EGGS / egg))
        gui = importlib.import_module("gnuradio._gui")
        gui._headless = True
        tb = getattr(importlib.import_module(egg), egg)()
        tb.soapy_plutosdr_source_0._live = True
        return types.SimpleNamespace(tb=tb, sink=tb.__dict__.get("qtgui_waterfall_sink_x_0"),
                                     radio=radio, clock=clock, gui=gui,
                                     qtgui=sys.modules["gnuradio.qtgui"])

    yield make
    for k in [k for k in sys.modules if k.split(".")[0] in OWN]:
        del sys.modules[k]
    sys.modules.update(saved)


@pytest.fixture
def board(make_board):
    return make_board()


W, ROWS = 40, 30


def place(b, w=W, rows=ROWS):
    b.sink._make(None, 0, 0, w, rows + b.gui.LABEL)
    return b.sink


def picture(sink):
    px = memoryview(sink._buf).cast("H")
    x, y, w, h = sink._plot()
    return [list(px[(y + r) * sink._w + x:(y + r) * sink._w + x + w]) for r in range(h)]


def draw(b, n=1, ms=50):
    for _ in range(n):
        b.clock[0] += ms
        b.sink._draw()


def settled(b, rate=2e6, grid_on=False):
    """Placed, at a rate where 1 MHz is half the width (W = 40), with rows from a known span."""
    b.tb.set_samp_rate(rate)
    sink = place(b)
    sink.enable_grid(grid_on)
    draw(b, 6)
    return sink


def test_chooser_is_sorted_with_40_ms(board):
    rates = board.tb._samp_rate_options
    assert rates == sorted(rates)
    assert rates == [250e3, 500e3, 1e6, 2e6, 40e6, 80e6]
    labels = board.tb._samp_rate_labels
    assert labels == ["250 kS/s", "500 kS/s", "1 MS/s", "2 MS/s", "40 MS/s", "80 MS/s"]
    assert board.tb.samp_rate in rates


def test_spectrums_chooser_is_sorted_with_40_ms():
    export = (REPO / "apps/micropython/eggs/spectrum/spectrum.py").read_text()
    assert "_samp_rate_options = [250000.0, 500000.0, 1000000.0, 2000000.0, 40000000.0]" in export
    assert "_samp_rate_labels = ['250 kS/s', '500 kS/s', '1 MS/s', '2 MS/s', '40 MS/s']" in export


def test_starts_on_agc_with_a_gain_waiting(board):
    src = board.tb.soapy_plutosdr_source_0
    assert board.tb.agc is True and src._auto is True
    assert board.tb.gain == 48 and src._gain == 48
    src._changed()
    assert board.radio.gains[-1] == -1


def test_the_gain_slider_waits_under_agc(board):
    src = board.tb.soapy_plutosdr_source_0
    win = board.tb._gain_win
    board.radio.gains.clear()
    win.set_index(win.index + 4)
    # No restart of the radio for a value not in use, and AGC stays on.
    assert board.radio.gains == [] and src._auto is True
    assert src._gain == 52


def test_manual_takes_the_slider_and_agc_comes_back(board):
    src = board.tb.soapy_plutosdr_source_0
    win = board.tb._gain_win
    board.radio.gains.clear()
    board.tb.set_agc(False)
    assert src._auto is False and board.radio.gains[-1] == 48
    win.set_index(win.index - 8)
    assert board.radio.gains[-1] == 40
    board.tb.set_agc(True)
    assert src._auto is True and board.radio.gains[-1] == -1


def test_gain_is_clamped_to_what_the_radio_has(board):
    board.radio.gain_max = 60
    board.tb.set_agc(False)
    board.tb._gain_win.set_index(70)
    assert board.radio.gains[-1] == 60


def test_the_same_mode_again_does_not_restart_the_radio(board):
    board.radio.gains.clear()
    board.tb.set_agc(True)
    assert board.radio.gains == []


@pytest.mark.parametrize("egg", ["waterfall", "spectrum", "constellation"])
def test_gain_and_agc_share_a_row(make_board, egg):
    board = make_board(egg)
    from PyQt5 import Qt

    out = []
    board.gui.root._place(0, 18, 720, 702, out)
    at = {id(leaf): (x, y, w, h) for leaf, x, y, w, h in out}
    gain = at[id(board.tb._gain_win)]
    agc = next((x, y, w, h) for leaf, x, y, w, h in out if isinstance(leaf, Qt.QCheckBox))
    assert gain[1] == agc[1] and gain[0] + gain[2] <= agc[0]


@pytest.mark.parametrize("egg", ["waterfall", "spectrum"])
def test_the_chooser_reaches_the_radio(make_board, egg):
    # The sink's axis alone would look right with the radio left at its old rate.
    board = make_board(egg)
    src = board.tb.soapy_plutosdr_source_0
    for rate in board.tb._samp_rate_options:
        board.radio.started.clear()
        board.tb.set_samp_rate(rate)
        assert src._rate == int(rate)
        assert board.radio.started and board.radio.started[-1] == int(rate)


@pytest.mark.parametrize("egg", ["waterfall", "spectrum", "constellation", "receiver"])
def test_every_receive_app_starts_on_agc_and_takes_a_manual_gain(make_board, egg):
    # They all sat at gain 16 of 79: the hackrf block's VGA, the last gain set.
    board = make_board(egg)
    tb, src = board.tb, board.tb.soapy_plutosdr_source_0
    assert tb.agc is True and src._auto is True and src._gain == 48
    src._changed()
    assert board.radio.gains[-1] == -1
    tb.set_agc(False)
    assert board.radio.gains[-1] == 48
    tb.set_gain(32)
    assert board.radio.gains[-1] == 32
    tb.set_agc(True)
    assert board.radio.gains[-1] == -1


def test_receiver_keeps_its_radio_buttons_and_check_box(make_board):
    board = make_board("receiver")
    from PyQt5 import Qt

    assert board.tb._gain_options == [16, 32, 48]
    assert isinstance(board.tb._gain_group_box, Qt.QGroupBox)
    assert "RF amp" not in (EGGS / "receiver/receiver.py").read_text()


def test_40_and_80_ms_run_the_gapless_stream(make_board):
    board = make_board(radio_type=GaplessRadio)
    tb, radio = board.tb, board.radio
    tb.set_samp_rate(40e6)
    assert radio.runs[-1]["half"] is True and radio.runs[-1]["channels"] == 40
    assert radio.runs[-1]["gain_servo"] is True
    tb.set_samp_rate(80e6)
    assert radio.runs[-1]["half"] is False and radio.runs[-1]["channels"] == 80
    tb.set_agc(False)
    assert radio.runs[-1]["gain_servo"] is False and radio.gains[-1] == 48
    runs = len(radio.runs)
    tb.set_samp_rate(2e6)
    assert radio.started[-1] == 2000000 and len(radio.runs) == runs


def test_a_radio_without_the_stream_takes_bursts(make_board):
    board = make_board(radio_type=GaplessRadio)
    board.radio.refuse = True
    board.tb.set_samp_rate(40e6)
    assert board.radio.runs == [] and board.radio.started[-1] == 40000000


def test_a_board_without_the_stream_takes_bursts_at_40_ms(board):
    board.tb.set_samp_rate(40e6)
    assert board.radio.started[-1] == 40000000


def test_rows_at_40_ms_come_from_the_channelizer(make_board):
    board = make_board(radio_type=GaplessRadio)
    board.tb.set_samp_rate(40e6)
    sink = place(board)
    sink.enable_grid(False)
    draw(board, 4)
    assert picture(sink)[0] != [sink._palette[0]] * W
    assert board.radio.total == 0


def test_the_knob_reaches_the_radio(board):
    board.radio.tuned.clear()
    board.tb.set_freq(2.45e9)
    assert board.radio.tuned and board.radio.tuned[-1] == 2.45e9


def test_export_turns_the_grid_on(board):
    assert board.sink._grid is True
    assert "enable_grid(True)" in (EGG / "waterfall.py").read_text()


def test_rows_scroll_down_newest_on_top(board):
    sink = settled(board)
    before = picture(sink)
    draw(board)
    after = picture(sink)
    assert after[1:] == before[:-1]
    assert after[0] != before[0]


def test_retune_moves_the_picture_with_0_power_on_the_new_side(board):
    sink = settled(board)
    fill = sink._palette[0]
    before = picture(sink)
    board.tb.set_freq(board.tb.freq + 1e6)
    draw(board)
    after = picture(sink)
    # The top row's samples may be of either tune: unknown. Below it the old rows, half a width
    # to the left, as their frequencies now lie.
    assert after[0] == [fill] * W
    for r in range(ROWS - 1):
        assert after[r + 1] == before[r][20:] + [fill] * 20


def test_retune_down_moves_it_the_other_way(board):
    sink = settled(board)
    fill = sink._palette[0]
    before = picture(sink)
    board.tb.set_freq(board.tb.freq - 1e6)
    draw(board)
    after = picture(sink)
    for r in range(ROWS - 1):
        assert after[r + 1] == [fill] * 20 + before[r][:20]


def test_a_tune_back_finds_the_rows_again(board):
    sink = settled(board)
    before = picture(sink)
    f = board.tb.freq
    board.tb.set_freq(f + 10e6)  # far off the screen: all 0 power
    draw(board)
    board.tb.set_freq(f)
    draw(board)
    after = picture(sink)
    for r in range(ROWS - 2):
        assert after[r + 2] == before[r]


def test_a_refused_tune_leaves_no_trace(board):
    sink = settled(board)
    board.radio.top = board.tb.freq + 5e6
    before = picture(sink)
    win = board.tb._freq_win
    win.set_index(win.index + 10)
    # The slider stopped at the last value the radio took; the sink follows that one alone.
    assert win.value == board.radio.top
    assert sink._hz == int(win.value)
    draw(board)
    after = picture(sink)
    # 5 MHz at 50 kHz a column: everything left the screen, none of it through a refused value.
    assert sink._view == 100
    assert all(p == sink._palette[0] for p in after[1])
    assert before  # the rows are there to come back to
    board.tb.set_freq(win.value - 5e6)
    draw(board)
    assert picture(sink)[2] == before[0]


def test_setting_the_same_frequency_is_no_retune(board):
    sink = settled(board)
    board.tb.set_freq(board.tb.freq)
    before = picture(sink)
    draw(board)
    assert picture(sink)[1:] == before[:-1]
    assert picture(sink)[0] != [sink._palette[0]] * W


def test_many_small_steps_do_not_drift(board):
    board.tb.set_samp_rate(250e3)
    sink = place(board)
    sink.enable_grid(False)
    f = board.tb.freq
    for k in range(1, 101):
        board.tb.set_freq(f + 100 * k)
        draw(board)
    # 10 kHz at 6250 Hz a column, not 100 steps that each round to nothing.
    assert sink._view == 2


def test_rows_after_a_retune_wait_for_new_samples(board):
    sink = settled(board)
    fill = sink._palette[0]
    board.radio.advance = 0
    board.tb.set_freq(board.tb.freq + 1e6)
    draw(board, 3)
    assert all(picture(sink)[r] == [fill] * W for r in range(3))
    board.radio.advance = 1000
    draw(board)
    assert picture(sink)[0] != [fill] * W


def test_a_new_span_clears_the_history(board):
    sink = settled(board)
    fill = sink._palette[0]
    board.tb.set_samp_rate(1e6)
    draw(board)
    assert all(row == [fill] * W for row in picture(sink))
    assert sink._view == 0
    # And rows after it are drawn again.
    draw(board)
    assert picture(sink)[0] != [fill] * W


@pytest.mark.parametrize("rate", [250e3, 500e3, 1e6, 2e6, 40e6, 80e6])
def test_every_rate_of_the_chooser_draws(board, rate):
    board.tb.set_samp_rate(rate)
    sink = place(board, w=320)
    draw(board, 3)
    assert picture(sink)[0] != [sink._palette[0]] * 320
    assert sink._span == int(rate)
    assert len(sink._lines) > 0


def test_grid_off_draws_no_line(board):
    sink = settled(board)
    board.clock[0] += 60000
    draw(board, 8)
    assert len(sink._lines) == 0 and not any(sink._marks)
    assert sink._notes == ""
    # Every pixel is a row's own: the top bit of the fake spectrum, or 0 power.
    assert all(p & 0x8000 or p == sink._palette[0] for row in picture(sink) for p in row)


def test_grid_on_draws_lines_over_the_rows_without_entering_them(board):
    board.tb.set_samp_rate(2e6)
    sink = place(board, w=320)
    sink.enable_grid(True)
    draw(board, 40)
    assert len(sink._lines) > 0
    assert any(sink._marks)
    px = memoryview(sink._img).cast("H")
    assert all(p & 0x8000 or p == sink._palette[0] for p in px)
    composed = picture(sink)
    coarse = [x & ~board.qtgui._wfgrid.COARSE for x in sink._lines
              if x & board.qtgui._wfgrid.COARSE]
    r = sink._rhead
    assert composed[0][coarse[0]] != px[r * 320 + coarse[0]]
    sink.enable_grid(False)
    draw(board)
    r = sink._rhead
    assert picture(sink)[0] == list(px[r * 320:(r + 1) * 320])


def test_title_names_the_spacings(board):
    board.tb.set_samp_rate(2e6)
    sink = place(board, w=320)
    sink.enable_grid(True)
    draw(board)
    assert sink._notes == " 100k/500kHz 200m/1s"
    assert sink._label().startswith("Waterfall ")
    assert sink._label().endswith(" 100k/500kHz 200m/1s")
    sink.set_grid_spacing(0, 1e6, 0, 0)
    draw(board)
    assert sink._notes == " 1MHz"


def test_set_grid_spacing_sets_the_lines(board):
    board.tb.set_samp_rate(2e6)
    sink = place(board, w=320)
    sink.enable_grid(True)
    sink.set_grid_spacing(200e3, 1e6, 0.5, 2)
    draw(board)
    assert sink._time.fine == 500 and sink._time.coarse == 2000
    assert len(sink._lines) == 10
    sink.set_grid_spacing(1, 1, 0, 0)
    draw(board)
    assert len(sink._lines) == 0
    assert sink._time.fine == 0


def test_changing_the_time_spacing_drops_old_time_lines(board):
    sink = settled(board, grid_on=True)
    draw(board, 40)
    assert any(sink._marks)
    sink.set_grid_spacing(None, None, 0, 0)
    draw(board)
    assert not any(sink._marks)


def test_tab_change_keeps_the_ring(board):
    sink = settled(board)
    ring = sink._img
    before = picture(sink)
    place(board)
    assert sink._img is ring
    assert picture(sink) == before


def test_a_new_size_starts_over(board):
    sink = settled(board)
    place(board, w=W, rows=ROWS - 5)
    assert all(row == [sink._palette[0]] * W for row in picture(sink))
    draw(board)


def test_a_band_without_room_for_rows_draws_nothing(board):
    sink = place(board, rows=0)
    assert sink._img is None
    board.tb.set_freq(board.tb.freq + 1e6)
    draw(board)
    assert board.radio.composes == 0


def test_retune_off_screen_touches_no_canvas(board):
    # Before it is placed, and while another tab is showing, there is only a note of it.
    sink = board.sink
    board.tb.set_freq(board.tb.freq + 1e6)
    assert sink._dirty and sink._img is None


def test_the_fpga_path_is_untouched(board, monkeypatch):
    fpga = board.qtgui._fpga
    calls = []
    monkeypatch.setattr(fpga, "claim", lambda s: True)
    monkeypatch.setattr(fpga, "configure", lambda: calls.append("configure"))
    monkeypatch.setattr(fpga, "feed", lambda: calls.append("feed"))
    sink = place(board)
    assert sink._fpga and sink._img is None
    assert calls == ["configure"]
    sink._draw()
    assert calls[-1] == "feed"
    assert board.radio.composes == 0 and board.radio.seq == 0
    sink.set_frequency_range(2.5e9, 2e6)
    assert calls[-1] == "configure"
    assert sink._notes == ""
    sink.enable_grid(True)
    assert sink._img is None
