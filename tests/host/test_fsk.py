# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/micropython: the (G)FSK modem of the gnuradio.digital stand-ins. The Python reference
(modules/gnuradio/digital/_fsk.py) from modulator to receiver through a channel with noise, a
carrier offset and a random phase; and the C modem the board runs (cmod/esdr_fsk.c), built here
with the host compiler, frame for frame against the reference (skipped without a compiler)."""

import array
import ctypes
import math
import random
import shutil
import subprocess

import pytest

from conftest import REPO, load

fsk = load("apps/micropython/modules/gnuradio/digital/_fsk.py", "gnuradio_digital_fsk")

# The datalink egg's modem: 250 kS/s, 25 kbaud, +-25 kHz, BT 0.5, preamble and sync word.
FS = 250000
SPS = 10
SENS = math.tau * 25e3 / FS
BT = 0.5
CODE = "01" * 16 + "11100100001011000110011001011101"
CODE_V, CODE_L = fsk.code_bits(CODE)
TAPS = fsk.gfsk_taps(SPS, BT)


def burst(payload, amp=0.5):
    """header_format_default's header and the payload, modulated (int16 I/Q)."""
    data = fsk.header(len(payload), CODE_V, CODE_L) + payload
    n = fsk.mod_samples(8 * len(data), SPS, len(TAPS))
    out = array.array("h", [0] * (2 * n))
    fsk.modulate(data, 8 * len(data), SPS, TAPS, SENS, amp, out)
    return out


def channel(iq, foff, snr_db, lead, tail, seed=1, amp=0.5):
    """iq after lead samples of noise, at foff Hz with a random phase, complex noise snr_db
    below the burst's level, tail samples of noise after it; halved, as the receiver's
    16-bit samples have headroom."""
    rnd = random.Random(seed)
    n = len(iq) // 2
    nr = amp * 32767 / 10 ** (snr_db / 20) / math.sqrt(2)
    out = array.array("h", [0] * (2 * (lead + n + tail)))
    ph = rnd.random() * math.tau
    step = math.tau * foff / FS
    for k in range(lead + n + tail):
        j = k - lead
        re = im = 0.0
        if 0 <= j < n:
            c, s = math.cos(ph), math.sin(ph)
            re = iq[2 * j] * c - iq[2 * j + 1] * s
            im = iq[2 * j] * s + iq[2 * j + 1] * c
        ph += step
        out[2 * k] = int(round((re + rnd.gauss(0, nr)) / 2))
        out[2 * k + 1] = int(round((im + rnd.gauss(0, nr)) / 2))
    return out


def receive(rx, iq, chunk=1000):
    frames = []
    n = len(iq) // 2
    for k in range(0, n, chunk):
        frames += rx.process(iq, min(chunk, n - k), k)
    return frames


def payload(seed, n=32):
    rnd = random.Random(seed)
    return bytes(rnd.getrandbits(8) for _ in range(n))


def test_angle():
    for k in range(-180, 181, 7):
        for r in (3, 100, 30000, 2 ** 30):
            a = math.radians(k + 0.3)
            got = fsk.angle(int(r * math.cos(a)), int(r * math.sin(a)))
            want = math.atan2(int(r * math.sin(a)), int(r * math.cos(a))) / math.tau * 65536
            err = (got - want + 32768) % 65536 - 32768
            assert abs(err) < (50 if r > 50 else 400), (k, r, got, want)
    assert fsk.angle(0, 0) == 0
    assert fsk.angle(-5, 0) == -32768


def test_header_format_default():
    # GNU Radio's header: access code, then the length twice, MSB first.
    h = fsk.header(0x123, CODE_V, CODE_L)
    assert h[:8] == CODE_V.to_bytes(8, "big")
    assert h[8:] == bytes([0x01, 0x23, 0x01, 0x23])
    with pytest.raises(ValueError):
        fsk.header(5, 5, 3)


def test_code_bits():
    assert fsk.code_bits("0110") == (6, 4)
    for bad in ("", "012", "1" * 65):
        with pytest.raises(ValueError):
            fsk.code_bits(bad)


def test_gfsk_taps():
    # filter.firdes.gaussian(1.0, 10, 0.5, 40) in GNU Radio 3.10.12.
    g = fsk.gaussian(1.0, 10, 0.5, 40)
    assert len(g) == 40
    assert g[19] == pytest.approx(0.150538370013, abs=1e-8)  # GNU Radio rounds to float32
    assert g[20] == pytest.approx(0.140193536878, abs=1e-8)
    assert g[2] == pytest.approx(1.75e-10, rel=0.01)
    taps = fsk.gfsk_taps(SPS, BT)
    assert len(taps) == 5 * SPS - 1
    # A run of ones: the filter's polyphase branches each sum to 1 (full deviation).
    for j in range(SPS):
        assert sum(taps[j::SPS]) == pytest.approx(1.0, abs=1e-9)


