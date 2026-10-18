# SPDX-License-Identifier: GPL-3.0-or-later
"""PyQt5.Qt stand-in for GRC Qt GUI exports on the board. The widgets and layouts an export
builds form a tree (QWidget, QScrollArea, QTabWidget, QVBoxLayout, QGridLayout, ...);
QApplication.exec_() shows what is reachable from the flowgraph, placed by gnuradio/_layout.py,
with LVGL (gnuradio/_gui.py): the qtgui sinks and Range variables, and the controls of the
Chooser, Check Box, Push Button, Label and Entry blocks. A QTabWidget shows one page at a time;
a QScrollArea does not scroll (what does not fit is cut off). Styling and whatever else has no
use here is accepted and ignored."""

from PyQt5._nothing import Nothing
from gnuradio import _gui, _layout

ROW = _gui.ROW


class QObject:
    def __init__(self, *args, **kwargs):
        pass

    def __getattr__(self, name):
        return Nothing()


class QWidget(QObject):
    """Base class of an exported flowgraph (class rx(gr.top_block, Qt.QWidget)) and of windows."""

    def __init__(self, parent=None, *args, **kwargs):
        from gnuradio import gr

        self._layout = None
        self._child = None
        # A new flowgraph: the sinks and variables of an earlier one leave the display.
        if isinstance(self, gr.top_block):
            _gui.reset(self)

    def setLayout(self, layout):
        self._layout = layout

    def layout(self):
        return self._layout

    def setWidget(self, widget):  # QScrollArea
        self._child = widget

    def _content(self):
        return [x for x in (self._layout, self._child) if x is not None]

    def _need(self):
        f = n = 0
        for x in self._content():
            a, b = _layout.need(x)
            f += a
            n += b
        return (f, n)

    def _place(self, x, y, w, h, out):
        _layout.stack(self._content(), x, y, w, h, out)

    def setWindowTitle(self, title):
        _gui.title(title)

    def show(self):
        _gui.show()

    def close(self):
        _gui.quit()
        return True


class QScrollArea(QWidget):
    pass


