# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/micropython's Pcap app (eggs/pcap): its packet capture (cmod/esdr_pcap_core.c with
apps/pcap's decoders, built here with the host compiler) on synthetic streams, its dissector and
capture view (eggs/pcap/pcap_view.py: filters, groups, sorts, the table's text), and the app on
a fake radio whose esdr.pcap_* are that same C: packets in the table, the choosers, a gap, a
ring overrun, and Save's pcapng read back by the specification's reader."""

import ctypes
import gc
import importlib
import shutil
import struct
import subprocess
import sys
import time
import types

import numpy as np
import pytest

from conftest import REPO
from test_ble_pcap import VECS, gfsk
from test_esdr_pcap import read_pcapng

MODULES = REPO / "apps/micropython/modules"
EGG = REPO / "apps/micropython/eggs/pcap"
OWN = ("gnuradio", "pmt", "PyQt5", "sip", "packaging", "esdr", "framebuf", "pcap", "pcap_view",
       "dusk")
RATE = 2_000_000
ADV_AA = 0x8E89BED6

SOURCES = ["apps/micropython/cmod/esdr_pcap_core.c", "apps/pcap/src/dec_ble.c",
           "apps/pcap/src/dec_wpan.c", "apps/pcap/src/dec_generic.c", "apps/pcap/src/refine.c",
           "apps/pcap/src/records.c", "apps/pcap/src/pcapng.c", "lib/ieee802154/ieee154_phy.c",
           "apps/ubertooth/src/upstream/bluetooth_le.c", "apps/micropython/cmod/esdr_pcap_dissect.c"]
INCLUDES = ["include", "apps/pcap/src", "apps/ubertooth/src/upstream", "apps/micropython/cmod"]


class Cfg(ctypes.Structure):
    _fields_ = [("freq_hz", ctypes.c_uint64), ("rate_hz", ctypes.c_uint32),
                ("shift", ctypes.c_uint8), ("gain", ctypes.c_uint8), ("ratio", ctypes.c_float),
                ("decoders", ctypes.c_uint32), ("burst_max", ctypes.c_uint32),
                ("clock", ctypes.c_void_p), ("pool", ctypes.c_uint32)]


class CText(ctypes.Structure):
    _fields_ = [("len", ctypes.c_uint16), ("s", ctypes.c_char * 320)]


class CFields(ctypes.Structure):
    _fields_ = [("channel", CText), ("phy", CText), ("kind", CText), ("src", CText),
                ("dst", CText), ("info", CText), ("length", ctypes.c_uint32),
                ("ok", ctypes.c_int8), ("random", ctypes.c_uint8)]


class CPkt(ctypes.Structure):
    _fields_ = [("iface", ctypes.c_uint8), ("ok", ctypes.c_bool), ("power_x10", ctypes.c_int16),
                ("floor_x10", ctypes.c_int16), ("at", ctypes.c_int64),
                ("offset_hz", ctypes.c_int32), ("duration_us", ctypes.c_uint32),
                ("rec", ctypes.c_void_p), ("len", ctypes.c_uint16), ("comment", ctypes.c_char_p)]


STATS = ("samples", "gaps", "bursts", "blips", "truncated", "pool_full", "ble", "ble_bad", "wpan",
         "wpan_bad", "generic", "wide", "skipped", "lost")


CYCLES = ("cyc_ble", "cyc_wpan", "cyc_generic")


class CStats(ctypes.Structure):
    _fields_ = ([("samples", ctypes.c_uint64)] + [(k, ctypes.c_uint32) for k in STATS[1:]] +
                [(k, ctypes.c_uint64) for k in CYCLES])


# pcap_proto.h
DEC_DEFAULT = 0x0001 | 0x0002 | 0x0004 | 0x0008 | 0x0020 | 0x0040
DEC_BAD_CRC = 0x0010


@pytest.fixture(scope="session")
def core(tmp_path_factory):
    """The capture as the board runs it; skipped without a compiler."""
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        pytest.skip("no C compiler")
    out = tmp_path_factory.mktemp("pcap") / "libesdr_pcap.so"
    args = [cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC", "-o",
            str(out)]
    args += ["-I" + str(REPO / i) for i in INCLUDES] + [str(REPO / s) for s in SOURCES] + ["-lm"]
    subprocess.run(args, check=True)
    lib = ctypes.CDLL(str(out))
    lib.esdr_pcap_size.restype = ctypes.c_size_t
    lib.esdr_pcap_size.argtypes = [ctypes.POINTER(Cfg)]
    lib.esdr_pcap_init.restype = ctypes.c_void_p
    lib.esdr_pcap_init.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(Cfg)]
    lib.esdr_pcap_tune.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint8]
    lib.esdr_pcap_feed.restype = ctypes.c_uint32
    lib.esdr_pcap_feed.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32,
                                   ctypes.c_int64, ctypes.c_uint32]
    lib.esdr_pcap_flush.restype = ctypes.c_uint32
    lib.esdr_pcap_flush.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.esdr_pcap_detect.restype = None
    lib.esdr_pcap_detect.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32,
                                     ctypes.c_int64]
    lib.esdr_pcap_end.restype = None
    lib.esdr_pcap_end.argtypes = [ctypes.c_void_p]
    lib.esdr_pcap_decode.restype = ctypes.c_uint32
    lib.esdr_pcap_decode.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32]
    lib.esdr_pcap_pending.restype = ctypes.c_uint32
    lib.esdr_pcap_pending.argtypes = [ctypes.c_void_p]
    lib.esdr_pcap_next.restype = ctypes.c_bool
    lib.esdr_pcap_next.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(CPkt)]
    lib.esdr_pcap_get_stats.argtypes = [ctypes.c_void_p, ctypes.POINTER(CStats)]
    lib.esdr_pcap_head.restype = ctypes.c_int
    lib.esdr_pcap_head.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p,
                                   ctypes.c_char_p, ctypes.c_char_p]
    lib.esdr_pcap_epb.restype = ctypes.c_int
    lib.esdr_pcap_epb.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint32,
                                  ctypes.c_uint64, ctypes.c_char_p, ctypes.c_uint32,
                                  ctypes.c_char_p]
    lib.esdr_pcap_epb_size.restype = ctypes.c_size_t
    lib.esdr_pcap_epb_size.argtypes = [ctypes.c_uint32, ctypes.c_char_p]
    lib.esdr_pcap_dissect.restype = None
    lib.esdr_pcap_dissect.argtypes = [ctypes.c_uint32, ctypes.c_char_p, ctypes.c_size_t,
                                      ctypes.POINTER(CFields)]
    lib.ieee154_phy_modulate.restype = ctypes.c_size_t
    lib.ieee154_phy_modulate.argtypes = [ctypes.c_char_p, ctypes.c_uint8, ctypes.c_int16,
                                         ctypes.c_void_p, ctypes.c_size_t]
    lib.ieee154_phy_append_fcs.restype = ctypes.c_size_t
    lib.ieee154_phy_append_fcs.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    return lib


class Capture:
    """esdr.pcap_new()'s state through ctypes; feed() returns esdr.pcap_feed()'s tuples."""

    def __init__(self, lib, freq, rate=RATE, decoders=DEC_DEFAULT, shift=0, gain=255,
                 burst_max=0, pool=0):
        self.lib = lib
        cfg = Cfg(int(freq), rate, shift, gain, 4.0, decoders, burst_max, None, pool)
        size = lib.esdr_pcap_size(ctypes.byref(cfg))
        if size == 0:
            raise ValueError("pcap: rate 2 or 4 MS/s, shift 0..8")
        self.mem = ctypes.create_string_buffer(size + 16)
        self.st = lib.esdr_pcap_init(self.mem, size + 16, ctypes.byref(cfg))
        self.pool = pool
        assert self.st

    def _packets(self, n):
        out = []
        p = CPkt()
        for k in range(n):
            assert self.lib.esdr_pcap_next(self.st, k, ctypes.byref(p))
            out.append((p.iface, p.at, p.ok, p.power_x10, p.floor_x10, p.offset_hz,
                        p.duration_us, ctypes.string_at(p.rec, p.len),
                        p.comment.decode() if p.comment else None))
        assert not self.lib.esdr_pcap_next(self.st, n, ctypes.byref(p))
        return out

    def feed(self, iq, at, analyse=1):
        a = np.ascontiguousarray(iq, dtype=np.int16)
        n = self.lib.esdr_pcap_feed(self.st, a.ctypes.data, len(a) // 2, at, analyse)
        return self._packets(n)

    def flush(self, analyse=1):
        return self._packets(self.lib.esdr_pcap_flush(self.st, analyse))

    def detect(self, iq, at):
        a = np.ascontiguousarray(iq, dtype=np.int16)
        self.lib.esdr_pcap_detect(self.st, a.ctypes.data, len(a) // 2, at)

    def end(self):
        self.lib.esdr_pcap_end(self.st)

    def decode(self, most=64, analyse=1):
        return self._packets(self.lib.esdr_pcap_decode(self.st, most, analyse))

    def pending(self):
        return self.lib.esdr_pcap_pending(self.st)

    def tune(self, freq, gain=255):
        self.lib.esdr_pcap_tune(self.st, int(freq), gain)

    def stats(self):
        s = CStats()
        self.lib.esdr_pcap_get_stats(self.st, ctypes.byref(s))
        return {k: getattr(s, k) for k in STATS}


# ---- synthetic streams ----


def noise(n, rng, sigma):
    return sigma * (rng.standard_normal(n) + 1j * rng.standard_normal(n))


def to_iq(x, full=16384):
    a = np.empty(2 * len(x), np.int16)
    a[0::2] = np.clip(np.round(x.real * full), -32768, 32767)
    a[1::2] = np.clip(np.round(x.imag * full), -32768, 32767)
    return a


def ble_stream(airs, gap=3000, amp=0.25, snr_db=30, offset_hz=20e3, seed=1, sps=2, full=16384):
    """Noise, then each packet's on-air bytes at sps samples a symbol, gap samples apart."""
    rng = np.random.default_rng(seed)
    sigma = amp * 10 ** (-snr_db / 20) / np.sqrt(2)
    parts, starts, at = [noise(gap, rng, sigma)], [], gap
    for air in airs:
        s = amp * gfsk(air, sps)
        starts.append(at)
        parts += [s + noise(len(s), rng, sigma), noise(gap, rng, sigma)]
        at += len(s) + gap
    x = np.concatenate(parts)
    rate = sps * 1e6
    return to_iq(x * np.exp(2j * np.pi * offset_hz / rate * np.arange(len(x))), full), starts


def wpan_frame(lib, payload):
    buf = ctypes.create_string_buffer(bytes(payload) + bytes(2), len(payload) + 2)
    n = lib.ieee154_phy_append_fcs(buf, len(payload))
    return buf.raw[:n]


def wpan_stream(lib, psdus, gap=3000, amp=4000, snr_db=25, seed=2):
    """Frames modulated at 4 MS/s (the PHY's own modulator), averaged to 2 MS/s in pairs."""
    # The modulator reads the chip tables that ieee154_phy_init() builds.
    lib.ieee154_phy_init()
    rng = np.random.default_rng(seed)
    sigma = amp / 16384 * 10 ** (-snr_db / 20) / np.sqrt(2)
    parts = [noise(gap, rng, sigma)]
    for psdu in psdus:
        cap = (6 + len(psdu)) * 128 + 4
        out = (ctypes.c_int16 * (2 * cap))()
        n = lib.ieee154_phy_modulate(psdu, len(psdu), amp, out, cap)
        a = np.frombuffer(out, np.int16)[:2 * n].astype(float) / 16384
        x = a[0::2] + 1j * a[1::2]
        x = (x[0:len(x) // 2 * 2:2] + x[1:len(x) // 2 * 2:2]) / 2
        parts += [x + noise(len(x), rng, sigma), noise(gap, rng, sigma)]
    return to_iq(np.concatenate(parts))


def feed_chunks(cap, iq, sizes, at=0):
    """iq fed in chunks of the given sizes (cycled), the stream index continuing."""
    out, k, j = [], 0, 0
    n = len(iq) // 2
    while k < n:
        m = min(sizes[j % len(sizes)], n - k)
        out += cap.feed(iq[2 * k:2 * (k + m)], at + k)
        k += m
        j += 1
    return out + cap.flush()


V37 = [v for v in VECS if v["mhz"] == 2402]


# ---- the capture ----


def test_bad_settings(core):
    with pytest.raises(ValueError):
        Capture(core, 2402e6, rate=1_000_000)
    with pytest.raises(ValueError):
        Capture(core, 2402e6, shift=9)
    with pytest.raises(ValueError):
        Capture(core, 2402e6, burst_max=40000)
    with pytest.raises(ValueError):
        Capture(core, 2402e6, pool=1)
    with pytest.raises(ValueError):
        Capture(core, 2402e6, pool=129)


def test_detector_and_decoders_apart(core):
    """The detector closes bursts into the pool, the decoders take them later: the packets of
    esdr_pcap_feed(), in order; an open burst at the end goes out with esdr_pcap_end()."""
    airs = [v["air"] for v in V37] * 2
    iq, starts = ble_stream(airs)
    ref = [p for p in feed_chunks(Capture(core, 2402e6), iq, [4096]) if p[0] == 0]
    cap = Capture(core, 2402e6, pool=16)
    n = len(iq) // 2
    for k in range(0, n, 3000):
        cap.detect(iq[2 * k:2 * min(n, k + 3000)], k)
    cap.end()
    assert cap.pending() >= len(airs)
    got = [p for p in cap.decode(4) + cap.decode() if p[0] == 0]
    assert cap.pending() == 0
    assert [p[1] for p in got] == [p[1] for p in ref] and [p[7] for p in got] == [p[7] for p in ref]
    st = cap.stats()
    assert st["pool_full"] == 0 and st["ble"] == len(airs)


def test_bursts_no_decoder_takes_leave_the_pool(core):
    """Two threads: noise filling the channel (wideband records) goes to the decoders as its
    numbers alone, so a pool of 4 still holds the three advertisements after twelve of them."""
    rng = np.random.default_rng(11)
    parts, airs = [], [v["air"] for v in V37]
    for k in range(12):
        parts.append(noise(3000, rng, 0.003))
        parts.append(0.3 * (rng.standard_normal(600) + 1j * rng.standard_normal(600)))
    x = np.concatenate(parts)
    ble, _ = ble_stream(airs)
    iq = np.concatenate([to_iq(x), ble])
    cap = Capture(core, 2402e6, pool=4)
    cap.detect(iq, 0)
    cap.end()
    st = cap.stats()
    assert st["pool_full"] == 0
    got = cap.decode() + cap.decode()
    wide = [p for p in got if p[0] == 2 and struct.unpack_from("<BBHQiIIIIBBBBhhhHII", p[7])[9] == 8]
    assert len(wide) == 12
    assert len([p for p in got if p[0] == 0]) == len(airs)
    assert cap.pending() == 0


def test_a_full_pool_loses_bursts(core):
    """Nothing decodes: after the pool's buffers the detector has none and counts the rest;
    decoding frees them again."""
    airs = [v["air"] for v in V37] * 3
    iq, _ = ble_stream(airs)
    cap = Capture(core, 2402e6, pool=4)
    cap.detect(iq, 0)
    st = cap.stats()
    assert cap.pending() == 4 and st["pool_full"] == len(airs) - 4
    assert len([p for p in cap.decode() if p[0] == 0]) == 4
    iq2, _ = ble_stream(airs[:2], seed=9)
    cap.detect(iq2, 10_000_000)
    cap.end()
    assert len([p for p in cap.decode() if p[0] == 0]) == 2


@pytest.mark.parametrize("sizes", [[100000], [4096], [7, 1000, 333, 16, 5000]])
def test_ble_advertising_packets(core, sizes):
    """Advertisements on channel 37, fed in pieces of any size: each packet once, its record
    apps/pcap's (LINKTYPE 256 header, then access address, PDU and CRC dewhitened)."""
    iq, starts = ble_stream([v["air"] for v in V37])
    cap = Capture(core, 2402e6)
    pkts = [p for p in feed_chunks(cap, iq, sizes) if p[0] == 0]
    assert len(pkts) == len(V37)
    for p, v, s in zip(pkts, V37, starts):
        iface, at, ok, power, floor, offset, us, rec, comment = p
        assert ok and rec[0] == 0  # RF channel 0: 2402 MHz
        assert rec[10:10 + len(v["clear"])] == v["clear"]
        assert abs(at - s) <= 4
        assert offset == pytest.approx(20e3, abs=8e3)
        assert -150 < power < -100 and power - floor > 200
        assert us == (1 + 4 + len(v["pdu"]) + 3) * 8
        assert comment.startswith("esdr: 2402.000 MHz, ch 0/1 at 2.000 MS/s, LE 1M, offset +")
    st = cap.stats()
    assert st["ble"] == len(V37) and st["ble_bad"] == 0 and st["lost"] == 0
    assert st["samples"] == len(iq) // 2


def test_a_bad_crc_only_when_asked(core):
    v = V37[0]
    broken = bytearray(v["air"])
    broken[9] ^= 0x04
    iq, _ = ble_stream([bytes(broken)])
    assert [p for p in feed_chunks(Capture(core, 2402e6), iq, [4096]) if p[0] == 0] == []
    cap = Capture(core, 2402e6, decoders=DEC_DEFAULT | DEC_BAD_CRC)
    pkts = [p for p in feed_chunks(cap, iq, [4096]) if p[0] == 0]
    assert len(pkts) == 1 and not pkts[0][2]
    assert cap.stats()["ble_bad"] == 1


def test_connect_ind_then_its_connection(core):
    """A CONNECT_IND teaches the access address: its connection's data PDUs decode next."""
    conn = next(v for v in VECS if v["name"] == "connect_req_37")
    data = next(v for v in VECS if v["name"] == "data_empty_9")
    cap = Capture(core, 2402e6)
    iq, _ = ble_stream([conn["air"]])
    assert len([p for p in feed_chunks(cap, iq, [4096]) if p[0] == 0]) == 1
    # The data channel 9 at 2422 MHz, the same receiver retuned.
    cap.tune(2422e6)
    iq, _ = ble_stream([data["air"]], seed=3)
    pkts = [p for p in feed_chunks(cap, iq, [4096], at=10_000_000) if p[0] == 0]
    if struct.unpack_from("<I", conn["pdu"], 14)[0] != data["aa"]:
        pytest.skip("the golden CONNECT_IND announces another access address")
    assert len(pkts) == 1 and pkts[0][7][10:14] == struct.pack("<I", data["aa"])


def test_wpan_frames_at_2_mss(core):
    """802.15.4 frames on channel 15 (2425 MHz) from the 2 MS/s stream (apps/pcap's channel
    mode reads them at 4 MS/s with midpoints)."""
    frames = [wpan_frame(core, bytes([0x41, 0x88, k, 0xbd, 0xc2, 0xff, 0xff, 0xdc, 0x45]) +
                         bytes(range(k, k + 20))) for k in range(4)]
    iq = wpan_stream(core, frames)
    cap = Capture(core, 2425e6)
    pkts = [p for p in feed_chunks(cap, iq, [3000]) if p[0] == 1]
    assert len(pkts) == 4
    for p, f in zip(pkts, frames):
        rec = p[7]
        hlen = struct.unpack_from("<H", rec, 2)[0]
        assert p[2] and rec[hlen:] == f
        assert p[6] == (6 + len(f)) * 32
    assert cap.stats()["wpan"] == 4


def test_a_gap_ends_the_burst_open_there(core):
    v = V37[0]
    iq, starts = ble_stream([v["air"]])
    cap = Capture(core, 2402e6)
    mid = starts[0] + 100
    pkts = cap.feed(iq[:2 * mid], 0)
    # The rest arrives 5000 samples later on the stream's axis: the packet is cut.
    pkts += cap.feed(iq[2 * mid:], mid + 5000) + cap.flush()
    assert [p for p in pkts if p[0] == 0] == []
    st = cap.stats()
    assert st["gaps"] == 1 and st["bursts"] >= 1


def fsk_burst(n, rate_bd, dev_hz, seed=4):
    rng = np.random.default_rng(seed)
    sps = RATE / rate_bd
    bits = rng.integers(0, 2, int(n / sps) + 1)
    f = (2 * bits[(np.arange(n) / sps).astype(int)] - 1) * dev_hz
    return np.exp(2j * np.pi * np.cumsum(f) / RATE)


def test_other_bursts_become_records(core):
    """A 2FSK burst at 50 kBd is no packet: the generic analysis records its modulation and rate,
    and without a budget left it is recorded unanalysed."""
    rng = np.random.default_rng(5)
    x = np.concatenate([noise(3000, rng, 0.002), 0.2 * fsk_burst(4000, 50e3, 25e3),
                        noise(3000, rng, 0.002)])
    iq = to_iq(x)
    cap = Capture(core, 2402e6)
    recs = [p for p in cap.feed(iq, 0, analyse=1) + cap.flush() if p[0] == 2]
    assert len(recs) == 1
    h = struct.unpack_from("<BBHQiIIIIBBBBhhhHII", recs[0][7])
    assert h[0] == 1 and h[9] == 3  # version 1, 2FSK
    assert h[7] == pytest.approx(50e3, rel=0.1)
    cap = Capture(core, 2402e6)
    recs = [p for p in cap.feed(iq, 0, analyse=0) + cap.flush() if p[0] == 2]
    assert len(recs) == 1
    h = struct.unpack_from("<BBHQiIIIIBBBBhhhHII", recs[0][7])
    assert h[2] & 0x0200 and h[9] == 0  # PCAP_BF_SKIPPED, modulation unknown
    assert cap.stats()["skipped"] == 1


def test_eight_bit_items_scaled(core):
    """A stream of full scale 128 (the WHY2025 badge's C6) with shift 7 decodes as at 16384."""
    iq, _ = ble_stream([v["air"] for v in V37[:2]], amp=0.5, snr_db=35)
    small = np.round(iq.astype(float) / 128).astype(np.int16)
    cap = Capture(core, 2402e6, shift=7)
    pkts = [p for p in feed_chunks(cap, small, [2048]) if p[0] == 0]
    assert len(pkts) == 2 and all(p[2] for p in pkts)


@pytest.mark.parametrize("shift", [1, 3])
def test_shift_as_scaled_samples(core, shift):
    """The detector shifts a word at a time: the same packets, numbers and records as samples
    scaled before the call, each half wrapping as an int16 would (the top bits of I must not
    reach Q)."""
    iq, _ = ble_stream([v["air"] for v in V37], amp=0.4, snr_db=30)
    small = (iq.astype(np.int32) >> shift).astype(np.int16)
    # Values whose shift overflows an int16: a burst of them, wrapped the same both ways.
    loud = np.full(400, -(1 << (15 - shift)) - 3, np.int16)
    small = np.concatenate([small[:20000], loud, small[20000:]])
    wide = (small.astype(np.int32) << shift).astype(np.int16)
    a = Capture(core, 2402e6, shift=shift)
    b = Capture(core, 2402e6)
    pa = feed_chunks(a, small, [999, 4096, 17])
    pb = feed_chunks(b, wide, [999, 4096, 17])
    assert pa == pb and len([p for p in pa if p[0] == 0]) >= 2
    sa, sb = a.stats(), b.stats()
    assert {k: v for k, v in sa.items() if not k.startswith("cyc")} == \
        {k: v for k, v in sb.items() if not k.startswith("cyc")}


def test_pcapng_blocks(core):
    buf = ctypes.create_string_buffer(2048)
    n = core.esdr_pcap_head(buf, 2048, b"board", b"Zephyr", b"app")
    head = buf.raw[:n]
    rec = bytes(range(37))
    size = core.esdr_pcap_epb_size(len(rec), b"hello")
    m = core.esdr_pcap_epb(buf, 2048, 1, 123456789012, rec, len(rec), b"hello")
    assert m == size
    blocks = read_pcapng(head + buf.raw[:m])
    assert [b["type"] for b in blocks] == ["SHB", "IDB", "IDB", "IDB", "EPB"]
    assert [b["linktype"] for b in blocks[1:4]] == [256, 283, 147]
    assert blocks[4]["interface"] == 1 and blocks[4]["timestamp"] == 123456789012
    assert blocks[4]["data"] == rec
    assert core.esdr_pcap_epb(buf, 2048, 3, 0, rec, len(rec), None) < 0
    assert core.esdr_pcap_head(buf, 64, b"board", b"Zephyr", b"app") < 0


# ---- the dissector and the view ----


@pytest.fixture
def pv(monkeypatch):
    monkeypatch.syspath_prepend(str(EGG))
    sys.modules.pop("pcap_view", None)
    return importlib.import_module("pcap_view")


def ble_rec(pdu, aa=ADV_AA, rf=0, flags=0x0C31):
    return struct.pack("<BbbBIH", rf, 0, 0, 0, aa, flags) + struct.pack("<I", aa) + pdu + bytes(3)


def adv(t, addr, data=b"", rnd=True):
    body = bytes(reversed(addr)) + data
    return bytes([t | (0x40 if rnd else 0), len(body)]) + body


def tap_rec(psdu, channel=15):
    tlvs = struct.pack("<HHI", 0, 1, 1) + struct.pack("<HHHBx", 3, 3, channel, 0)
    return struct.pack("<BBH", 0, 0, 4 + len(tlvs)) + tlvs + psdu


def raw(iface, rec, ok=True, power=-400, at=0, us=376):
    return (iface, at, ok, power, -800, 1200, us, rec, None)


ADDR = bytes([0xc4, 0x7c, 0x8d, 0x6a, 0x12, 0x34])


def test_ble_advertisements(pv):
    name = bytes([2, 0x01, 0x06, 6, 0x09]) + b"Plant" + bytes([5, 0xFF, 0x4C, 0x00, 1, 2])
    p = pv.Pkt(1, 0.5, 2402e6, raw(0, ble_rec(adv(0, ADDR, name))))
    assert (p.kind, p.src, p.channel, p.phy, p.ok) == ("ADV_IND", "c4:7c:8d:6a:12:34", "37",
                                                       "LE 1M", True)
    assert p.random
    assert p.info == '"Plant" Apple' and p.length == 2 + 6 + len(name)
    req = adv(3, ADDR, bytes(reversed(bytes(range(6)))))
    p = pv.Pkt(2, 0.6, 2402e6, raw(0, ble_rec(req, rf=12)))
    assert (p.kind, p.src, p.dst, p.channel) == ("SCAN_REQ", "c4:7c:8d:6a:12:34",
                                                 "00:01:02:03:04:05", "38")
    # A CRC the decoder found bad (kept with PCAP_DEC_BAD_CRC).
    p = pv.Pkt(3, 0.7, 2402e6, raw(0, ble_rec(adv(2, ADDR), flags=0x0431), ok=False))
    assert p.kind == "ADV_NONCONN" and not p.ok


def test_ble_golden_records(pv, core):
    """The decoder's own records of the golden packets read back."""
    iq, _ = ble_stream([v["air"] for v in V37])
    cap = Capture(core, 2402e6)
    pkts = [pv.Pkt(k, 0.0, 2402e6, r) for k, r in enumerate(feed_chunks(cap, iq, [8192]))]
    assert [p.kind for p in pkts] == ["ADV_IND", "SCAN_RSP", "CONNECT_IND"]
    assert pkts[2].info.startswith("AA ") and pkts[2].info.endswith(" ms")
    assert all(p.src.count(":") == 5 for p in pkts)


def test_ble_data_and_extended(pv):
    p = pv.Pkt(1, 0, 2422e6, raw(0, ble_rec(bytes([0x03, 0x01, 0x02]), aa=0x332211d6, rf=10)))
    assert (p.kind, p.src, p.channel) == ("LL_TERMINATE", "AA 332211d6", "9")
    p = pv.Pkt(1, 0, 2422e6, raw(0, ble_rec(bytes([0x01, 0x00]), aa=0x332211d6, rf=10)))
    assert p.kind == "Empty" and p.info == "SN 0 NESN 0"
    # ADV_EXT_IND with an address and an auxiliary pointer to channel 5, LE 2M.
    ext = bytes([10, 0x11]) + bytes(reversed(ADDR)) + bytes([5, 0, 1 << 5])
    p = pv.Pkt(1, 0, 2402e6, raw(0, ble_rec(bytes([0x07, len(ext)]) + ext)))
    assert (p.kind, p.src, p.info) == ("ADV_EXT", "c4:7c:8d:6a:12:34", "aux ch 5 2M")


def test_wpan_frames(pv):
    data = bytes([0x61, 0x88, 7, 0xbd, 0xc2, 0x00, 0x00, 0xdc, 0x45, 1, 2, 0, 0])
    p = pv.Pkt(1, 0, 2425e6, raw(1, tap_rec(data)))
    assert (p.kind, p.src, p.dst, p.channel, p.info) == ("Data", "c2bd:45dc", "c2bd:0000",
                                                         "15", "#7 ack?")
    ack = bytes([0x02, 0x00, 9, 0, 0])
    p = pv.Pkt(2, 0, 2425e6, raw(1, tap_rec(ack)))
    assert (p.kind, p.src, p.dst, p.info, p.length) == ("Ack", "", "", "#9", 5)
    req = bytes([0x63, 0xc8, 3, 0x36, 0x58, 0x00, 0x00]) + bytes(range(8)) + bytes([0x04, 0, 0])
    p = pv.Pkt(3, 0, 2425e6, raw(1, tap_rec(req)))
    assert (p.kind, p.src, p.dst) == ("DataReq", "5836:0706050403020100", "5836:0000")
    p = pv.Pkt(4, 0, 2425e6, raw(1, tap_rec(bytes([0x00, 0x80, 1, 0x36, 0x58, 0xdc, 0x45]))))
    assert (p.kind, p.src) == ("Beacon", "5836:45dc")


def test_burst_records_and_malformed(pv):
    hdr = struct.pack("<BBHQiIIIIBBBBhhhHII", 1, 52, 0x0040, 2402000000, 1000, 120000, 800,
                      50000, 25000, 3, 255, 0, 1, -300, -700, -280, 0, 2000000, 40)
    p = pv.Pkt(1, 0, 2402e6, raw(2, hdr + bytes(5)))
    assert (p.kind, p.src, p.length) == ("2FSK", "2FSK 50kBd", 5)
    assert p.info == "50 kBd bw 120 kHz dev 25 kHz 40 bits"
    p = pv.Pkt(2, 0, 2402e6, raw(2, hdr[:20]))
    assert p.kind == "malformed"


def packets(pv):
    out = []
    srcs = [ADDR, bytes(range(6)), ADDR, ADDR, bytes(range(6))]
    for k, (t, a, pw) in enumerate(zip((0, 4, 0, 2, 0), srcs, (-500, -300, -450, -700, -350))):
        out.append(pv.Pkt(k + 1, 0.1 * k, 2402e6, raw(0, ble_rec(adv(t, a)), power=pw,
                                                       us=100 + 10 * k)))
    out.append(pv.Pkt(6, 0.6, 2425e6, raw(1, tap_rec(bytes([0x02, 0x00, 9, 0, 0])), power=-200,
                                          us=352)))
    out.append(pv.Pkt(7, 0.7, 2425e6, raw(1, tap_rec(bytes([0x02, 0x00, 9, 0, 0])), ok=False)))
    hdr = struct.pack("<BBHQiIIIIBBBBhhhHII", 1, 52, 0, 2402000000, 0, 0, 800, 0, 0, 8, 255, 0,
                      1, -300, -700, -280, 0, 2000000, 0)
    out.append(pv.Pkt(8, 0.8, 2402e6, raw(2, hdr, power=-100, us=900)))
    return out


def c_fields(core, iface, rec):
    """esdr_pcap_dissect()'s fields as the Python dissector names them."""
    f = CFields()
    core.esdr_pcap_dissect(iface, rec, len(rec), ctypes.byref(f))

    def text(t):
        return bytes(t.s[:t.len]).decode("utf-8")

    return {"channel": text(f.channel), "phy": text(f.phy), "kind": text(f.kind),
            "src": text(f.src), "dst": text(f.dst), "info": text(f.info), "length": f.length,
            "random": bool(f.random), "ok": None if f.ok < 0 else bool(f.ok)}


def py_fields(pv, iface, rec):
    p = pv.Pkt(1, 0.0, 2402e6, raw(iface, rec, ok="?"))
    p._dissect()
    return {"channel": p.channel, "phy": p.phy, "kind": p.kind, "src": p.src, "dst": p.dst,
            "info": p.info, "length": p.length, "random": p.random,
            "ok": None if p.ok == "?" else p.ok}


def dissector_records(pv):
    """Every kind the tests above build, and each cut short at every length."""
    name = bytes([2, 0x01, 0x06, 6, 0x09]) + b"Plant" + bytes([5, 0xFF, 0x4C, 0x00, 1, 2])
    recs = [(0, ble_rec(adv(0, ADDR, name))), (0, ble_rec(adv(3, ADDR, bytes(range(6))), rf=12)),
            (0, ble_rec(adv(2, ADDR), flags=0x0431)), (0, ble_rec(adv(2, ADDR), flags=0x0C31)),
            (0, ble_rec(adv(0, ADDR, bytes([4, 0x09, 0xff, 0xfe, 0x41]) +
                            bytes([3, 0xFF, 0x34, 0x12])))),
            (0, ble_rec(adv(0, ADDR, bytes([5, 0x08]) + "Rü".encode() + b"x"))),
            (0, ble_rec(adv(1, ADDR, bytes(range(6))), rf=39)),
            (0, ble_rec(bytes([0x03, 0x01, 0x02]), aa=0x332211d6, rf=10)),
            (0, ble_rec(bytes([0x03, 0x01, 0x30]), aa=0x332211d6, rf=20)),
            (0, ble_rec(bytes([0x01, 0x00]), aa=0x332211d6, rf=10)),
            (0, ble_rec(bytes([0x1e, 0x05]) + bytes(5), aa=0x12345678)),
            (0, ble_rec(bytes([0x07, 11, 10, 0x11]) + bytes(reversed(ADDR)) + bytes([5, 0, 1 << 5]))),
            (0, ble_rec(bytes([0x07, 9, 0x02, 0x13, 1, 2, 3, 4, 5, 6, 7]))),
            (0, ble_rec(bytes([0x0E, 2, 0, 0]))),
            (1, tap_rec(bytes([0x61, 0x88, 7, 0xbd, 0xc2, 0x00, 0x00, 0xdc, 0x45, 1, 2, 0, 0]))),
            (1, tap_rec(bytes([0x02, 0x00, 9, 0, 0]))),
            (1, tap_rec(bytes([0x63, 0xc8, 3, 0x36, 0x58, 0x00, 0x00]) + bytes(range(8)) +
                        bytes([0x04, 0, 0]))),
            (1, tap_rec(bytes([0x00, 0x80, 1, 0x36, 0x58, 0xdc, 0x45]), channel=26)),
            (1, tap_rec(bytes([0x63, 0x20, 3, 0x36, 0x58, 0xdc, 0x45, 0x09, 0, 0, 0]))),
            (1, tap_rec(bytes([0x41, 0xee, 3, 0x36, 0x58]) + bytes(range(16)) + bytes(4))),
            (1, tap_rec(bytes([0x49, 0x2c, 3, 0x36, 0x58, 1, 2, 0, 0]))),
            (1, tap_rec(bytes([0x29, 0x18, 3]) + bytes(4)))]
    for mod, flags, rate, bw, dev, nbits in ((3, 0x0040, 50000, 120000, 25000, 40),
                                             (8, 0, 0, 0, 0, 0), (2, 0x0200, 999, 1500, 0, 7),
                                             (12, 0, 2500000, 12345678, 3, 1 << 20),
                                             (5, 0x0240, 1234567, 999999, 1000, 9),
                                             (3, 0, 1235000, 12345, 2005, 1),
                                             (3, 0, 1225000, 1005, 99995, 2)):
        hdr = struct.pack("<BBHQiIIIIBBBBhhhHII", 1, 52, flags, 2402000000, -1000, bw, 800,
                          rate, dev, mod, 255, 0, 1, -300, -700, -280, 0, 2000000, nbits)
        recs.append((2, hdr + bytes(5)))
    recs.append((3, bytes(60)))
    cut = []
    for iface, rec in recs:
        cut += [(iface, rec[:n]) for n in range(len(rec))]
    return recs + cut


def test_dissector_in_c(pv, core):
    """esdr_pcap_dissect() (the board's) gives the Python dissector's fields: for every record
    above, every record cut short, and random bytes behind each interface's header."""
    rng = np.random.default_rng(7)
    recs = dissector_records(pv)
    for _ in range(3000):
        iface = int(rng.integers(0, 3))
        n = int(rng.integers(0, 90))
        body = bytes(rng.integers(0, 256, n, dtype=np.uint8))
        if iface == 0 and rng.random() < 0.5:
            body = ble_rec(body[:2] + body[2:2 + (body[1] if len(body) > 1 else 0)],
                           aa=ADV_AA if rng.random() < 0.7 else int(rng.integers(0, 1 << 32)),
                           rf=int(rng.integers(0, 40)), flags=int(rng.integers(0, 1 << 16)))
        elif iface == 1 and rng.random() < 0.7:
            body = tap_rec(body, channel=int(rng.integers(11, 27)))
        recs.append((iface, body))
    for iface, rec in recs:
        assert c_fields(core, iface, rec) == py_fields(pv, iface, rec), (iface, rec.hex())


def test_filters(pv):
    ps = packets(pv)
    no = lambda f, src=None: [p.no for p in pv.view(ps, f, "None", "Oldest", src)]  # noqa: E731
    assert no("Decoded") == [1, 2, 3, 4, 5, 6]
    assert no("All") == [1, 2, 3, 4, 5, 6, 7, 8]
    assert no("BLE") == [1, 2, 3, 4, 5]
    assert no("802.15.4") == [6, 7]
    assert no("Bursts") == [8]
    assert no("Bad CRC") == [7]
    assert no("Source", "c4:7c:8d:6a:12:34") == [1, 3, 4]
    assert no("Source") == no("All")


def test_sorts(pv):
    ps = packets(pv)
    no = lambda s: [p.no for p in pv.view(ps, "All", "None", s)]  # noqa: E731
    assert no("Newest") == [8, 7, 6, 5, 4, 3, 2, 1]
    assert no("Oldest") == [1, 2, 3, 4, 5, 6, 7, 8]
    assert no("Loudest")[:3] == [8, 6, 2]
    assert no("Longest")[:3] == [8, 7, 6]
    # Most packets from the same source first, then the newest of each.
    assert no("Count")[:3] == [4, 3, 1]
    assert no("Source")[:3] == [5, 2, 8]
    assert no("Type") == [7, 6, 5, 3, 1, 4, 2, 8]


def test_groups(pv):
    ps = packets(pv)
    gs = pv.view(ps, "All", "Source", "Count")
    # Equal counts: the newest first (the acknowledgements have no source).
    assert [(g.key, g.count) for g in gs] == [("c4:7c:8d:6a:12:34", 3), ("-", 2),
                                              ("00:01:02:03:04:05", 2), ("Wide", 1)]
    g = gs[0]
    assert (g.first, g.last, g.power, g.kind()) == (0.0, 0.30000000000000004, -45.0,
                                                    "BLE ADV_IND")
    assert pv.row_source(g) == "c4:7c:8d:6a:12:34"
    types = pv.view(ps, "All", "Type", "Count")
    assert [(g.key, g.count) for g in types][:2] == [("BLE ADV_IND", 3), ("154 Ack", 2)]
    protos = pv.view(ps, "All", "Protocol", "Newest")
    assert [g.key for g in protos] == ["Burst", "802.15.4", "BLE"]
    chans = pv.view(ps, "All", "Channel", "Loudest")
    assert [g.key for g in chans] == ["Bst -", "154 15", "BLE 37"]
    assert [g.key for g in pv.view(ps, "All", "Source", "Source")] == [
        "00:01:02:03:04:05", "Wide", "c4:7c:8d:6a:12:34", "-"]
    assert [g.key for g in pv.view(ps, "Decoded", "Protocol", "Oldest")] == ["BLE", "802.15.4"]


@pytest.mark.parametrize("chars", [38, 78, 98])
def test_lines_fit(pv, chars):
    ps = packets(pv)
    for grouped, rows in ((False, ps), (True, pv.view(ps, "All", "Source", "Count"))):
        cols = pv.columns(chars, grouped)
        assert len(pv.header(cols)) == chars
        for r in rows:
            assert len(pv.line(r, cols)) == chars
        titles = [c[0] for c in cols]
        if chars < 46:
            # The protocol goes first on a narrow line: the colour tells it.
            assert titles == (["Count", "Last", "dB", "Key", "Type"] if grouped else
                              ["Time", "dB", "Source", "Type"])
        else:
            assert titles[:4] == (["Count", "Last", "dB", "Pro"] if grouped else
                                  ["Time", "dB", "Pro", "Source"])
            assert titles[-1] == "Info"
    narrow = pv.columns(38, False)
    # The address keeps its end in a narrow source column, the numbers their right edge.
    assert pv.header(narrow).startswith("   Time   dB Source")
    line = pv.line(ps[0], narrow)
    assert line.startswith("   0.00  -50 ") and "6a:12:34" in line and "ADV_IND" in line


def test_detail(pv):
    ps = packets(pv)
    d = pv.detail(ps[0], 38, 3)
    assert d == ["#1 0.000 s BLE LE 1M ch 37 -50 dBFS",
                 "snr 30 dB +1.2 kHz 100 us",
                 "ADV_IND from c4:7c:8d:6a:12:34 random"]
    d = pv.detail(ps[0], 38, 2)
    assert d[1].startswith("ADV_IND from") and len(d) == 2
    d = pv.detail(pv.view(ps, "All", "Source", "Count")[0], 78, 2)
    assert d[0].startswith("c4:7c:8d:6a:12:34: 3 packets")
    assert pv.detail(None, 38) == []


# ---- the app on a fake radio ----


class FakeRadio(types.ModuleType):
    """esdr as the Pcap app uses it: a ring the test fills, pcap_* through the C above."""

    RATES = (2000000, 1000000)
    FULL_SCALE = 16384

    def __init__(self, lib, stream, rate=RATE, engine=False):
        super().__init__("esdr")
        self.rate = rate
        self.made = []
        # The S3's engine: the detector runs as the radio produces (its thread), the decoders
        # on theirs into a ring of packets that pcap_take() collects.
        self.engine = None
        self.pkt_ring = []
        if engine:
            self.pcap_start = self._pcap_start
            self.pcap_stop = self._pcap_stop
        self.lib = lib
        self.stream = stream
        self.pos = 0
        self.total = 0
        self.ring = None
        self.running = False
        self.calls = []
        self.gaps = 0
        self.claimed = []
        self.PCAP_DEC_DEFAULT = DEC_DEFAULT

    def init(self):
        pass

    def channel_bw(self, mode):
        pass

    def rx_bandwidth(self, hz):
        pass

    def tune(self, hz, offset=True):
        self.calls.append(("tune", hz))
        return hz

    def rx_gain(self, g=None):
        self.calls.append(("gain", g))

    def rx_start(self, rate, buf, count=0, wrap=False, **kw):
        assert rate == self.rate and wrap and count == 0
        self.ring = buf
        self.total = 0
        self.running = True
        self.calls.append(("start",))

    def rx_stop(self):
        self.running = False
        self.calls.append(("stop",))

    def rx_count(self):
        return self.total

    def rx_stats(self):
        return {"gaps": self.gaps}

    def produce(self, n):
        """n more samples of the stream into the ring (zeros after its end), or through the
        engine's detector."""
        if self.engine is not None:
            src = np.zeros(2 * n, np.int16)
            part = self.stream[2 * self.pos:2 * (self.pos + n)]
            src[:len(part)] = part
            self.engine.detect(src, self.pos)
            self.pos += n
            return
        if self.ring is None:
            return
        cap = len(self.ring) // 4
        ring = np.frombuffer(self.ring, np.int16)
        src = np.zeros(2 * n, np.int16)
        part = self.stream[2 * self.pos:2 * (self.pos + n)]
        src[:len(part)] = part
        k = 0
        while k < n:
            i = (self.total + k) % cap
            m = min(n - k, cap - i)
            ring[2 * i:2 * (i + m)] = src[2 * k:2 * (k + m)]
            k += m
        self.pos += n
        self.total += n

    def pcap_new(self, freq, rate=RATE, decoders=DEC_DEFAULT, ratio=4.0, gain=255, shift=0,
                 pool=0):
        self.made.append((rate, shift))
        return Capture(self.lib, freq, rate, decoders, shift, gain, pool=pool)

    def _pcap_start(self, st):
        assert self.engine is None
        self.calls.append(("pcap_start",))
        self.engine = st

    def _pcap_stop(self, st):
        assert self.engine is st
        self.calls.append(("pcap_stop",))
        self.engine = None
        st.end()

    def pcap_take(self, st, most=64, analyse=2):
        if self.engine is st:
            # The decoder thread has kept up: each closed burst decoded on its own, analysed
            # while under a quarter of the pool waits.
            while st.pending():
                self.pkt_ring += st.decode(1, 1 if 4 * st.pending() < st.pool else 0)
            out, self.pkt_ring = self.pkt_ring[:most], self.pkt_ring[most:]
            return out
        return st.decode(most, analyse)

    def pcap_pending(self, st):
        return st.pending()

    def pcap_tune(self, st, freq, gain=255):
        st.tune(freq, gain)

    def pcap_feed(self, st, buf, start, n, at, analyse=1):
        a = np.frombuffer(buf, np.int16)[2 * start:2 * (start + n)]
        return st.feed(a, at, analyse)

    def pcap_flush(self, st, analyse=1):
        assert self.engine is None
        out, self.pkt_ring = self.pkt_ring, []
        return out + st.flush(analyse)

    def pcap_stats(self, st):
        return dict(st.stats(), calls=1, busy_us=1000)

    def pcapng_head(self, hw, os, appl):
        buf = ctypes.create_string_buffer(4096)
        n = self.lib.esdr_pcap_head(buf, 4096, hw.encode(), os.encode(), appl.encode())
        return buf.raw[:n]

    def pcapng_epb(self, iface, t_ns, rec, comment=None):
        c = comment.encode() if comment else None
        size = self.lib.esdr_pcap_epb_size(len(rec), c)
        buf = ctypes.create_string_buffer(size)
        n = self.lib.esdr_pcap_epb(buf, size, iface, t_ns, rec, len(rec), c)
        return buf.raw[:n]

    def claim(self, vol):
        self.claimed.append(("claim", vol))

    def release(self, vol):
        self.claimed.append(("release", vol))


class FakeFB:
    """The table's framebuffer: what is written where."""

    def __init__(self):
        self.texts = {}

    def fill_rect(self, x, y, w, h, c):
        for k in [k for k in self.texts if y <= k < y + h]:
            del self.texts[k]

    def hline(self, x, y, w, c):
        pass

    def text(self, s, x, y, c):
        self.texts[y] = s


@pytest.fixture
def app(monkeypatch, core, tmp_path):
    def make(stream, rate=RATE, engine=False, display=None):
        radio = FakeRadio(core, stream, rate, engine)
        if display is not None:
            radio.display_size = lambda: display
        saved = {k: v for k, v in sys.modules.items() if k.split(".")[0] in OWN}
        for k in saved:
            del sys.modules[k]
        made.append(saved)
        framebuf = types.ModuleType("framebuf")
        framebuf.RGB565 = 1
        framebuf.FrameBuffer = object
        monkeypatch.setitem(sys.modules, "esdr", radio)
        monkeypatch.setitem(sys.modules, "framebuf", framebuf)
        clock = [1000]
        # MicroPython's gc.mem_free(): the ring takes a share of what is free.
        monkeypatch.setattr(gc, "mem_free", lambda: 8 << 20, raising=False)
        monkeypatch.setattr(time, "ticks_ms", lambda: clock[0], raising=False)
        monkeypatch.setattr(time, "ticks_add", lambda a, b: a + b, raising=False)
        monkeypatch.setattr(time, "ticks_diff", lambda a, b: a - b, raising=False)
        monkeypatch.syspath_prepend(str(EGG))
        monkeypatch.syspath_prepend(str(MODULES))
        gui = importlib.import_module("gnuradio._gui")
        gui._headless = True
        mod = importlib.import_module("pcap")
        blocks = importlib.import_module("gnuradio.blocks")
        sd = tmp_path / "SD"
        (sd / "pcap").mkdir(parents=True, exist_ok=True)

        def board_path(path):
            return path

        monkeypatch.setattr(blocks, "board_path", board_path)
        monkeypatch.setattr(mod, "open", lambda p, m: open(str(sd) + p[len("/SD:"):], m),
                            raising=False)
        return types.SimpleNamespace(radio=radio, gui=gui, mod=mod, clock=clock, sd=sd)

    made = []
    yield make
    for saved in made:
        for k in [k for k in sys.modules if k.split(".")[0] in OWN]:
            del sys.modules[k]
        sys.modules.update(saved)


def passes(a, n, samples=20000):
    """Passes of the GUI loop 10 ms apart, the radio writing samples before each."""
    for _ in range(n):
        a.clock[0] += 10
        a.radio.produce(samples)
        for fn in a.gui._pollers[:]:
            fn()


def table_text(table, w=320, h=200):
    table._fbo = FakeFB()
    table._w, table._h = w, h
    table._shown = None
    table._draw()
    return [table._fbo.texts[y] for y in sorted(table._fbo.texts)]


def test_the_app_shows_and_saves(app, core, monkeypatch, capsys):
    iq, _ = ble_stream([v["air"] for v in V37] * 3)
    a = app(iq)
    seen = {}

    def run():
        table = a.gui._sinks[0]
        view = table.view
        passes(a, 40)
        rows = view.rows()
        seen["rows"] = [(p.kind, p.no) for p in rows]
        seen["text"] = table_text(table)
        bars = [v for v in a.gui._vars if type(v).__name__ == "QToolBar"]
        boxes = [w for b in bars for w in b._widgets if type(w).__name__ == "QComboBox"]
        seen["boxes"] = [b._items[:3] for b in boxes]
        boxes[2]._picked(1)  # Group: Source
        boxes[3]._picked(4)  # Sort: Count
        a.clock[0] += 1000
        seen["groups"] = [(g.key, g.count) for g in view.rows()]
        boxes[0]._picked(3)  # Ch: 802.15.4 channel 11
        seen["tuned"] = a.radio.calls[-2:]
        button = [w for b in bars for w in b._widgets if type(w).__name__ == "QPushButton"][0]
        button.clicked.emit()
        seen["note"] = table.note
        return 0

    monkeypatch.setattr(a.gui, "run", run)
    a.mod.main()
    assert [k for k, _ in seen["rows"]] == ["CONNECT_IND", "SCAN_RSP", "ADV_IND"] * 3
    assert [n for _, n in seen["rows"]] == list(range(9, 0, -1))
    text = seen["text"]
    assert text[0].startswith("9/9 ")
    assert text[1].startswith("   Time   dB Source")
    assert "CONNECT_IND" in text[2] and len(text[2]) <= 40
    assert text[-3].startswith("#9 ")
    assert seen["boxes"][0] == ["BLE 37", "BLE 38", "BLE 39"]
    assert seen["boxes"][1] == ["Decoded", "All", "BLE"]
    # CONNECT_IND comes from the initiator, the rest from the advertiser.
    assert seen["groups"] == [("c6:55:44:33:22:11", 6), ("d7:aa:99:88:77:66", 3)]
    assert seen["tuned"] == [("gain", -1), ("start",)]
    assert ("tune", 2405e6) in a.radio.calls
    assert seen["note"] == "saved esdr-000.pcapng"
    assert a.radio.claimed == [("claim", "/SD:"), ("release", "/SD:")]
    blocks = read_pcapng((a.sd / "pcap/esdr-000.pcapng").read_bytes())
    assert [b["type"] for b in blocks] == ["SHB", "IDB", "IDB", "IDB"] + ["EPB"] * 9
    assert all(b["interface"] == 0 for b in blocks[4:])
    assert blocks[4]["data"][10:14] == struct.pack("<I", ADV_AA)
    assert blocks[5]["timestamp"] > blocks[4]["timestamp"]
    assert a.radio.running is False


def test_overrun_and_radio_gaps(app, monkeypatch):
    iq, _ = ble_stream([v["air"] for v in V37] * 4, gap=20000)
    a = app(iq)
    seen = {}

    def run():
        cap = a.gui._sinks[0].view.cap
        passes(a, 2)
        # The loop stalls: more than the ring arrives before the next pass.
        a.radio.produce(len(a.radio.ring) // 4 + 1000)
        passes(a, 1)
        seen["overruns"] = cap.overruns
        a.radio.gaps += 1
        passes(a, 30)
        seen["gaps"] = esdr_stats(a, cap)["gaps"]
        seen["n"] = len(cap.pkts)
        return 0

    monkeypatch.setattr(a.gui, "run", run)
    a.mod.main()
    assert seen["overruns"] == 1
    assert seen["gaps"] == 2
    assert 0 < seen["n"] < 12


def esdr_stats(a, cap):
    return a.radio.pcap_stats(cap.state)


@pytest.mark.parametrize("packet_rates", [(4000000, 2000000), (4000000,)])
def test_the_s3_takes_its_packet_rate(app, monkeypatch, packet_rates):
    """No 2 MS/s among the S3's filtered rates: its packet rate, 2 MS/s where it has that, at
    full scale 8192."""
    rate = min(packet_rates)
    iq, _ = ble_stream([v["air"] for v in V37], sps=rate // 1_000_000, full=8192)
    a = app(iq, rate=rate)
    a.radio.RATES = (500000, 250000)
    a.radio.PACKET_RATES = packet_rates
    a.radio.PACKET_FULL_SCALE = 8192
    seen = {}

    def run():
        passes(a, 10, samples=40000)
        seen["kinds"] = [p.kind for p in a.gui._sinks[0].view.rows()]
        return 0

    monkeypatch.setattr(a.gui, "run", run)
    a.mod.main()
    assert a.radio.made == [(rate, 1)]
    assert seen["kinds"] == ["CONNECT_IND", "SCAN_RSP", "ADV_IND"]


def test_the_engine_decodes_in_the_loop(app, monkeypatch):
    """With esdr.pcap_start() (the S3 in multiapp) no ring: the radio's engine detects, each
    pass decodes what it closed; a retune stops and flushes it, then starts it again."""
    iq, _ = ble_stream([v["air"] for v in V37] * 2)
    a = app(iq, engine=True)
    seen = {}

    def run():
        view = a.gui._sinks[0].view
        passes(a, 30)
        seen["ring"] = view.cap.ring
        seen["kinds"] = [p.kind for p in view.rows()]
        bars = [v for v in a.gui._vars if type(v).__name__ == "QToolBar"]
        boxes = [w for b in bars for w in b._widgets if type(w).__name__ == "QComboBox"]
        boxes[0]._picked(1)  # BLE 38
        return 0

    monkeypatch.setattr(a.gui, "run", run)
    a.mod.main()
    assert seen["ring"] is None
    assert seen["kinds"] == ["CONNECT_IND", "SCAN_RSP", "ADV_IND"] * 2
    starts = [c for c in a.radio.calls if c[0] in ("pcap_start", "pcap_stop")]
    assert starts == [("pcap_start",), ("pcap_stop",), ("pcap_start",), ("pcap_stop",)]
    assert ("tune", 2426e6) in a.radio.calls
    assert a.radio.engine is None


def test_a_short_display_gets_tabs(app, monkeypatch):
    """The Communicator's 428 x 142 panel: the table on one tab with all the height below the
    tab bar, the controls on the other; the XIAO's 320 x 240 keeps one page."""
    iq, _ = ble_stream([V37[0]["air"]])
    seen = {}
    for size in ((428, 142), (320, 240)):
        a = app(iq, display=size)

        def run():
            from gnuradio import _layout

            out = []
            root = a.gui.root
            if root is not None:
                _layout.fit(root, 0, a.gui.HEADER, a.gui.W, a.gui.H - a.gui.HEADER, out)
            seen[size] = (root, [(type(r[0]).__name__, r[2], r[4]) for r in out])
            return 0

        monkeypatch.setattr(a.gui, "run", run)
        a.mod.main()
    root, rects = seen[(428, 142)]
    assert root is not None
    assert rects == [("QTabWidget", 18, 26), ("_Table", 44, 98)]
    assert seen[(320, 240)] == (None, [])


def test_no_2mss_no_app(app, monkeypatch):
    a = app(np.zeros(2, np.int16))
    a.radio.RATES = (1000000,)
    monkeypatch.setattr(a.gui, "run", lambda: pytest.fail("a GUI without the rate"))
    with pytest.raises(OSError):
        a.mod.main()


def test_headless_prints_packets(app, monkeypatch, capsys):
    iq, _ = ble_stream([v["air"] for v in V37])
    a = app(iq)

    def run():
        passes(a, 10)
        return 0

    monkeypatch.setattr(a.gui, "run", run)
    a.mod.main()
    out = capsys.readouterr().out.splitlines()
    assert len([s for s in out if "ADV_IND" in s or "SCAN_RSP" in s or "CONNECT_IND" in s]) == 3


def test_view_grows_as_a_whole_build(app):
    """The newest-first view grows by the packets that came since its last build: always the
    rows a whole build gives, through late packets (inserted before the newest), trims and
    every filter; other groups and sorts build whole."""
    a = app(np.zeros(2 * 1000, np.int16), engine=True)
    mod, pv_ = a.mod, sys.modules["pcap_view"]
    rng = np.random.default_rng(11)
    cap = types.SimpleNamespace(pkts=[], count=0, trimmed=0, version=0, run_t0=0.0, at0=0,
                                rate=RATE, freq=2402e6)
    v = mod.View(cap)
    grows = []
    plain = v._grow
    v._grow = lambda: grows.append(plain()) or grows[-1]
    at = 0
    name = bytes([2, 0x01, 0x06, 6, 0x09]) + b"Plant"
    for step in range(400):
        batch = []
        for _ in range(int(rng.integers(0, 12))):
            at += int(rng.integers(100, 5000))
            late = rng.random() < 0.05
            when = at - int(rng.integers(1, 40000)) if late else at
            kind = int(rng.integers(0, 3))
            rec = ble_rec(adv(0, bytes(rng.integers(0, 256, 6, dtype=np.uint8)), name)) \
                if kind == 0 else (tap_rec(bytes([0x02, 0x00, 9, 0, 0])) if kind == 1 else
                                   bytes(60))
            batch.append(raw(kind, rec, ok=bool(rng.random() < 0.9), at=max(0, when)))
        mod.Capture._add(cap, batch)
        v.filt = pv_.FILTERS[(step // 50) % len(pv_.FILTERS)]
        if step % 97 == 96:
            v.sort = "Oldest"
        elif step % 97 == 0:
            v.sort = "Newest"
        a.clock[0] += 1000
        rows = v.rows()
        want = pv_.view(cap.pkts, v.filt, v.group, v.sort, v.source)
        assert [p.no for p in rows] == [p.no for p in want], step
    assert cap.trimmed > 0
    # Most passes grow; late packets, trims and the other sort build whole.
    assert sum(grows) > 200 and len(grows) - sum(grows) > 20