def test_modulator_deviation():
    # All ones: the phase turns at the deviation, 25 kHz.
    data = b"\xff" * 8
    n = fsk.mod_samples(64, SPS, len(TAPS))
    out = array.array("h", [0] * (2 * n))
    fsk.modulate(data, 64, SPS, TAPS, SENS, 0.5, out)
    k = 40 * SPS
    d = fsk.angle(out[2 * k] * out[2 * k - 2] + out[2 * k + 1] * out[2 * k - 1],
                  out[2 * k + 1] * out[2 * k - 2] - out[2 * k] * out[2 * k - 1])
    assert d / 65536 * FS == pytest.approx(25e3, rel=0.01)
    assert max(abs(v) for v in out) <= int(0.5 * 32767) + 1


@pytest.mark.parametrize("foff,snr", [(0, 30), (25e3, 20), (-40e3, 15), (50e3, 12), (30e3, 8),
                                      (-15e3, 6)])
def test_loopback(foff, snr):
    data = payload(int(foff) + snr)
    rx = fsk.Receiver(SPS, CODE_V, CODE_L, 8)
    frames = receive(rx, channel(burst(data), foff, snr, 3000, 400, seed=snr))
    assert len(frames) == 1
    f = frames[0]
    assert f.data == data
    assert f.offset_hz(FS, SPS) == pytest.approx(foff, abs=1500)
    if snr >= 12:
        assert f.errors == 0
        assert f.snr_db() == pytest.approx(snr, abs=2.5)
        # The code's 0101 preamble keeps well inside the deviation (the Gaussian filter).
        assert 0.6 * 25e3 < f.deviation_hz(FS, SPS) < 0.8 * 25e3


def test_bursts_back_to_back_and_chunks():
    # Three bursts with gaps of a few symbols, fed in odd chunks: the same frames.
    datas = [payload(s, 5 + 7 * s) for s in range(3)]
    iq = array.array("h")
    for d in datas:
        iq += burst(d)
        iq += array.array("h", [0] * 60)
    sig = channel(iq, 20e3, 20, 2000, 300, seed=7)
    for chunk in (1, 97, 4096):
        rx = fsk.Receiver(SPS, CODE_V, CODE_L, 8)
        assert [f.data for f in receive(rx, sig, chunk)] == datas


def test_rejects():
    rx = fsk.Receiver(SPS, CODE_V, CODE_L, 8)
    # Noise alone: nothing.
    assert receive(rx, channel(array.array("h"), 0, 0, 30000, 0, seed=3)) == []
    # A frame longer than max_len, and one whose lengths disagree: dropped.
    rx = fsk.Receiver(SPS, CODE_V, CODE_L, 8, max_len=16)
    assert receive(rx, channel(burst(payload(1, 17)), 0, 30, 2000, 300)) == []
    data = bytearray(fsk.header(4, CODE_V, CODE_L) + b"abcd")
    data[-5] ^= 1  # the second length
    n = fsk.mod_samples(8 * len(data), SPS, len(TAPS))
    out = array.array("h", [0] * (2 * n))
    fsk.modulate(bytes(data), 8 * len(data), SPS, TAPS, SENS, 0.5, out)
    rx = fsk.Receiver(SPS, CODE_V, CODE_L, 8)
    assert receive(rx, channel(out, 0, 30, 2000, 300)) == []


def test_receiver_settings():
    for bad in ((1, 8), (33, 8)):
        with pytest.raises(ValueError):
            fsk.Receiver(bad[0], CODE_V, CODE_L, bad[1])
    with pytest.raises(ValueError):
        fsk.Receiver(SPS, 5, 4, 0)
    with pytest.raises(ValueError):
        fsk.Receiver(SPS, CODE_V, CODE_L, 8, max_len=256)


# ---- the C modem against the reference ----


class CFrame(ctypes.Structure):
    _fields_ = [("len", ctypes.c_uint32), ("errors", ctypes.c_uint32), ("end", ctypes.c_uint32),
                ("phase", ctypes.c_uint32), ("dc", ctypes.c_double), ("amp", ctypes.c_double),
                ("var", ctypes.c_double), ("power", ctypes.c_double), ("floor", ctypes.c_uint32),
                ("data", ctypes.c_uint8 * 255)]


EMIT = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.POINTER(CFrame))