class QTabWidget(QWidget):
    """A bar of buttons, one per tab, and the current page below it."""

    _flex = False
    _h = ROW

    def __init__(self, *args, **kwargs):
        QWidget.__init__(self)
        self._pages = []
        self._names = []
        self._cur = 0
        self.currentChanged = _gui.Signal()

    def addTab(self, widget, name, *args):
        self._pages.append(widget)
        self._names.append(str(name))
        return len(self._pages) - 1

    def currentIndex(self):
        return self._cur

    def setCurrentIndex(self, i):
        i = int(i)
        if 0 <= i < len(self._pages) and i != self._cur:
            self._cur = i
            _gui.relayout()
            self.currentChanged.emit(i)

    def _need(self):
        if not self._pages:
            return (0, 0)
        f, n = _layout.need(self._pages[self._cur])
        return (f + ROW, n)

    def _place(self, x, y, w, h, out):
        if self._pages:
            out.append((self, x, y, w, ROW))
            _layout.place(self._pages[self._cur], x, y + ROW, w, h - ROW, out)

    def _make(self, parent, x, y, w, h):
        lv = _gui.lv
        n = len(self._names)
        for i, name in enumerate(self._names):
            x0 = w * i // n
            x1 = w * (i + 1) // n
            b = lv.button(parent)
            b.set_size(x1 - x0 - 2, ROW - 2)
            b.set_pos(x + x0, y + 1)
            if i == self._cur:
                b.set_style_bg_color(lv.color_hex(0x2060C0), 0)
            label = lv.label(b)
            label.set_text(name[: (x1 - x0) // 8])
            label.center()
            _gui.on(b, lv.EVENT.CLICKED, self._select(i))
            _gui.add_to_group(b)

    def _select(self, i):
        return lambda: self.setCurrentIndex(i)


class QGroupBox(QWidget):
    """A title row above the widgets of its layout."""

    def __init__(self, title="", *args, **kwargs):
        QWidget.__init__(self)
        self._title = QLabel(title)

    def _content(self):
        return [self._title] + QWidget._content(self)


class QLayout(QObject):
    def __init__(self, parent=None, *args, **kwargs):
        if parent is not None and hasattr(type(parent), "setLayout"):
            parent.setLayout(self)

    def addWidget(self, widget, *args, **kwargs):
        self.add(widget, *args)

    addLayout = addWidget

    def addStretch(self, *args):
        pass


class QVBoxLayout(_layout.Box, QLayout):
    def __init__(self, parent=None):
        _layout.Box.__init__(self)
        QLayout.__init__(self, parent)


class QHBoxLayout(_layout.HBox, QLayout):
    def __init__(self, parent=None):
        _layout.HBox.__init__(self)
        QLayout.__init__(self, parent)


class QGridLayout(_layout.Grid, QLayout):
    def __init__(self, parent=None):
        _layout.Grid.__init__(self)
        QLayout.__init__(self, parent)


class QBoxLayout(_layout.Box, QLayout):
    LeftToRight = 0
    RightToLeft = 1
    TopToBottom = 2
    BottomToTop = 3

    def __init__(self, direction=2, parent=None):
        _layout.Box.__init__(self)
        QLayout.__init__(self, parent)
        self.horizontal = direction in (0, 1)


class QApplication:
    def __init__(self, *args, **kwargs):
        pass

    def exec_(self):
        return _gui.run()

    exec = exec_

    @staticmethod
    def quit():
        _gui.quit()

    @staticmethod
    def instance():
        return None

    def __getattr__(self, name):
        return Nothing()


class QTimer:
    """Calls its timeout handlers from the display loop."""

    def __init__(self, *args, **kwargs):
        self.timeout = _gui.Signal()
        self._ms = 0

    def start(self, ms=None):
        if ms is not None:
            self._ms = int(ms)
        _gui.timer(self)

    def stop(self):
        _gui.timer(self, False)

    def setInterval(self, ms):
        self._ms = int(ms)

    def __getattr__(self, name):
        return Nothing()


class QMetaObject:
    @staticmethod
    def invokeMethod(obj, name, *args):
        getattr(obj, name)(*args)


def Q_ARG(kind, value):
    return value


# ---- controls: each is a row (or part of one) on the display ----


class _Control(QObject):
    _flex = False
    _h = ROW


class QLabel(_Control):
    def __init__(self, text="", *args, **kwargs):
        self._text = str(text)
        self._obj = None

    def setText(self, text):
        self._text = str(text)
        if self._obj is not None:
            self._obj.set_text(self._text)
        elif _gui._headless:
            from gnuradio import _console

            if _console.active():
                _console.label(self, self._text)

    def text(self):
        return self._text

    def _inline(self, parent, x, y, w):
        self._obj = _gui.lv.label(parent)
        self._obj.set_text(self._text)
        self._obj.set_pos(x, y + 5)
        return min(8 * len(self._text) + 6, w)

    def _make(self, parent, x, y, w, h):
        self._inline(parent, x + 2, y, w - 4)


class QLineEdit(QLabel):
    """Shown as text; the board has no keyboard to edit it with."""

    def __init__(self, text="", *args, **kwargs):
        QLabel.__init__(self, text)
        self.editingFinished = _gui.Signal()
        self.returnPressed = _gui.Signal()


class QComboBox(_Control):
    def __init__(self, *args, **kwargs):
        self._items = []
        self._idx = 0
        self._obj = None
        self.currentIndexChanged = _gui.Signal()

    def addItem(self, text, *args):
        self._items.append(str(text))

    def currentIndex(self):
        return self._idx

    def currentText(self):
        return self._items[self._idx] if self._items else ""

    def setCurrentIndex(self, i):
        i = int(i)
        if i == self._idx or not 0 <= i < len(self._items):
            return
        self._idx = i
        if self._obj is not None:
            self._obj.set_selected(i)
        self.currentIndexChanged.emit(i)

    def _picked(self, i):
        if i != self._idx:
            self._idx = i
            self.currentIndexChanged.emit(i)

    def _inline(self, parent, x, y, w):
        lv = _gui.lv
        d = lv.dropdown(parent)
        d.set_options("\n".join(self._items))
        d.set_selected(self._idx)
        d.set_width(min(w, 150))
        d.set_height(ROW - 2)
        d.set_style_pad_all(2, 0)
        d.set_pos(x, y + 1)
        _gui.on(d, lv.EVENT.VALUE_CHANGED, self._picked, d.get_selected)
        _gui.add_to_group(d)
        self._obj = d
        return min(w, 150)

    def _make(self, parent, x, y, w, h):
        self._inline(parent, x + 2, y, w - 4)


class QCheckBox(_Control):
    def __init__(self, text="", *args, **kwargs):
        self._text = str(text)
        self._on = False
        self._obj = None
        self.stateChanged = _gui.Signal()
        self.toggled = _gui.Signal()
        self.clicked = _gui.Signal()

    def isChecked(self):
        return self._on

    def setChecked(self, on):
        on = bool(on)
        if on == self._on:
            return
        self._on = on
        if self._obj is not None:
            lv = _gui.lv
            if on:
                self._obj.add_state(lv.STATE.CHECKED)
            else:
                self._obj.remove_state(lv.STATE.CHECKED)
        self.toggled.emit(on)
        self.stateChanged.emit(2 if on else 0)

    def _sync(self):
        """The user toggled the LVGL widget."""
        on = self._obj.has_state(_gui.lv.STATE.CHECKED)
        if on != self._on:
            self._on = on
            self.toggled.emit(on)
            self.stateChanged.emit(2 if on else 0)
            self.clicked.emit(on)

    def _make(self, parent, x, y, w, h):
        lv = _gui.lv
        c = lv.checkbox(parent)
        c.set_text(self._text)
        c.set_pos(x + 2, y + 1)
        if self._on:
            c.add_state(lv.STATE.CHECKED)
        self._obj = c
        _gui.on(c, lv.EVENT.VALUE_CHANGED, self._sync)
        _gui.add_to_group(c)


class QRadioButton(QCheckBox):
    """A check box that a QButtonGroup keeps exclusive."""

    def __init__(self, text="", *args, **kwargs):
        QCheckBox.__init__(self, text)
        self._group = None

    def _sync(self):
        QCheckBox._sync(self)
        if self._on and self._group is not None:
            self._group._chosen(self)

    def setChecked(self, on):
        QCheckBox.setChecked(self, on)
        if self._on and self._group is not None:
            self._group._chosen(self, False)

    def _make(self, parent, x, y, w, h):
        QCheckBox._make(self, parent, x, y, w, h)
        # Round marks tell radio buttons from check boxes.
        lv = _gui.lv
        self._obj.set_style_radius(0x7FFF, lv.PART.INDICATOR)


class QButtonGroup(QObject):
    def __init__(self, parent=None):
        self._buttons = []
        self._ids = []
        self.buttonClicked = _gui.Signal()
        self.buttonToggled = _gui.Signal()

    def addButton(self, button, id=-1):
        if id < 0:
            id = len(self._buttons)
        self._buttons.append(button)
        self._ids.append(id)
        button._group = self

    def button(self, id):
        return self._buttons[self._ids.index(id)]

    def checkedId(self):
        for b, i in zip(self._buttons, self._ids):
            if b._on:
                return i
        return -1

    def _chosen(self, button, user=True):
        for b in self._buttons:
            if b is not button and b._on:
                b._on = False
                if b._obj is not None:
                    b._obj.remove_state(_gui.lv.STATE.CHECKED)
        if user:
            self.buttonClicked.emit(self._ids[self._buttons.index(button)])


class QPushButton(_Control):
    def __init__(self, text="", *args, **kwargs):
        self._text = str(text)
        self.pressed = _gui.Signal()
        self.released = _gui.Signal()
        self.clicked = _gui.Signal()

    def _make(self, parent, x, y, w, h):
        lv = _gui.lv
        b = lv.button(parent)
        b.set_size(min(w - 4, 12 * len(self._text) + 24), h - 2)
        b.set_pos(x + 2, y + 1)
        label = lv.label(b)
        label.set_text(self._text)
        label.center()
        _gui.on(b, lv.EVENT.PRESSED, self.pressed.emit)
        _gui.on(b, lv.EVENT.RELEASED, self.released.emit)
        _gui.on(b, lv.EVENT.CLICKED, self.clicked.emit)
        _gui.add_to_group(b)
        self._obj = b


class QToolBar(_Control):
    """A row of widgets: Chooser, Label and Entry blocks put a QLabel and a control in one."""

    def __init__(self, *args, **kwargs):
        self._widgets = []

    def addWidget(self, widget, *args):
        self._widgets.append(widget)
        widget._bar = self

    def _make(self, parent, x, y, w, h):
        x0 = x + 2
        for wd in self._widgets:
            if hasattr(type(wd), "_inline"):
                x0 += wd._inline(parent, x0, y, x + w - x0 - 2) + 6


def qVersion():
    return "5.15.2"


Qt = Nothing()


def __getattr__(name):
    # QFrame.NoFrame, QIcon.fromTheme, QSettings, QSizePolicy, ...
    return Nothing()
