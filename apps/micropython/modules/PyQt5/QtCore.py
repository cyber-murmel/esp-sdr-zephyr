# SPDX-License-Identifier: GPL-3.0-or-later
"""PyQt5.QtCore stand-in (QtCore.Qt.Horizontal and the like)."""

from PyQt5._nothing import Nothing
from PyQt5.Qt import QMetaObject, QObject, QTimer, Q_ARG, qVersion

Qt = Nothing()


def pyqtSignal(*args, **kwargs):
    from gnuradio import _gui

    return _gui.Signal()


def pyqtSlot(*args, **kwargs):
    def deco(f):
        return f

    return deco


def __getattr__(name):
    return Nothing()
