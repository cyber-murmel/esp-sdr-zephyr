# SPDX-License-Identifier: GPL-3.0-or-later
"""Window constants of GRC scripts; the window functions are not available."""

WIN_HAMMING = 0
WIN_HANN = 1
WIN_BLACKMAN = 2
WIN_RECTANGULAR = 3
WIN_KAISER = 4
WIN_BLACKMAN_HARRIS = 5
WIN_BLACKMAN_hARRIS = 5
WIN_BARTLETT = 6
WIN_FLATTOP = 7
WIN_NUTTALL = 8
WIN_NUTTALL_CFD = 8
WIN_WELCH = 9
WIN_PARZEN = 10
WIN_EXPONENTIAL = 11
WIN_RIEMANN = 12
WIN_GAUSSIAN = 13
WIN_TUKEY = 14


def _na(*args, **kw):
    raise NotImplementedError("fft.window functions are not available on the board")


hamming = hann = blackman = rectangular = kaiser = blackman_harris = _na
