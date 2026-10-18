# SPDX-License-Identifier: GPL-3.0-or-later
"""The Qt GUI of GRC exports on a board without a display (or with /FLASH:/headless): the
console instead of the screen. QApplication.exec_() lists the flowgraph's controls (of every
tab page) with numbers and runs the display loop's work (timers, packet radios, messages) from
a timer, while lines typed at the console work the controls:

  N           press push button N, toggle check box N, choose radio button N
  N VALUE     set range N, choose N's option (label or index), set entry N, check box N to
              1 or 0; for a QT GUI Msg Push Button, send VALUE instead of its own value
  ?           the controls again
  !STATEMENT  run a Python statement

QT GUI Label values print as they change; Ctrl-C ends the flowgraph as on the display (the
export's SIGINT handler). The sinks are not shown."""

import sys

from gnuradio import _gui

PERIOD_MS = 10

_controls = []
_on = False
_ns = {}


def active():
    return _on


def _collect(root, out):
    """The controls and value labels under root, every tab page, in layout order. A walk with
    a list for a stack: the layouts nest deeper than a small board's C stack likes to recurse."""
    from PyQt5 import Qt
    from gnuradio import _layout

    todo = [root]
    while todo:
        node = todo.pop()
        if isinstance(node, tuple):  # a tab's name, placed before its page
            out.append(node)
            continue
        kids = None
        if isinstance(node, Qt.QTabWidget):
            kids = []
            for name, page in zip(node._names, node._pages):
                kids += [("tab", name), page]
        elif isinstance(node, Qt.QWidget):  # QScrollArea, QGroupBox too
            kids = node._content()
        elif isinstance(node, _layout.Grid):
            kids = [c[0] for c in sorted(node._cells, key=lambda c: (c[1], c[2]))]
        elif isinstance(node, _layout.Box):
            kids = node._items
        elif isinstance(node, Qt.QToolBar):
            ws = node._widgets
            caption = ws[0].text() if ws and _kind(ws[0]) == "label" else ""
            for w in ws[1 if caption else 0:]:
                out.append((w, caption))
        elif hasattr(type(node), "_make") and not getattr(type(node), "_draws", False):
            out.append((node, ""))
        if kids:
            todo.extend(reversed(kids))


def _kind(w):
    from PyQt5 import Qt
    from gnuradio import qtgui

    for cls, k in ((qtgui.MsgPushButton, "msgbutton"), (qtgui.RangeWidget, "range"),
                   (Qt.QComboBox, "combo"), (Qt.QRadioButton, "radio"), (Qt.QCheckBox, "check"),
                   (Qt.QLineEdit, "entry"), (Qt.QPushButton, "button"), (Qt.QLabel, "label")):
        if isinstance(w, cls):
            return k
    return None


def _describe(w, caption):
    caption = caption.strip()
    k = _kind(w)
    if k == "range":
        r = w.r
        return "{} = {}  ({} .. {}, step {})".format(w.label, _gui.eng(w.value), _gui.eng(r.min),
                                                    _gui.eng(r.max), _gui.eng(r.step))
    if k == "combo":
        return "{} {}  [{}]".format(caption, w.currentText(), "|".join(w._items))
    if k == "radio":
        return "({}) {}".format("*" if w._on else " ", w._text)
    if k == "check":
        return "[{}] {}".format("x" if w._on else " ", w._text)
    if k == "entry":
        return "{} {!r}".format(caption, w.text())
    if k == "msgbutton":
        return "<{}>  (N TEXT sends TEXT)".format(w._text)
    if k == "button":
        return "<{}>".format(w._text)
    return "{} {}".format(caption, w.text()).strip()


def show():
    """Print the window title and the controls."""
    global _controls
    found = []
    if _gui.root is not None:
        _collect(_gui.root, found)
    _controls = []
    print("== {} ==".format(_gui._title or "flowgraph"))
    for item in found:
        if item[0] == "tab":
            print("  -- {} --".format(item[1]))
            continue
        w, caption = item
        k = _kind(w)
        if k is None:
            continue
        if k == "label":
            print("     {}".format(_describe(w, caption)))
            continue
        _controls.append((w, caption))
        print("  {:2d} {}".format(len(_controls), _describe(w, caption)))


