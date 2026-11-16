# SPDX-License-Identifier: GPL-3.0-or-later
"""lib/esp_sdr's ring stage (src/esp_sdr_ring_stage.c, include/esp_sdr/esp_sdr_ring_stage.h),
built with the host compiler on a buffer of the test's in place of SRAM bank 2: the sink's
words in order, the stream index over holes and new runs, pairs averaged at half the rate, a
reader the sink laps, and samples overwritten while read."""

import ctypes
import shutil
import subprocess

import numpy as np
import pytest

from conftest import REPO

WORDS = 16240


class Reader(ctypes.Structure):
    _fields_ = [("pos", ctypes.c_uint32), ("rpos", ctypes.c_uint32), ("holes", ctypes.c_uint32),
                ("index", ctypes.c_uint64), ("lapped", ctypes.c_uint32), ("gen", ctypes.c_uint32)]


# Bulk work in C: the stage's bank, and n samples put in pieces of 2048 whose words are their
# stream index's low 32 bits (a reader keeping up after each piece, or none).
BANK_C = """
#include <stdint.h>
#include <esp_sdr/esp_sdr_ring_stage.h>

unsigned char esp_sdr_stage_host_bank[65536] __attribute__((aligned(16)));
static uint32_t piece[2048];

void put_run(struct esp_sdr_stage_reader *r, uint64_t at, uint64_t n)
{
	for (uint64_t k = 0; k < n; k += 2048U) {
		const uint32_t m = n - k < 2048U ? (uint32_t)(n - k) : 2048U;
		const uint32_t *w;
		uint64_t idx;
		uint32_t got;

		for (uint32_t j = 0; j < m; j++) {
			piece[j] = (uint32_t)(at + k + j);
		}
		esp_sdr_stage_put((const int16_t *)piece, m, at + k);
		while (r != 0 && (got = esp_sdr_stage_peek(r, &w, &idx, 4096U)) != 0U) {
			esp_sdr_stage_advance(r, got);
		}
	}
}
"""


@pytest.fixture(scope="session")
def lib(tmp_path_factory):
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        pytest.skip("no C compiler")
    d = tmp_path_factory.mktemp("stage")
    bank = d / "bank.c"
    bank.write_text(BANK_C)
    out = d / "libstage.so"
    subprocess.run([cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                    "-DESP_SDR_STAGE_HOST", "-I" + str(REPO / "include"), "-o", str(out),
                    str(REPO / "lib/esp_sdr/src/esp_sdr_ring_stage.c"), str(bank)], check=True)
    so = ctypes.CDLL(str(out))
    so.esp_sdr_stage_init.argtypes = [ctypes.c_bool]
    so.esp_sdr_stage_sink.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                                      ctypes.c_size_t, ctypes.c_uint64]
    so.esp_sdr_stage_written.restype = ctypes.c_uint32
    so.esp_sdr_stage_reader_init.argtypes = [ctypes.POINTER(Reader)]
    so.esp_sdr_stage_peek.restype = ctypes.c_uint32
    so.esp_sdr_stage_peek.argtypes = [ctypes.POINTER(Reader), ctypes.POINTER(ctypes.c_void_p),
                                      ctypes.POINTER(ctypes.c_uint64), ctypes.c_uint32]
    so.esp_sdr_stage_advance.restype = ctypes.c_bool
    so.esp_sdr_stage_advance.argtypes = [ctypes.POINTER(Reader), ctypes.c_uint32]
    so.esp_sdr_stage_put.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint64]
    so.put_run.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint64]
    return so


class Stage:
    def __init__(self, lib, halve=False):
        self.lib = lib
        lib.esp_sdr_stage_init(halve)
        self.r = Reader()
        lib.esp_sdr_stage_reader_init(ctypes.byref(self.r))

    def run_start(self):
        self.lib.esp_sdr_stage_run_start()

    def sink(self, i, q, index):
        i = np.ascontiguousarray(i, np.int16)
        q = np.ascontiguousarray(q, np.int16)
        self.lib.esp_sdr_stage_sink(None, i.ctypes.data, q.ctypes.data, len(i), index)

    def read(self, max_words=64):
        """Everything new: (I, Q, stream index) per sample, each run's advance's verdict."""
        out, oks = [], []
        while True:
            w = ctypes.c_void_p()
            idx = ctypes.c_uint64()
            n = self.lib.esp_sdr_stage_peek(ctypes.byref(self.r), ctypes.byref(w),
                                            ctypes.byref(idx), max_words)
            if n == 0:
                return out, oks
            words = np.frombuffer((ctypes.c_uint32 * n).from_address(w.value), np.uint32)
            for k, v in enumerate(words):
                out.append((np.int16(v & 0xFFFF), np.int16(v >> 16), idx.value + k))
            oks.append(self.lib.esp_sdr_stage_advance(ctypes.byref(self.r), n))


def ramp(a, n):
    k = np.arange(a, a + n)
    return (k % 30000).astype(np.int16), (-(k % 30000)).astype(np.int16)


def test_words_in_order(lib):
    st = Stage(lib)
    for a in range(0, 1000, 100):
        st.sink(*ramp(a, 100), a)
    got, oks = st.read(64)
    assert [g[2] for g in got] == list(range(1000))
    assert [int(g[0]) for g in got] == list(range(1000))
    assert [int(g[1]) for g in got] == [-k for k in range(1000)]
    assert all(oks)
    assert lib.esp_sdr_stage_written() == 1000


