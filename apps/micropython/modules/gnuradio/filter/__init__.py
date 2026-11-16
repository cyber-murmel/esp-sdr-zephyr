# SPDX-License-Identifier: GPL-3.0-or-later
"""Imported by every GRC script; no filter blocks on the board yet."""


class firdes:
    @staticmethod
    def low_pass(*args, **kw):
        raise NotImplementedError("firdes is not available on the board")

    high_pass = low_pass
    band_pass = low_pass
    band_reject = low_pass
