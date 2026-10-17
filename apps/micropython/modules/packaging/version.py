# SPDX-License-Identifier: GPL-3.0-or-later
"""packaging.version.Version for the Qt version checks of older GRC exports."""


class Version:
    def __init__(self, s):
        self._t = tuple(int(p) if p.isdigit() else 0 for p in str(s).split("."))

    def __lt__(self, o):
        return self._t < o._t

    def __le__(self, o):
        return self._t <= o._t

    def __gt__(self, o):
        return self._t > o._t

    def __ge__(self, o):
        return self._t >= o._t

    def __eq__(self, o):
        return self._t == o._t


StrictVersion = Version
