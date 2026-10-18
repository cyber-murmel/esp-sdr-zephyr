# SPDX-License-Identifier: GPL-3.0-or-later
"""The board's stand-in for the Qt GUI of GRC exports, on LVGL: a header with the window title,
a row (label and slider) per QT GUI Range variable, and the qtgui sinks sharing the rest of
the 320x240 display in bands, top to bottom in the order the flowgraph creates them. A sink
owns an RGB565 buffer shown by an lv.canvas; the esdr C helpers draw into it. QApplication
.exec_() runs the loop here: sinks redraw at their update time from the latest samples
(esdr.rx_latest()), the encoder moves between the sliders (turn) and edits one (press, turn,
press), and Ctrl-C runs the flowgraph's SIGINT handler as Qt would.

Colours are native RGB565: the display glue swaps the bytes when it flushes. LVGL runs only in
this thread; slider events are recorded inside LVGL and handed to the slots outside it.

Without a display (no LVGL in the firmware) or with a file /FLASH:/headless, the console takes
the display's place (gnuradio/_console.py)."""

import time

import esdr
from gnuradio import _layout as _layout_mod

W = 320
H = 240
HEADER = 18
ROW = 26
LABEL = 9


def rgb(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


BG = rgb(0, 0, 0)
FG = rgb(220, 220, 220)
DIM = rgb(60, 60, 60)
TRACE = (rgb(80, 160, 255), rgb(255, 80, 80), rgb(80, 220, 80), rgb(255, 255, 255))


def _palette():
    # Black, blue, cyan, yellow, red, white: GNU Radio's multi-colour waterfall map.
    stops = ((0, 0, 0), (0, 0, 200), (0, 200, 255), (255, 255, 0), (255, 0, 0), (255, 255, 255))
    import array

    pal = array.array("H", [0] * 64)
    for k in range(64):
        f = k * (len(stops) - 1) / 63
        i = min(int(f), len(stops) - 2)
        t = f - i
        a, b = stops[i], stops[i + 1]
        pal[k] = rgb(*(int(a[j] + (b[j] - a[j]) * t) for j in range(3)))
    return pal


PALETTE = _palette()

_sinks = []
_vars = []
_timers = []
_pollers = []
_title = ""
_quit = False
interrupted = False  # run() ended on Ctrl-C (the launcher tells the console's from the knob's)
root = None  # the flowgraph's top-level widget (PyQt5.Qt.QWidget): its layouts hold the controls
lv = None
_active = []
_scr = None
_group = None
_head = None
_changed = []
_relayout = False
# Redraws per sink name, and the time the loop spent drawing (ms), for stats().
_frames = {}
_busy = [0, 0]
_headless = None


def headless():
    """No display: the firmware has no LVGL, or /FLASH:/headless asks for the console."""
    global _headless
    if _headless is None:
        import os

        try:
            os.stat("/FLASH:/headless")
            _headless = True
        except OSError:
            try:
                import lvgl  # noqa: F401

                _headless = False
            except ImportError:
                _headless = True
    return _headless


class Signal:
    """A Qt signal: connect() handlers, emit() calls them."""

    def __init__(self):
        self._h = []

    def connect(self, fn):
        self._h.append(fn)

    def __getitem__(self, types):
        # buttonClicked[int].connect(...)
        return self

    def disconnect(self, fn=None):
        self._h = [] if fn is None else [h for h in self._h if h is not fn]

    def emit(self, *args):
        for h in self._h:
            h(*args)


def reset(top=None):
    global _title, _quit, root
    root = top
    _sinks.clear()
    _vars.clear()
    _timers.clear()
    _pollers.clear()
    _changed.clear()
    _frames.clear()
    _title = ""
    _quit = False
    _clear()


def _clear():
    # The widgets of an earlier flowgraph leave the display (LVGL deletes their children).
    global _head
    _head = None
    if _scr is not None:
        _group.remove_all_objs()
        _scr.clean()


def register(sink):
    _sinks.append(sink)


def variable(var):
    _vars.append(var)


def title(t):
    global _title
    _title = str(t)


def timer(t, on=True):
    if on and t not in _timers:
        t._next = time.ticks_add(time.ticks_ms(), t._ms)
        _timers.append(t)
    elif not on and t in _timers:
        _timers.remove(t)


def poll(fn, on=True):
    """Call fn at every pass of the loop (a packet flowgraph's radio, message dispatch); the same
    object again to stop."""
    if on and fn not in _pollers:
        _pollers.append(fn)
    elif not on and fn in _pollers:
        _pollers.remove(fn)


def quit():
    global _quit
    _quit = True


def eng(v, unit=""):
    from gnuradio.eng_notation import num_to_str

    return num_to_str(v, 6) + unit


def _up():
    """LVGL, the display, the screen and the encoder's group, once per session."""
    global lv, _scr, _group
    if lv is not None:
        return
    import lvgl

    if not lvgl.display_get_default():
        import ZDD

        ZDD.init()
    lv = lvgl
    _scr = lv.screen_active()
    _scr.remove_flag(lv.obj.FLAG.SCROLLABLE)
    _scr.set_style_bg_color(lv.color_hex(0), 0)
    _scr.set_style_text_color(lv.color_hex(0xDCDCDC), 0)
    _scr.set_style_pad_all(0, 0)
    _group = lv.group_create()
    _group.set_default()
    # The Zephyr encoder is the only input device; a throw-away one finds it in the list.
    tmp = lv.indev_create()
    dev = tmp.get_next()
    tmp.delete()
    while dev:
        if dev.get_type() == lv.INDEV_TYPE.ENCODER:
            dev.set_group(_group)
        dev = dev.get_next()


def on(obj, event, handler, getter=None, latest=False):
    """Call handler(getter()) when the LVGL object gets the event, after task_handler(): the
    callback inside LVGL only records. latest: of the events before then only the last counts
    (the steps of a slider while the knob turns)."""

    def cb(e):
        value = getter() if getter else None
        if latest:
            for k in range(len(_changed)):
                if _changed[k][0] is handler:
                    _changed[k] = (handler, value)
                    return
        _changed.append((handler, value))

    obj.add_event_cb(cb, event, None)
    return cb


def add_to_group(obj):
    _group.add_obj(obj)


def relayout():
    """A tab changed: build the screen again at the next pass of the loop."""
    global _relayout
    _relayout = True


def _layout():
    global _head, _active
    _clear()
    _head = lv.label(_scr)
    _head.set_pos(2, 1)
    out = []
    if root is not None:
        root._place(0, HEADER, W, H - HEADER, out)
    if not out:  # an export that put nothing in a layout: what was created, in order
        _layout_mod.stack(_vars + _sinks, 0, HEADER, W, H - HEADER, out)
    _active = []
    for leaf, x, y, w, h in out:
        leaf._make(_scr, x, y, w, h)
        if getattr(type(leaf), "_draws", False):
            _active.append(leaf)
    _header()


def _header(stopped=False):
    text = _title + ("  (stopped)" if stopped else "")
    _head.set_text(text[:40])


def show():
    if headless():
        return
    _up()
    _layout()
    lv.refr_now(None)


def _dispatch():
    while _changed:
        handler, value = _changed.pop(0)
        try:
            handler(value) if value is not None else handler()
        except Exception as e:
            import sys

            sys.print_exception(e)


def run():
    """QApplication.exec_(): redraw the sinks until quit() or Ctrl-C."""
    global _quit, _relayout, interrupted
    if headless():
        from gnuradio import _console

        return _console.run()
    _quit = False
    _relayout = False
    interrupted = False
    if lv is None or _head is None:
        show()
    due = [time.ticks_ms()] * len(_active)
    _busy[0] = 0
    _busy[1] = time.ticks_ms()
    try:
        while not _quit:
            now = time.ticks_ms()
            t0 = now
            for k, s in enumerate(_active):
                if time.ticks_diff(now, due[k]) >= 0:
                    s._draw()
                    s._canvas.invalidate()
                    _frames[s._title] = _frames.get(s._title, 0) + 1
                    due[k] = time.ticks_add(now, int(s._update * 1000))
            for t in _timers:
                if t._ms and time.ticks_diff(now, t._next) >= 0:
                    t._next = time.ticks_add(now, t._ms)
                    t.timeout.emit()
            for fn in _pollers[:]:
                try:
                    fn()
                except Exception as e:
                    # A failing engine stops rather than failing again at every pass.
                    import sys

                    sys.print_exception(e)
                    poll(fn, False)
            lv.task_handler()
            _dispatch()
            if _relayout:
                _relayout = False
                _layout()
                lv.refr_now(None)
                due = [time.ticks_ms()] * len(_active)
            _busy[0] += time.ticks_diff(time.ticks_ms(), t0)
            time.sleep_ms(5)
    except KeyboardInterrupt:
        import signal

        interrupted = True
        h = signal._handlers.get(signal.SIGINT)
        if not callable(h):
            raise
        h(signal.SIGINT, None)
    finally:
        # The window closes with the loop, as when a desktop export exits: the export's
        # closeEvent() stops the flowgraph (a no-op after its SIGINT handler did).
        _close_window()
        # The bands keep their last picture: say so.
        if _head is not None:
            _header(True)
            lv.task_handler()
    return 0


def _close_window():
    close = getattr(type(root), "closeEvent", None) if root is not None else None
    if close is None:
        return
    from PyQt5._nothing import Nothing

    try:
        close(root, Nothing())
    except Exception as e:
        import sys

        sys.print_exception(e)


def stats():
    """Redraws per sink and the share of the loop's time spent drawing, since run() began."""
    span = max(1, time.ticks_diff(time.ticks_ms(), _busy[1]))
    return {"frames": dict(_frames), "seconds": span / 1000, "drawing": _busy[0] / span}
