# SPDX-License-Identifier: GPL-3.0-or-later
"""The stand-in for any Qt object the board has no use for."""


class Nothing:
    """Takes any call and any attribute, does nothing, and is false (so an export's
    `if geometry:` skips what follows)."""

    def __init__(self, *args, **kwargs):
        pass

    def __call__(self, *args, **kwargs):
        return Nothing()

    def __getattr__(self, name):
        return Nothing()

    def __bool__(self):
        return False

    def __iter__(self):
        return iter(())

    def __int__(self):
        return 0