def test_holes_move_the_index(lib):
    st = Stage(lib)
    st.sink(*ramp(0, 100), 0)
    st.sink(*ramp(100, 100), 150)  # 50 samples lost
    st.run_start()
    st.sink(*ramp(200, 100), 0)  # a new run counts from 0 again
    got, _ = st.read(1000)
    idx = [g[2] for g in got]
    assert idx[:100] == list(range(100))
    assert idx[100:200] == list(range(150, 250))
    assert idx[200:] == list(range(251, 351))
    # Each hole ends a peek: three runs.
    st2 = Stage(lib)
    st2.sink(*ramp(0, 10), 0)
    st2.sink(*ramp(10, 10), 30)
    w, i = ctypes.c_void_p(), ctypes.c_uint64()
    assert lib.esp_sdr_stage_peek(ctypes.byref(st2.r), ctypes.byref(w), ctypes.byref(i), 64) == 10


def test_pairs_averaged(lib):
    """At half the rate: samples 2m and 2m + 1 as one, their mean, index m; a call may end
    between the two; a hole between them drops the pair."""
    st = Stage(lib, halve=True)
    i = np.arange(0, 400, dtype=np.int16) * 2
    q = -i
    for a, b in ((0, 7), (7, 64), (64, 129), (129, 200)):
        st.sink(i[a:b], q[a:b], a)
    got, _ = st.read(1000)
    assert [g[2] for g in got] == list(range(100))
    assert [int(g[0]) for g in got] == [(4 * m + 4 * m + 2) // 2 for m in range(100)]
    assert [int(g[1]) for g in got] == [-(4 * m + 1) for m in range(100)]
    # Pair 100's first half arrives, then the stream jumps: the pair is dropped, 105 follows.
    st.sink(i[200:201], q[200:201], 200)
    st.sink(i[210:220], q[210:220], 210)
    got, _ = st.read(1000)
    assert [g[2] for g in got] == list(range(105, 110))


def test_a_lapped_reader_resumes(lib):
    st = Stage(lib)
    n = 0
    for _ in range(3 * WORDS // 512):
        st.sink(*ramp(n, 512), n)
        n += 512
    got, _ = st.read(4096)
    assert st.r.lapped == 1
    # From half a stage back, every sample on its own index.
    assert len(got) == WORDS // 2
    assert all(int(v) == k % 30000 for v, _, k in got)
    assert got[-1][2] == n - 1


def test_overwritten_while_read(lib):
    st = Stage(lib)
    st.sink(*ramp(0, 100), 0)
    w, i = ctypes.c_void_p(), ctypes.c_uint64()
    m = lib.esp_sdr_stage_peek(ctypes.byref(st.r), ctypes.byref(w), ctypes.byref(i), 64)
    assert m == 64
    # The sink comes round before the reader is done.
    k = 100
    while k < WORDS + 100:
        st.sink(*ramp(k, 500), k)
        k += 500
    assert not lib.esp_sdr_stage_advance(ctypes.byref(st.r), m)


def peek1(lib, st):
    """The next run of words and its index: (n, index, first word)."""
    w, i = ctypes.c_void_p(), ctypes.c_uint64()
    n = lib.esp_sdr_stage_peek(ctypes.byref(st.r), ctypes.byref(w), ctypes.byref(i), 4096)
    first = ctypes.c_uint32.from_address(w.value).value if n else None
    return n, i.value, first


@pytest.mark.parametrize("keep_up", [True, False])
def test_index_after_2_31_samples(lib, keep_up):
    """A run without a hole for 2^31 samples and more, then a lap: the index still follows
    (it came out 0 when the lap's place was found against the written count), and so does a new
    run's hole after it."""
    st = Stage(lib)
    n = (1 << 31) + 100000
    lib.put_run(ctypes.byref(st.r) if keep_up else None, 0, n)
    if keep_up:
        assert st.r.index == n and st.r.lapped == 0
    lib.put_run(None, n, 10 * 2048)
    got, index, first = peek1(lib, st)
    assert got > 0 and st.r.lapped == 1
    assert index & 0xFFFFFFFF == first
    assert n + 10 * 2048 - WORDS // 2 - 64 <= index <= n + 10 * 2048 - WORDS // 2
    # Read to the end, then a new run: one more than the last index.
    while got:
        lib.esp_sdr_stage_advance(ctypes.byref(st.r), got)
        got, index, first = peek1(lib, st)
    end = st.r.index
    st.run_start()
    lib.put_run(None, 0, 100)
    got, index, _ = peek1(lib, st)
    assert got == 100 and index == end + 1


def test_a_stage_started_over(lib):
    """esp_sdr_stage_init() again under a reader (the S31 after a paused run): the reader starts
    over at the new stage's oldest sample, its old words no longer stand."""
    st = Stage(lib)
    lib.put_run(None, 0, 5000)
    got, index, _ = peek1(lib, st)
    assert got and index == 0
    lib.esp_sdr_stage_init(False)
    assert not lib.esp_sdr_stage_advance(ctypes.byref(st.r), got)
    lib.put_run(None, 0, 300)
    got, index, first = peek1(lib, st)
    assert (got, index, first) == (300, 0, 0)
    assert st.r.lapped == 1


def test_more_holes_than_the_log(lib):
    """A reader 40 holes behind (the log keeps 32): it laps, and the index it resumes at is the
    one its words carry."""
    st = Stage(lib)
    at = 0
    for _ in range(40):
        lib.put_run(None, at, 100)
        at += 150  # 50 lost each time: a hole
    got, index, first = peek1(lib, st)
    assert st.r.lapped >= 1
    assert got and index & 0xFFFFFFFF == first
    total = 0
    while got:
        _, index, first = peek1(lib, st)
        assert index & 0xFFFFFFFF == first
        lib.esp_sdr_stage_advance(ctypes.byref(st.r), got)
        total += got
        got, index, first = peek1(lib, st)
    assert st.r.index == at - 50
