# SPDX-License-Identifier: GPL-3.0-or-later
"""signal for GRC scripts. Handlers are remembered; the Qt GUI display loop
(gnuradio/_gui.py) runs the SIGINT handler on Ctrl-C, as Qt's event loop would. Elsewhere
Ctrl-C raises KeyboardInterrupt."""

SIGINT = 2
SIGTERM = 15
SIG_DFL = 0
SIG_IGN = 1

_handlers = {}


def signal(sig, handler):
    old = _handlers.get(sig, SIG_DFL)
    _handlers[sig] = handler
    return old


def getsignal(sig):
    return _handlers.get(sig, SIG_DFL)