def label(w, text):
    """A label's text changed: print it with its caption."""
    bar = getattr(w, "_bar", None)
    caption = ""
    if bar is not None and bar._widgets and bar._widgets[0] is not w and \
            _kind(bar._widgets[0]) == "label":
        caption = bar._widgets[0].text()
    print("{} {}".format(caption.strip(), text).strip())


def command(line):
    line = line.strip()
    if not line:
        return
    if line in ("?", "help"):
        show()
        return
    if line[0] == "!":
        try:
            exec(line[1:], _ns)
        except Exception as e:
            sys.print_exception(e)
        return
    head, _, value = line.partition(" ")
    try:
        n = int(head)
        w, caption = _controls[n - 1]
        if n < 1:
            raise IndexError
    except (ValueError, IndexError):
        print("? N, N VALUE, ?, !STATEMENT (controls 1 .. {})".format(len(_controls)))
        return
    try:
        _apply(w, value.strip())
    except Exception as e:
        print("{}: {}".format(n, e))
        return
    k = _kind(w)
    if k not in ("button", "msgbutton"):
        print("  {:2d} {}".format(n, _describe(w, caption)))


def _apply(w, value):
    k = _kind(w)
    if k == "range":
        from gnuradio.eng_notation import str_to_num

        v = str_to_num(value) if value else w.value
        w.set_index(int(round((v - w.r.min) / w.r.step)))
    elif k == "combo":
        items = [x.lower() for x in w._items]
        if value.lower() in items:
            i = items.index(value.lower())
        else:
            i = int(value)
        w.setCurrentIndex(i)
    elif k == "radio":
        if not w._on:
            w._on = True
            w.toggled.emit(True)
            w.stateChanged.emit(2)
            w.clicked.emit(True)
            if w._group is not None:
                w._group._chosen(w)
    elif k == "check":
        on = (not w._on) if not value else value.lower() in ("1", "on", "true", "yes", "x")
        w.setChecked(on)
    elif k == "entry":
        w.setText(value)
        w.editingFinished.emit()
        w.returnPressed.emit()
    elif k == "msgbutton":
        w._send(value if value else w.msgValue)
    elif k == "button":
        w.pressed.emit()
        w.released.emit()
        w.clicked.emit()


def run():
    """QApplication.exec_() without a display: the controls on the console until Ctrl-C."""
    global _on
    import signal

    _gui._quit = False
    _gui.interrupted = False
    _on = True
    timer = None
    try:
        show()
        timer = _timer(True)
        while not _gui._quit:
            command(input())
    except KeyboardInterrupt:
        _gui.interrupted = True
        h = signal._handlers.get(signal.SIGINT)
        if not callable(h):
            raise
        h(signal.SIGINT, None)
    finally:
        if timer is not None:
            _timer(False, timer)
        _on = False
        _gui._close_window()
        print("== {} stopped ==".format(_gui._title or "flowgraph"))
    return 0


def _timer(on, t=None):
    """The display loop's work, every PERIOD_MS, also while input() waits (a virtual timer's
    callback runs from the scheduler)."""
    import machine

    if on:
        t = machine.Timer(-1)
        t.init(period=PERIOD_MS, mode=machine.Timer.PERIODIC, callback=lambda _t: tick())
        return t
    t.deinit()


def tick():
    """One pass of the loop's work: due timers, pollers (a packet radio), message delivery."""
    import time

    now = time.ticks_ms()
    for t in _gui._timers[:]:
        if t._ms and time.ticks_diff(now, t._next) >= 0:
            t._next = time.ticks_add(now, t._ms)
            t.timeout.emit()
    for fn in _gui._pollers[:]:
        try:
            fn()
        except Exception as e:
            sys.print_exception(e)
            _gui.poll(fn, False)
    _gui._dispatch()
