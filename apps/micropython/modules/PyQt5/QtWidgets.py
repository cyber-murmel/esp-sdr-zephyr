# SPDX-License-Identifier: GPL-3.0-or-later
"""PyQt5.QtWidgets stand-in."""

from PyQt5._nothing import Nothing


def __getattr__(name):
    return Nothing()