@pytest.fixture(scope="module")
def clib(tmp_path_factory):
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        pytest.skip("no host C compiler")
    out = tmp_path_factory.mktemp("fsk") / "libesdr_fsk.so"
    src = REPO / "apps/micropython/cmod/esdr_fsk.c"
    subprocess.run([cc, "-O2", "-Wall", "-Werror", "-shared", "-fPIC", "-o", str(out), str(src),
                    "-lm"], check=True)
    lib = ctypes.CDLL(str(out))
    lib.esdr_fsk_rx_size.restype = ctypes.c_size_t
    lib.esdr_fsk_rx_size.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
    lib.esdr_fsk_rx_init.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32,
                                     ctypes.c_uint64, ctypes.c_uint32, ctypes.c_uint32,
                                     ctypes.c_uint32]
    lib.esdr_fsk_rx_process.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, EMIT,
                                        ctypes.c_void_p]
    lib.esdr_fsk_angle.argtypes = [ctypes.c_int32, ctypes.c_int32]
    lib.esdr_fsk_angle.restype = ctypes.c_int32
    lib.esdr_fsk_mod.restype = ctypes.c_size_t
    lib.esdr_fsk_mod.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint32,
                                 ctypes.POINTER(ctypes.c_float), ctypes.c_size_t, ctypes.c_float,
                                 ctypes.c_float, ctypes.c_void_p, ctypes.c_size_t,
                                 ctypes.POINTER(ctypes.c_float)]
    return lib


class CReceiver:
    def __init__(self, lib, sps, code, code_len, threshold, max_len=255):
        self.lib = lib
        size = lib.esdr_fsk_rx_size(sps, code_len)
        self.mem = ctypes.create_string_buffer(size)
        assert lib.esdr_fsk_rx_init(self.mem, size, sps, code, code_len, threshold, max_len) == 0
        self.frames = []
        self.cb = EMIT(self._emit)

    def _emit(self, user, f):
        f = f.contents
        self.frames.append(fsk.Frame(bytes(f.data[:f.len]), f.errors, f.end, f.phase, f.dc,
                                     f.amp, f.var, f.power, f.floor))

    def process(self, iq, n, start=0):
        self.frames = []
        buf = (ctypes.c_int16 * (2 * n)).from_buffer_copy(
            array.array("h", iq[2 * start:2 * (start + n)]))
        self.lib.esdr_fsk_rx_process(self.mem, buf, n, self.cb, None)
        return self.frames


def test_c_angle(clib):
    rnd = random.Random(5)
    for _ in range(3000):
        re = rnd.randint(-2 ** 31 + 1, 2 ** 31 - 1)
        im = rnd.randint(-2 ** 31 + 1, 2 ** 31 - 1)
        if rnd.random() < 0.3:
            re >>= rnd.randint(0, 31)
            im >>= rnd.randint(0, 31)
        assert clib.esdr_fsk_angle(re, im) == fsk.angle(re, im), (re, im)


def test_c_receiver_matches(clib):
    datas = [payload(s, 3 + 11 * s) for s in range(4)]
    iq = array.array("h")
    for d in datas:
        iq += burst(d)
        iq += array.array("h", [0] * 500)
    for foff, snr, chunk in ((23e3, 25, 997), (-31e3, 7, 4096), (0, 3, 64)):
        sig = channel(iq, foff, snr, 2500, 300, seed=int(snr))
        py = receive(fsk.Receiver(SPS, CODE_V, CODE_L, 8), sig, chunk)
        c = receive(CReceiver(clib, SPS, CODE_V, CODE_L, 8), sig, chunk)
        assert len(c) == len(py)
        for a, b in zip(c, py):
            assert (a.data, a.errors, a.end, a.phase, a.floor) == \
                (b.data, b.errors, b.end, b.phase, b.floor)
            assert (a.dc, a.amp, a.power) == pytest.approx((b.dc, b.amp, b.power), rel=1e-9)
            assert a.var == pytest.approx(b.var, rel=1e-6)
        if snr > 20:
            assert [f.data for f in c] == datas


def test_c_modulator_matches(clib):
    data = fsk.header(9, CODE_V, CODE_L) + b"modulator"
    nbits = 8 * len(data)
    n = fsk.mod_samples(nbits, SPS, len(TAPS))
    py = array.array("h", [0] * (2 * n))
    _, ph_py = fsk.modulate(data, nbits, SPS, TAPS, SENS, 0.7, py, 0.25)
    out = (ctypes.c_int16 * (2 * n))()
    taps = (ctypes.c_float * len(TAPS))(*TAPS)
    ph = ctypes.c_float(0.25)
    assert clib.esdr_fsk_mod(data, nbits, SPS, taps, len(TAPS), SENS, 0.7, out, n - 1,
                             ctypes.byref(ph)) == 0
    assert clib.esdr_fsk_mod(data, nbits, SPS, taps, len(TAPS), SENS, 0.7, out, n,
                             ctypes.byref(ph)) == n
    assert max(abs(a - b) for a, b in zip(out, py)) <= 3
    assert ph.value == pytest.approx(ph_py, abs=1e-3)
