# SPDX-License-Identifier: GPL-3.0-or-later
"""A burst's half-band filter (apps/micropython/cmod/esdr_hb.c) against the loop it replaced in
esdr_remote.c, kept here line for line: the same pairs out, bit for bit, however the burst falls
into blocks."""

import array
import ctypes
import random
import struct

import pytest

# ESDR_BURST_MAX: one burst at 40 MS/s from 80 MS/s, and the 8187 pairs it has always given.
BURST = 16380
OUT = 8187


class Hb(ctypes.Structure):
    """struct esdr_hb of apps/micropython/cmod/esdr_hb.h."""
    _fields_ = [("i", ctypes.c_int32 * 7), ("q", ctypes.c_int32 * 7), ("n", ctypes.c_uint32)]


def c_div_32(x):
    return -(-x // 32) if x < 0 else x // 32


def wrap16(x):
    return ((x + 0x8000) & 0xFFFF) - 0x8000


class Old:
    """The per-sample loop as it was: memmoves, then (int16_t)(sum / 32)."""

    def __init__(self):
        self.i = [0] * 7
        self.q = [0] * 7
        self.n = 0

    def block(self, pairs):
        out = []
        for i, q in pairs:
            self.i[1:7] = self.i[0:6]
            self.q[1:7] = self.q[0:6]
            self.i[0], self.q[0] = i, q
            self.n += 1
            if self.n & 1 or self.n < 7:
                continue
            a, b = self.i, self.q
            out.append(wrap16(c_div_32(16 * a[3] + 9 * (a[2] + a[4]) - (a[0] + a[6]))))
            out.append(wrap16(c_div_32(16 * b[3] + 9 * (b[2] + b[4]) - (b[0] + b[6]))))
        return out


def pack(pairs, wide):
    fmt = "<hh" if wide else "<bb"
    return b"".join(struct.pack(fmt, i, q) for i, q in pairs)


def run(lib, hb, pairs, wide):
    out = array.array("h", [0x5555] * (2 * ((len(pairs) + 1) // 2) + 2))
    made = lib.esdr_hb_block(ctypes.byref(hb), pack(pairs, wide), len(pairs), wide,
                             out.buffer_info()[0])
    assert made <= (len(pairs) + 1) // 2
    # Nothing written past what it reports.
    assert list(out[2 * made:]) == [0x5555] * (len(out) - 2 * made)
    return list(out[:2 * made])


@pytest.fixture
def lib(hb_clib):
    if hb_clib is None:
        pytest.skip("no host C compiler")
    return hb_clib


def noise(rng, n, wide):
    top = 32767 if wide else 127
    return [(rng.randint(-top - 1, top), rng.randint(-top - 1, top)) for _ in range(n)]


@pytest.mark.parametrize("wide", [True, False])
@pytest.mark.parametrize("seed", range(8))
def test_same_pairs_as_the_old_loop_in_any_blocks(lib, wide, seed):
    rng = random.Random(seed)
    pairs = noise(rng, 3000, wide)
    want = Old().block(pairs)
    hb = Hb()
    got = []
    at = 0
    while at < len(pairs):
        n = rng.choice((1, 2, 3, 7, 8, 255, 1020))
        got += run(lib, hb, pairs[at:at + n], wide)
        at += n
    assert got == want


def test_a_whole_burst_in_the_links_blocks(lib):
    # The C6 sends int16 blocks of 1020 pairs: raw 10 bits times 32.
    rng = random.Random(9)
    pairs = [(rng.randint(-512, 511) * 32, rng.randint(-512, 511) * 32) for _ in range(BURST)]
    hb = Hb()
    got = []
    for at in range(0, BURST, 1020):
        got += run(lib, hb, pairs[at:at + 1020], True)
    assert len(got) == 2 * OUT
    assert got == Old().block(pairs)


def test_full_scale_wraps_as_it_always_did(lib):
    # The gain overshoots 16 bits only for a pattern real samples (10 bits times 32) cannot reach.
    pairs = [(32767, -32768), (-32768, 32767)] * 40 + [(-32768, 32767)] * 3 + [(32767, -32768)] * 9
    assert run(lib, Hb(), pairs, True) == Old().block(pairs)


def test_a_filter_keeps_its_state_between_blocks_and_starts_clean(lib):
    pairs = noise(random.Random(4), 40, True)
    hb = Hb()
    first = run(lib, hb, pairs[:13], True) + run(lib, hb, pairs[13:], True)
    assert first == run(lib, Hb(), pairs, True)
    assert hb.n == 40


def test_nothing_before_the_filter_has_filled(lib):
    pairs = [(100, -100)] * 7
    assert run(lib, Hb(), pairs, True) == []
    assert len(run(lib, Hb(), pairs + [(100, -100)], True)) == 2


def test_a_constant_passes_at_unit_gain(lib):
    assert run(lib, Hb(), [(320, -320)] * 40, True)[-2:] == [320, -320]
