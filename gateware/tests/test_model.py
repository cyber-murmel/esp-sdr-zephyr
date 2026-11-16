# SPDX-License-Identifier: GPL-3.0-or-later
"""The reference model against floating point."""

import cmath
import math
import random

import numpy as np

from esdr_fpga import model


def test_fft_matches_numpy():
    rng = random.Random(2)
    for bits in (4, 6, 9):
        n = 1 << bits
        x = [complex(rng.randint(-11000, 11000), rng.randint(-11000, 11000)) for _ in range(n)]
        re, im, exp = model.fft([int(v.real) for v in x], [int(v.imag) for v in x])
        got = (np.array(re) + 1j * np.array(im)) * 2.0 ** exp
        want = np.fft.fft(np.array(x))
        err = np.abs(got - want)
        snr = 10 * math.log10(np.sum(np.abs(want) ** 2) / np.sum(err ** 2))
        assert snr > 45, (n, snr)


def test_fft_tone_dynamic_range():
    """At N = 512, a tone 55 dB below a full-scale one, over the radio's noise (a few LSB of
    10 bits), stands clear of the floor."""
    rng = random.Random(10)
    n = 512
    x = [20000 * cmath.exp(1j * model.TAU * 50.25 * k / n)
         + 36 * cmath.exp(1j * model.TAU * 150 * k / n)
         + complex(rng.gauss(0, 200), rng.gauss(0, 200)) for k in range(n)]
    x = [(round(v.real), round(v.imag)) for v in x]
    win = model.window_q15(5, n)
    l = model.spectrum_frame(x, win, 0)
    floor = sorted(l)[n // 2]
    assert l[150 ^ 256] - floor > 256 * 2      # over 6 dB above the median floor


def test_log2():
    for p in (1, 2, 3, 1000, 123456789, (1 << 31) - 1):
        assert abs(model.log2_units(p) / 256 - math.log2(p)) < 0.01


def test_average_matches_linear():
    alpha = 0.2
    tab, hi, lo = model.avg_table(alpha)
    rng = random.Random(3)
    s_lin = 1000.0
    s = round(256 * math.log2(s_lin))
    for _ in range(200):
        p = rng.uniform(10, 1e6)
        s_lin = alpha * p + (1 - alpha) * s_lin
        s = model.average(s, round(256 * math.log2(p)), tab, hi, lo)
        assert abs(s / 256 - math.log2(s_lin)) < 0.05   # 0.15 dB


def test_columns():
    bins = list(range(100))
    assert model.columns(bins, 10, 0, 10 << 8) == [9, 19, 29, 39, 49, 59, 69, 79, 89, 99]
    assert model.columns(bins, 4, 10, 1 << 7) == [10, 10, 11, 11]
