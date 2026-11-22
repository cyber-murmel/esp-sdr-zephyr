# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/multiapp/src/streamgate.h: is a stream that runs one to protect (a consumer reads it)? Built for the PC."""

import ctypes
import shutil
import subprocess

import pytest
from conftest import REPO

SRC = REPO / "apps/multiapp/src"
W = 5000
WRAP = 1 << 32


@pytest.fixture(scope="module")
def gate(tmp_path_factory):
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        pytest.skip("no C compiler")
    tmp = tmp_path_factory.mktemp("sg")
    (tmp / "sg.c").write_text('#include "streamgate.h"\n'
                              "int live(unsigned now, unsigned last, unsigned since, unsigned w)"
                              "{return stream_consumer_live(now, last, since, w);}\n")
    so = tmp / "sg.so"
    r = subprocess.run([cc, "-shared", "-fPIC", "-O1", "-Wall", "-Werror", "-o", str(so), str(tmp / "sg.c"), f"-I{SRC}"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    c = ctypes.CDLL(str(so))
    c.live.argtypes = [ctypes.c_uint] * 4

    def live(now, last, since, w=W):
        return bool(c.live(now % WRAP, last % WRAP, since % WRAP, w))

    return live


def test_first_look_is_a_stream(gate):
    # the ring was just seen, no transfer finished yet: the first one gets its time
    assert gate(100_000, 0, 100_000)
    assert gate(100_000 + W - 1, 0, 100_000)
    assert not gate(100_000 + W, 0, 100_000)


def test_a_consumer_that_reads_keeps_it_a_stream(gate):
    assert gate(200_000, 199_990, 100_000)
    assert gate(200_000, 195_001, 100_000)


def test_a_transfer_stamped_just_after_now_is_live(gate):
    # Board 2026-11-27: the IN completion was stamped 1 ms after the caller's "now" (it used to set bit 0 of the
    # uptime); its unsigned age wrapped and a live 20 MS/s stream read as "no consumer" one poll in five.
    assert gate(200_000, 200_001, 100_000)
    assert gate(200_000, 200_000 + W - 1, 100_000)
    assert gate(WRAP - 1, 0 + 1, WRAP - 50_000)    # across the wrap


def test_a_consumer_that_stops_ends_it(gate):
    assert not gate(200_000, 194_999, 100_000)
    assert not gate(200_000 + 60_000, 194_999, 100_000)


def test_a_slow_consumer_is_still_one_within_the_window(gate):
    # a PC that pauses for 4 s (below the window) is not taken for gone
    assert gate(200_000, 196_000, 100_000)
    assert not gate(200_000, 194_000, 100_000)


def test_the_last_transfer_of_an_earlier_stream_is_no_news(gate):
    # transfer at 50 s, the ring seen again at 100 s, now 101 s: the stream is new, within its window
    assert gate(101_000, 50_000, 100_000)
    # and 6 s later, no transfer since: over
    assert not gate(106_001, 50_000, 100_000)


def test_wrap_of_the_uptime(gate):
    near = WRAP - 1000
    # ring seen just before the wrap, transfer after it
    assert gate(WRAP + 500, WRAP + 300, near)
    # the same, the consumer gone for 6 s across the wrap
    assert not gate(WRAP + 6000, near - 100, near - 200)
    # a last transfer that is "older than 2^31 ms" must not look newer than the ring (the signed compare did)
    assert not gate(3_000_000_000, 100, 2_900_000_000)
    assert gate(3_000_000_000, 100, 2_999_998_000)


def test_never_a_transfer_after_the_ring_started_long_ago(gate):
    assert not gate(10_000_000, 0, 1_000)

