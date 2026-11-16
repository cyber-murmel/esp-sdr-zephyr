#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Wideband packet capture on the PC, as pcapng for Wireshark.

An osmosdr device's stream (osmosdr_hs on an ESP32-S31: 20 MS/s over USB high speed, through the
esdr SoapySDR module) or a recording of one is cut by GNU Radio into the Bluetooth LE and
IEEE 802.15.4 channels inside it, one frequency translating filter each, down to 4 MS/s. Every
channel goes through apps/pcap's burst detector and decoders: the Pcap app's capture core
(apps/micropython/cmod/esdr_pcap_core.c), built here with the host compiler and cached. The
records are apps/pcap's (README.rst): Bluetooth LE packets on LINKTYPE 256, 802.15.4 frames on
LINKTYPE 283, every other burst as a LINKTYPE_USER0 record (tools/esdr_burst.lua), each with its
metadata comment.

What the channels see of each other is sorted out before the write: an access address a
CONNECT_IND taught one channel is followed on all of them, a burst record of a signal a
neighbouring channel decoded (the channels are 1 MHz apart where the two grids meet) goes, so does
a second channel's record of one signal, and a wide signal (Wi-Fi), which lifts the channels it
covers over their floors unevenly and in pieces, comes out as one wideband record over its span
and time.

  esdr_gr_pcap.py -f 2409M -w capture.pcapng
  esdr_gr_pcap.py -f 2409M -w - | wireshark -k -i -
  esdr_gr_pcap.py --file rec.cs8 -f 2409M -w capture.pcapng

Needs GNU Radio's Python (gnuradio, with gr-soapy and the esdr SoapySDR module for a device) and a
C compiler: python3 of the dev shell (default.nix) has them all.
"""

import argparse
import collections
import ctypes
import hashlib
import heapq
import os
import platform
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[3]

# The capture core and apps/pcap's decoders (tests/host/test_pcap_egg.py builds the same set).
SOURCES = ("apps/micropython/cmod/esdr_pcap_core.c", "apps/pcap/src/dec_ble.c",
           "apps/pcap/src/dec_wpan.c", "apps/pcap/src/dec_generic.c", "apps/pcap/src/refine.c",
           "apps/pcap/src/records.c", "apps/pcap/src/pcapng.c", "lib/ieee802154/ieee154_phy.c",
           "apps/ubertooth/src/upstream/bluetooth_le.c")
INCLUDES = ("include", "apps/pcap/src", "apps/ubertooth/src/upstream", "apps/micropython/cmod")
CFLAGS = ("-std=gnu11", "-O2", "-shared", "-fPIC")

# pcap_proto.h
IF_BLE, IF_WPAN, IF_BURST = 0, 1, 2
IF_NAMES = ("BLE", "802.15.4", "burst")
DEC_BLE = 0x0001
DEC_WPAN = 0x0002
DEC_GENERIC = 0x0004
DEC_WIDEBAND = 0x0008
DEC_BAD_CRC = 0x0010
DEC_BLE_DATA = 0x0020
DEC_COMMENTS = 0x0040
DEC_DEFAULT = DEC_BLE | DEC_WPAN | DEC_GENERIC | DEC_WIDEBAND | DEC_BLE_DATA | DEC_COMMENTS
MOD_WIDEBAND = 8
# struct pcap_burst_hdr.flags: bits follow.
BF_BITS = 0x0040
# ... the generic analysis did not run (no time): no offset, bandwidth or bits.
BF_SKIPPED = 0x0200
# struct pcap_burst_hdr: version, hdr_len, flags, freq_hz, offset_hz, bandwidth_hz, duration_us,
# symbol_rate, deviation_hz, modulation, rx_gain, channel, channels, power, floor and peak
# (dBFS x 10), reserved, sample_rate_hz, nbits.
BURST_HDR = struct.Struct("<BBHQiIIIIBBBBhhhHII")
BURST_CHANNEL_AT = struct.calcsize("<BBHQiIIIIBB")
GAIN_AUTO = 255
# The channels' filter, in the channels' rate: flat to PASS (1 MHz at 4 MS/s: Bluetooth LE 1M
# whole, 802.15.4's main lobe but its outer half MHz), 50 dB down from STOP (1.6 MHz: the
# Bluetooth LE channels 2 MHz on, 1.5 to 2.5 MHz out, stay out).
PASS, STOP = 0.25, 0.4
# Channels whose records of one moment make a wide signal of them, even with none finding it wide.
WIDE_CHANNELS = 3
# Half the main lobe of a decoded packet's spectrum, Hz: Bluetooth LE 1M, 802.15.4 O-QPSK.
HALF_WIDTH = {IF_BLE: 1000000, IF_WPAN: 1500000}

# The decoders' full scale (esdr_pcap_core.h: 16384 for the stream's full scale, 1.0 here).
FULL = 16384.0

# The grids, Hz: Bluetooth LE RF channels 0..39, 802.15.4 channels 11..26.
BLE_HZ = tuple(2402000000 + 2000000 * k for k in range(40))
WPAN_HZ = tuple(2405000000 + 5000000 * k for k in range(16))


def ble_channel_hz(index):
    """A Bluetooth LE channel index (0..36 data, 37..39 advertising) as its frequency, Hz."""
    if index == 37:
        return 2402000000
    if index == 38:
        return 2426000000
    if index == 39:
        return 2480000000
    if 0 <= index <= 10:
        return 2404000000 + 2000000 * index
    if 11 <= index <= 36:
        return 2428000000 + 2000000 * (index - 11)
    raise ValueError(f"no Bluetooth LE channel {index}")


def ble_index(hz):
    """The Bluetooth LE channel index of a frequency on the grid, None off it."""
    if hz not in BLE_HZ:
        return None
    rf = (hz - 2402000000) // 2000000
    return {0: 37, 12: 38, 39: 39}.get(rf, rf - 1 if rf < 12 else rf - 2)


def wpan_channel_hz(number):
    if not 11 <= number <= 26:
        raise ValueError(f"no 802.15.4 channel {number} at 2.4 GHz (11..26)")
    return 2405000000 + 5000000 * (number - 11)


def wpan_number(hz):
    return 11 + (hz - 2405000000) // 5000000 if hz in WPAN_HZ else None


def channel_name(hz):
    names = []
    if ble_index(hz) is not None:
        names.append(f"BLE {ble_index(hz)}")
    if wpan_number(hz) is not None:
        names.append(f"802.15.4 {wpan_number(hz)}")
    return f"{hz / 1e6:g}" + (f" ({', '.join(names)})" if names else "")


def channel_plan(spec, center_hz, edge_hz):
    """The channels to decode, Hz, sorted: a comma separated list of 'all', 'ble', 'wpan' (the
    grids' channels at most edge_hz from the centre), 'bleN' (Bluetooth LE channel index N),
    'wpanN' (802.15.4 channel N) or a frequency in MHz."""
    out = set()
    for item in spec.lower().replace(" ", "").split(","):
        if item in ("all", "ble", "wpan"):
            grid = (BLE_HZ if item != "wpan" else ()) + (WPAN_HZ if item != "ble" else ())
            out.update(f for f in grid if abs(f - center_hz) <= edge_hz)
        elif item.startswith("ble"):
            out.add(ble_channel_hz(int(item[3:])))
        elif item.startswith("wpan"):
            out.add(wpan_channel_hz(int(item[4:])))
        elif item:
            out.add(round(float(item) * 1e6))
    far = [f for f in out if abs(f - center_hz) > edge_hz]
    if far:
        raise ValueError(f"{', '.join(f'{f / 1e6:g}' for f in sorted(far))} MHz: more than "
                         f"{edge_hz / 1e6:g} MHz from the centre (--edge)")
    return sorted(out)


def channel_rate(rate):
    """The channels' rate and the decimation to it: 4 MS/s where the stream's rate is a multiple
    of it (Bluetooth LE 1M and 2M, 802.15.4), else 2 MS/s (LE 1M; the core interpolates 802.15.4
    to 4 MS/s)."""
    for ch in (4000000, 2000000):
        if rate % ch == 0:
            return ch, rate // ch
    raise ValueError(f"{rate} S/s: the stream's rate must be a multiple of 2 MS/s")


def parse_hz(text):
    """Hz from '2409e6', '2409M', '20M', '500k'."""
    text = text.strip()
    scale = {"k": 1e3, "K": 1e3, "M": 1e6, "G": 1e9}.get(text[-1:], None)
    value = float(text[:-1]) * scale if scale else float(text)
    if value != 0 and abs(value) < 1e5 and not scale:
        raise argparse.ArgumentTypeError(f"{text}: Hz, or with a unit (2409M)")
    return value


def parse_aa(text):
    """AA[:CRCINIT], hexadecimal."""
    aa, _, crc = text.partition(":")
    return int(aa, 16), int(crc, 16) if crc else 0x555555


# ---- the C core ----


class Cfg(ctypes.Structure):
    _fields_ = [("freq_hz", ctypes.c_uint64), ("rate_hz", ctypes.c_uint32),
                ("shift", ctypes.c_uint8), ("gain", ctypes.c_uint8), ("ratio", ctypes.c_float),
                ("decoders", ctypes.c_uint32), ("burst_max", ctypes.c_uint32),
                ("clock", ctypes.c_void_p), ("pool", ctypes.c_uint32)]


class CPkt(ctypes.Structure):
    _fields_ = [("iface", ctypes.c_uint8), ("ok", ctypes.c_bool), ("power_x10", ctypes.c_int16),
                ("floor_x10", ctypes.c_int16), ("at", ctypes.c_int64),
                ("offset_hz", ctypes.c_int32), ("duration_us", ctypes.c_uint32),
                ("rec", ctypes.c_void_p), ("len", ctypes.c_uint16), ("comment", ctypes.c_char_p)]


STATS = ("samples", "gaps", "bursts", "blips", "truncated", "pool_full", "ble", "ble_bad", "wpan",
         "wpan_bad", "generic", "wide", "skipped", "lost")


class CStats(ctypes.Structure):
    _fields_ = ([("samples", ctypes.c_uint64)] + [(k, ctypes.c_uint32) for k in STATS[1:]] +
                [(k, ctypes.c_uint64) for k in ("cyc_ble", "cyc_wpan", "cyc_generic")])


def cache_dir():
    base = os.environ.get("XDG_CACHE_HOME") or os.path.join(os.path.expanduser("~"), ".cache")
    return Path(base) / "esdr-pcap"


def build_lib(out_dir=None, cc=None):
    """The core as a shared library, built once per content of its sources and headers."""
    cc = cc or os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        raise RuntimeError("no C compiler (cc) to build the decoders")
    srcs = [str(REPO / s) for s in SOURCES]
    incs = ["-I" + str(REPO / i) for i in INCLUDES]
    deps = subprocess.run([cc, "-MM", *incs, *srcs], check=True, capture_output=True,
                          text=True).stdout
    files = sorted({t for t in deps.replace("\\\n", " ").split() if not t.endswith(":")})
    digest = hashlib.sha256(" ".join([cc, *CFLAGS]).encode())
    for f in files:
        digest.update(f.encode())
        digest.update(Path(f).read_bytes())
    out_dir = Path(out_dir) if out_dir else cache_dir()
    lib = out_dir / f"libesdr_pcap-{digest.hexdigest()[:16]}.so"
    if not lib.exists():
        out_dir.mkdir(parents=True, exist_ok=True)
        fd, tmp = tempfile.mkstemp(suffix=".so", dir=out_dir)
        os.close(fd)
        try:
            subprocess.run([cc, *CFLAGS, "-o", tmp, *incs, *srcs, "-lm"], check=True)
            os.replace(tmp, lib)
        finally:
            if os.path.exists(tmp):
                os.unlink(tmp)
    return lib


def load_lib(path):
    lib = ctypes.CDLL(str(path))
    vp, u32 = ctypes.c_void_p, ctypes.c_uint32
    sigs = {
        "esdr_pcap_size": (ctypes.c_size_t, [ctypes.POINTER(Cfg)]),
        "esdr_pcap_init": (vp, [vp, ctypes.c_size_t, ctypes.POINTER(Cfg)]),
        "esdr_pcap_feed": (u32, [vp, vp, u32, ctypes.c_int64, u32]),
        "esdr_pcap_flush": (u32, [vp, u32]),
        "esdr_pcap_next": (ctypes.c_bool, [vp, u32, ctypes.POINTER(CPkt)]),
        "esdr_pcap_get_stats": (None, [vp, ctypes.POINTER(CStats)]),
        "esdr_pcap_analyses_left": (u32, [vp]),
        "esdr_pcap_learn": (None, [vp, u32, u32]),
        "esdr_pcap_learnt": (ctypes.c_bool, [vp, u32, ctypes.POINTER(u32), ctypes.POINTER(u32)]),
        "esdr_pcap_head": (ctypes.c_int, [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p,
                                          ctypes.c_char_p, ctypes.c_char_p]),
        "esdr_pcap_epb": (ctypes.c_int, [ctypes.c_char_p, ctypes.c_size_t, u32, ctypes.c_uint64,
                                         ctypes.c_char_p, u32, ctypes.c_char_p]),
    }
    for name, (res, args) in sigs.items():
        fn = getattr(lib, name)
        fn.restype, fn.argtypes = res, args
    return lib


class Pkt:
    """A packet or record of one channel, with what the cross-channel checks need."""

    __slots__ = ("iface", "ok", "power_x10", "floor_x10", "at", "end", "offset_hz",
                 "duration_us", "rec", "comment", "chan", "freq_hz", "centre", "wide", "lo",
                 "hi", "dead", "own", "vague")

    def __init__(self, p, chan, freq_hz, ch_rate):
        self.iface, self.ok = p.iface, p.ok
        self.power_x10, self.floor_x10 = p.power_x10, p.floor_x10
        self.at, self.offset_hz, self.duration_us = p.at, p.offset_hz, p.duration_us
        self.rec = ctypes.string_at(p.rec, p.len)
        self.comment = p.comment.decode() if p.comment else None
        self.chan, self.freq_hz = chan, freq_hz
        self.end = self.at + max(1, p.duration_us * ch_rate // 1000000)
        self.centre = freq_hz + p.offset_hz
        self.wide = (self.iface == IF_BURST and len(self.rec) >= BURST_HDR.size and
                     self.rec[BURST_CHANNEL_AT - 2] == MOD_WIDEBAND)
        # The signal's span: a burst record's occupied bandwidth (a wide one at least half the
        # channel), a quarter of the channels' rate either side where it is not known.
        half = ch_rate / 4
        if self.iface == IF_BURST and len(self.rec) >= BURST_HDR.size:
            bw = BURST_HDR.unpack_from(self.rec)[5]
            half = max(half, bw / 2) if self.wide else bw / 2 if bw else half
        self.lo, self.hi = self.centre - half, self.centre + half
        # own: the record's header is this tool's (a merged one), its channel fields set.
        self.dead = self.own = False
        # A record the analysis did not see: no carrier offset, the signal anywhere in the band.
        self.vague = (self.iface == IF_BURST and len(self.rec) >= BURST_HDR.size and
                      (BURST_HDR.unpack_from(self.rec)[2] & BF_SKIPPED) != 0)


class Channel:
    """One channel's detector and decoders (esdr_pcap_core.h)."""

    def __init__(self, lib, index, freq_hz, rate_hz, decoders, gain, ratio):
        self.lib, self.index, self.freq_hz, self.rate = lib, index, int(freq_hz), rate_hz
        cfg = Cfg(int(freq_hz), rate_hz, 0, gain, ratio, decoders, 0, None, 0)
        size = lib.esdr_pcap_size(ctypes.byref(cfg))
        if size == 0:
            raise ValueError(f"{freq_hz / 1e6:g} MHz at {rate_hz} S/s: no valid configuration")
        self.mem = ctypes.create_string_buffer(size + 8)
        self.st = lib.esdr_pcap_init((ctypes.addressof(self.mem) + 7) & ~7, size,
                                     ctypes.byref(cfg))
        if not self.st:
            raise ValueError(f"{freq_hz / 1e6:g} MHz: the core refused its configuration")
        self._p = CPkt()

    def packets(self, count):
        out = []
        for k in range(count):
            if not self.lib.esdr_pcap_next(self.st, k, ctypes.byref(self._p)):
                break
            out.append(Pkt(self._p, self.index, self.freq_hz, self.rate))
        return out

    def stats(self):
        s = CStats()
        self.lib.esdr_pcap_get_stats(self.st, ctypes.byref(s))
        return {k: getattr(s, k) for k in STATS}


# ---- the pcapng out ----


def overlap(a, b, slack):
    return a.at < b.end + slack and b.at < a.end + slack


class Wide:
    """A wide signal's records while it may still grow, in time and frequency."""

    def __init__(self, members):
        self.members = list(members)
        self.at = min(q.at for q in members)
        self.end = max(q.end for q in members)
        self.lo = min(q.lo for q in members)
        self.hi = max(q.hi for q in members)

    def add(self, q):
        self.members.append(q)
        self.end = max(self.end, q.end)
        self.lo, self.hi = min(self.lo, q.lo), max(self.hi, q.hi)


class Output:
    """Writes the channels' packets in time order, a hold back behind the newest sample, after
    the cross-channel checks (module docstring); merge=False writes them as the channels saw
    them."""

    def __init__(self, lib, stream, ch_rate, freqs, decoders=DEC_DEFAULT, gain=GAIN_AUTO,
                 t0_ns=None, hardware="ESP-SDR", merge=True, flush=False, tol_hz=750e3):
        self.lib, self.stream, self.ch_rate = lib, stream, ch_rate
        self.freqs, self.decoders, self.gain = list(freqs), decoders, gain
        self.t0_ns, self.merge, self.flush_each, self.tol = t0_ns, merge, flush, tol_hz
        # A burst is at most 5 ms (the core's default cap) and closes some blocks after: half
        # again the longest packet's reach covers both.
        self.holdback = 3 * ch_rate // 200
        self.slack = 50 * ch_rate // 1000000
        # Records starting this close on several channels are one signal's; a wide signal's
        # pieces join it until it is this long quiet, up to wide_max from its start.
        self.coincide = 8 * ch_rate // 1000000
        self.gap = 100 * ch_rate // 1000000
        self.wide_max = 10000 * ch_rate // 1000000
        self.heap, self.seq = [], 0
        self.recent = collections.deque()
        self.open_w, self.pending = [], []
        self.written = [0, 0, 0]
        self.crosstalk = self.duplicates = self.merged = self.wide_dropped = 0
        self.buf = ctypes.create_string_buffer(4096)
        self.hardware = hardware
        if stream is not None:
            self.open(stream)

    def open(self, stream):
        """The section header and apps/pcap's interfaces, the start of the stream."""
        self.stream = stream
        n = self.lib.esdr_pcap_head(self.buf, len(self.buf), self.hardware.encode(),
                                    platform.platform().encode(), b"esdr_gr_pcap.py")
        if n < 0:
            raise RuntimeError("pcapng header")
        self.stream.write(self.buf.raw[:n])

    def anchor(self, at):
        """The PC's clock at stream index @p at (live: the samples just in)."""
        self.t0_ns = time.time_ns() - round(at * 1e9 / self.ch_rate)

    def add(self, pkts):
        for p in pkts:
            heapq.heappush(self.heap, (p.at, self.seq, p))
            self.seq += 1

    def drain(self, now, final=False):
        while self.heap and (final or self.heap[0][0] < now - self.holdback):
            p = heapq.heappop(self.heap)[2]
            if not p.dead:
                self._close_wides(p.at)
                self._emit(p)
        # No record can join a wide signal quiet since what has come out.
        self._close_wides(None if final else now - self.holdback)
        if not final:
            while self.recent and self.recent[0].end < now - 2 * self.holdback:
                self.recent.popleft()
        if self.flush_each:
            self.stream.flush()

    def _others(self):
        yield from self.recent
        for _, _, q in self.heap:
            if not q.dead:
                yield q

    def _emit(self, p):
        if not self.merge:
            if not (p.wide and not self.decoders & DEC_WIDEBAND):
                self._write(p)
            return
        if p.iface == IF_BURST:
            self._emit_record(p)
        else:
            self._remember(p)
            self._out(p)

    def _remember(self, p):
        # recent is kept in end order, roughly: the oldest end is pruned first.
        self.recent.append(p)

    def _out(self, p):
        """Written now, or after the wide signals still open (they start earlier)."""
        if self.open_w:
            self.pending.append(p)
        else:
            self._write(p)

    def _close_wides(self, t):
        """The wide signals quiet since before @p t (None: all) as their records, and what waited
        for them, in time order."""
        done = [w for w in self.open_w if t is None or w.end + self.gap < t]
        if not done:
            return
        self.open_w = [w for w in self.open_w if w not in done]
        ready = self.pending + [self._wide_record(w) for w in done]
        first = min((w.at for w in self.open_w), default=None)
        self.pending = [q for q in ready if first is not None and q.at >= first]
        for q in sorted((q for q in ready if first is None or q.at < first), key=lambda q: q.at):
            self._write(q)

    def _wide_record(self, w):
        r = self._merge(w.members) if len(w.members) > 1 else w.members[0]
        self._remember(r)
        return r

    def _near(self, a, b):
        """Two records' frequency spans touch (within the tolerance)."""
        return max(a.lo, b.lo) - min(a.hi, b.hi) <= self.tol

    def _together(self, a, b):
        """Two records of one signal: started at once, or at one time and frequency."""
        return (abs(a.at - b.at) <= self.coincide or
                (overlap(a, b, self.slack) and self._near(a, b)))

    def _emit_record(self, p):
        """A burst record: part of a wide signal (one record over its span and time), a packet
        another channel decoded, another channel's record of the same signal, or a record of its
        own."""
        for w in self.open_w:
            if (p.at <= w.end + self.gap and p.at - w.at <= self.wide_max and
                    self._near(p, w)):
                w.add(p)
                self.merged += 1
                return
        # The records of the same moment, through each other.
        cands = [q for _, _, q in self.heap
                 if not q.dead and q.iface == IF_BURST and
                 (abs(q.at - p.at) <= self.coincide or overlap(p, q, self.slack))]
        group, grew = [p], True
        while grew:
            grew = False
            for q in cands:
                if q not in group and any(self._together(q, g) for g in group):
                    group.append(q)
                    grew = True
        # Less what packets decoded elsewhere left in the channels, a wide signal (Wi-Fi): one a
        # channel found filling it, one three channels or more saw, or two far apart (a wide
        # signal lifts the channels it fills over their floors unevenly).
        rest = [g for g in group if not self._leak(g)]
        chans = {g.chan for g in rest}
        if rest and (any(g.wide for g in rest) or len(chans) >= WIDE_CHANNELS or
                     (len(chans) == 2 and not self._near(rest[0], next(
                         g for g in rest if g.chan != rest[0].chan)))):
            for g in group:
                g.dead = True
            self.crosstalk += len(group) - len(rest)
            if not self.decoders & DEC_WIDEBAND:
                self.wide_dropped += len(rest)
                return
            self.merged += len(rest) - 1
            self.open_w.append(Wide(rest))
            return
        if self._leak(p):
            self.crosstalk += 1
            return
        # One signal in two channels: at one frequency, or between them from the same moment.
        for q in self._others():
            if (q is not p and q.iface == IF_BURST and overlap(p, q, self.slack) and
                    (abs(q.centre - p.centre) <= max(self._tol(p), self._tol(q)) or
                     (abs(q.at - p.at) <= self.coincide and self._near(p, q))) and
                    (q in self.recent or self._rank(q) < self._rank(p))):
                self.duplicates += 1
                return
        self._remember(p)
        self._out(p)

    @staticmethod
    def _rank(p):
        """Which of two records of one signal stays (the lower): an analysed one nearest its
        channel's centre, else the stronger."""
        return (p.vague, -p.power_x10 if p.vague else abs(p.offset_hz))

    def _tol(self, p):
        """How far a record's centre may be from another's for the same signal: further for one
        whose signal may be anywhere in its channel's flat band."""
        return self.tol + (PASS * self.ch_rate if p.vague else 0)

    def _leak(self, p):
        """Whether burst record @p p is a packet another channel decoded: one at its frequency, or
        one reaching into its channel's filter (short of the stop band) at least 6 dB louder."""
        f = self.freqs[p.chan]
        band = STOP * self.ch_rate
        for q in self._others():
            if q.iface == IF_BURST or not overlap(p, q, self.slack):
                continue
            if abs(q.centre - p.centre) <= self._tol(p):
                return True
            half = HALF_WIDTH[q.iface]
            if (f - band <= q.centre + half and q.centre - half <= f + band and
                    p.power_x10 <= q.power_x10 - 60):
                return True
        return False

    def _merge(self, group):
        """One wideband record over the group's span."""
        first = next((q for q in group if q.wide), group[0])
        h = list(BURST_HDR.unpack_from(first.rec))
        lo, hi = min(q.lo for q in group), max(q.hi for q in group)
        start, end = min(q.at for q in group), max(q.end for q in group)
        heads = [BURST_HDR.unpack_from(q.rec) for q in group]
        centre = round((lo + hi) / 2)
        chan = min(range(len(self.freqs)), key=lambda k: abs(self.freqs[k] - centre))
        dur = (end - start) * 1000000 // self.ch_rate
        h[2] = 0
        for g in heads:
            h[2] |= g[2]
        h[2] &= ~(BF_BITS | BF_SKIPPED)
        h[3], h[4], h[5], h[6], h[7], h[8] = centre, 0, round(hi - lo), dur, 0, 0
        h[9], h[11], h[12] = MOD_WIDEBAND, chan, min(255, len(self.freqs))
        h[13] = max(g[13] for g in heads)
        h[14] = min(g[14] for g in heads)
        h[15] = max(g[15] for g in heads)
        h[17], h[18] = self.ch_rate, 0
        rec = BURST_HDR.pack(*h) + first.rec[BURST_HDR.size:h[1]]
        w = Pkt.__new__(Pkt)
        w.iface, w.ok, w.power_x10, w.floor_x10 = IF_BURST, True, h[13], h[14]
        w.at, w.end, w.offset_hz, w.duration_us = start, end, 0, dur
        w.rec, w.chan, w.freq_hz, w.centre = rec, chan, centre, centre
        w.wide, w.lo, w.hi, w.dead, w.own, w.vague = True, lo, hi, False, True, False
        w.comment = None
        if self.decoders & DEC_COMMENTS:
            gain = "auto" if self.gain == GAIN_AUTO else str(self.gain)
            w.comment = (f"esdr: {centre / 1e6:.3f} MHz, ch {chan}/{len(self.freqs)} at "
                         f"{self.ch_rate / 1e6:.3f} MS/s, offset +0.0 kHz, power "
                         f"{h[13] / 10:.1f} dBFS, floor {h[14] / 10:.1f} dBFS, snr "
                         f"{(h[13] - h[14]) / 10:.1f} dB, gain {gain}, {dur} us, wideband in "
                         f"{len(group)} channels, {lo / 1e6:.3f} to {hi / 1e6:.3f} MHz")
        return w

    def _write(self, p):
        n_ch = min(255, len(self.freqs))
        rec, comment = p.rec, p.comment
        if p.iface == IF_BURST and len(rec) >= BURST_HDR.size and not p.own:
            rec = rec[:BURST_CHANNEL_AT] + bytes((p.chan, n_ch)) + rec[BURST_CHANNEL_AT + 2:]
        if comment:
            comment = comment.replace(", ch 0/1 at ", f", ch {p.chan}/{n_ch} at ", 1)
        cb = comment.encode() if comment else None
        need = len(rec) + (len(cb) if cb else 0) + 64
        if need > len(self.buf):
            self.buf = ctypes.create_string_buffer(need)
        t_ns = max(0, (self.t0_ns or 0) + round(p.at * 1e9 / self.ch_rate))
        n = self.lib.esdr_pcap_epb(self.buf, len(self.buf), p.iface, t_ns, rec, len(rec), cb)
        if n < 0:
            raise RuntimeError("pcapng block")
        self.stream.write(self.buf.raw[:n])
        self.written[p.iface] += 1


# ---- the channels side by side ----


def connect_ind(p):
    """A Bluetooth LE CONNECT_IND with a good CRC (LINKTYPE 256: its 10-byte header, the access
    address, then the PDU's header, type 5), which teaches its channel's core a connection."""
    return p.iface == IF_BLE and p.ok and len(p.rec) > 14 and p.rec[14] & 0x0f == 0x05


class Capture:
    """The channels' cores fed side by side (complex samples, full scale 1, one array per
    channel, all starting at the same index), their packets into the Output."""

    def __init__(self, lib, freqs, ch_rate, out, decoders=DEC_DEFAULT, gain=GAIN_AUTO,
                 ratio=4.0, analyses=1000, aas=()):
        self.lib, self.ch_rate, self.out = lib, ch_rate, out
        self.chans = [Channel(lib, k, f, ch_rate, decoders, gain, ratio)
                      for k, f in enumerate(freqs)]
        self.analyses = analyses
        self.tokens = float(analyses)
        self.known = {}
        self.samples = 0
        self.stopped = False
        self._f = np.empty(0, np.float32)
        self._aa, self._crc = ctypes.c_uint32(), ctypes.c_uint32()
        for aa, crc in aas:
            self._teach(aa, crc, None)

    def _teach(self, aa, crc, source):
        self.known[aa] = crc
        for ch in self.chans:
            if ch is not source:
                self.lib.esdr_pcap_learn(ch.st, aa, crc)

    def _share(self, chans):
        """Access addresses the channels learnt, to all."""
        for ch in chans:
            k = 0
            while self.lib.esdr_pcap_learnt(ch.st, k, ctypes.byref(self._aa),
                                            ctypes.byref(self._crc)):
                if self.known.get(self._aa.value) != self._crc.value:
                    self._teach(self._aa.value, self._crc.value, ch)
                k += 1

    def _int16(self, x):
        """int16 I, Q pairs at the decoders' full scale: GNU Radio's as they come, complex
        samples (full scale 1) converted."""
        if x.dtype == np.int16:
            return np.ascontiguousarray(x)
        f = np.asarray(x, dtype=np.complex64).view(np.float32)
        if len(self._f) < len(f):
            self._f = np.empty(len(f), np.float32)
        b = self._f[:len(f)]
        np.multiply(f, FULL, out=b)
        np.rint(b, out=b)
        np.clip(b, -32768, 32767, out=b)
        return b.astype(np.int16)

    def feed(self, xs, at):
        n = min(len(x) for x in xs)
        if n == 0:
            return
        if self.out.t0_ns is None:
            self.out.anchor(at + n)
        self.tokens = min(float(self.analyses), self.tokens + self.analyses * n / self.ch_rate)
        pkts, learnt = [], []
        for ch, x in zip(self.chans, xs):
            iq = self._int16(x[:n])
            budget = int(self.tokens)
            count = self.lib.esdr_pcap_feed(ch.st, iq.ctypes.data, n, at, budget)
            self.tokens -= budget - self.lib.esdr_pcap_analyses_left(ch.st)
            got = ch.packets(count)
            if any(connect_ind(q) for q in got):
                learnt.append(ch)
            pkts += got
        self._share(learnt)
        self.out.add(pkts)
        self.samples = at + n
        self.out.drain(self.samples)

    def finish(self):
        pkts = []
        for ch in self.chans:
            pkts += ch.packets(self.lib.esdr_pcap_flush(ch.st, max(0, int(self.tokens))))
        self._share(self.chans)
        self.out.add(pkts)
        self.out.drain(self.samples, final=True)

    def totals(self):
        tot = dict.fromkeys(STATS, 0)
        for ch in self.chans:
            for k, v in ch.stats().items():
                tot[k] += v
        return tot


# ---- GNU Radio ----


def make_sink(gr, cap):
    class Sink(gr.sync_block):
        """The channels into the Capture, all in one call (one index for all)."""

        def __init__(self):
            gr.sync_block.__init__(self, name="esdr_pcap",
                                   in_sig=[(np.int16, 2)] * len(cap.chans), out_sig=None)

        def work(self, input_items, output_items):
            n = len(input_items[0])
            try:
                cap.feed(input_items, self.nitems_read(0))
            except BrokenPipeError:
                cap.stopped = True
                return -1
            except Exception:
                traceback.print_exc()
                cap.stopped = True
                return -1
            return n

    return Sink()


def build_flowgraph(args, cap, freqs, rate, decim, ch_rate, gain):
    try:
        from gnuradio import blocks, gr
        from gnuradio import filter as grfilter
        from gnuradio.filter import firdes
    except ImportError as e:
        raise SystemExit(f"esdr_gr_pcap: needs GNU Radio's Python ({e}); see --help") from None
    tb = gr.top_block("esdr_gr_pcap")
    if args.file:
        if args.format == "cs8":
            src = blocks.file_source(gr.sizeof_char, args.file, False)
            head = blocks.interleaved_char_to_complex(False, 128.0)
            tb.connect(src, head)
        elif args.format == "cs16":
            src = blocks.file_source(gr.sizeof_short, args.file, False)
            head = blocks.interleaved_short_to_complex(False, False, 32768.0)
            tb.connect(src, head)
        else:
            head = blocks.file_source(gr.sizeof_gr_complex, args.file, False)
    else:
        try:
            from gnuradio import soapy
        except ImportError as e:
            raise SystemExit(f"esdr_gr_pcap: needs gr-soapy for a device ({e})") from None
        dev = ",".join(x for x in ("driver=esdr", f"serial={args.serial}" if args.serial else "",
                                   args.args) if x)
        try:
            head = soapy.source(dev, "fc32", 1, "", "", [""], [""])
        except RuntimeError as e:
            raise SystemExit(f"esdr_gr_pcap: {dev}: {e} (on USB? --serial, --args)") from None
        head.set_sample_rate(0, rate)
        head.set_frequency(0, args.freq)
        if gain == GAIN_AUTO:
            head.set_gain_mode(0, True)
        else:
            head.set_gain_mode(0, False)
            head.set_gain(0, gain)
    if args.seconds:
        lim = blocks.head(gr.sizeof_gr_complex, int(args.seconds * rate))
        tb.connect(head, lim)
        head = lim
    # 50 dB: as deep as 8-bit samples reach.
    taps = firdes.low_pass_2(1.0, rate, (PASS + STOP) / 2 * ch_rate, (STOP - PASS) * ch_rate, 50)
    sink = make_sink(gr, cap)
    sink.set_max_noutput_items(16384)
    # GNU Radio holds the block, not its Python object, whose work() it calls.
    tb.sink = sink
    for k, f in enumerate(freqs):
        xl = grfilter.freq_xlating_fir_filter_ccf(decim, taps, f - args.freq, rate)
        # int16 pairs at the decoders' full scale, clipped, for the cores as they are.
        tb.connect(head, xl, blocks.complex_to_interleaved_short(True, FULL), (sink, k))
    return tb


def progress(cap, out, t0):
    tot = cap.totals()
    return (f"{cap.samples / cap.ch_rate:7.1f} s ({time.monotonic() - t0:.1f} s): BLE "
            f"{tot['ble']} ({tot['ble_bad']} bad), 802.15.4 {tot['wpan']} ({tot['wpan_bad']} "
            f"bad), bursts {tot['bursts']}, written {sum(out.written)}")


def run(tb, cap, out, quiet):
    waiter = threading.Thread(target=tb.wait, daemon=True)
    t0 = time.monotonic()
    tb.start()
    waiter.start()
    last = t0
    try:
        while waiter.is_alive() and not cap.stopped:
            waiter.join(0.2)
            if not quiet and time.monotonic() - last >= 2.0:
                last = time.monotonic()
                print("esdr_gr_pcap: " + progress(cap, out, t0), file=sys.stderr)
    except KeyboardInterrupt:
        pass
    tb.stop()
    waiter.join()
    return t0


def file_start_ns(args, rate):
    """The recording's first sample on the PC's clock: --start, else its file's time less its
    length (esdr_usb -o writes as the samples come)."""
    if args.start is not None:
        return round(args.start * 1e9)
    st = os.stat(args.file)
    item = {"cs8": 2, "cs16": 4, "cf32": 8}[args.format]
    return st.st_mtime_ns - round(st.st_size / item / rate * 1e9)


def parse_args(argv=None):
    ap = argparse.ArgumentParser(
        description="Bluetooth LE and 802.15.4 packets from every channel of an osmosdr "
        "stream (osmosdr_hs, 20 MS/s) or a recording, as pcapng (apps/pcap's records).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="channels: 'all' (default), 'ble', 'wpan', 'bleN', 'wpanN' or MHz, comma "
        "separated.\ndecoders (--dec, hex): ble 1, wpan 2, generic 4, wideband 8, bad CRC 10, "
        "ble data 20, comments 40\n(default 6f).\n\nNeeds GNU Radio's Python: python3 of the "
        "dev shell (default.nix) has it.\n  esdr_gr_pcap.py -f 2409M -w - | wireshark -k -i -")
    ap.add_argument("-w", "--write", required=True, help="the pcapng file, - for stdout")
    ap.add_argument("-f", "--freq", type=parse_hz, default=2409e6,
                    help="the stream's centre (default 2409M: Bluetooth LE 37, 0..6 and "
                    "802.15.4 11..13, no channel at DC)")
    ap.add_argument("-r", "--rate", type=parse_hz, default=20e6, help="samples a second "
                    "(default 20M)")
    ap.add_argument("-g", "--gain", default="auto", help="receive gain index, or auto (default)")
    ap.add_argument("-s", "--serial", help="the device's USB serial number")
    ap.add_argument("--args", default="", help="more SoapySDR device arguments")
    ap.add_argument("--file", help="decode a recording instead of a device (--format, --start)")
    ap.add_argument("--format", choices=("cs8", "cs16", "cf32"), default="cs8",
                    help="the recording's items (default cs8, as esdr_usb -o writes 8-bit ones)")
    ap.add_argument("--start", type=float, help="the recording's first sample, seconds since "
                    "the epoch (default: the file's time less its length)")
    ap.add_argument("-c", "--channels", default="all", help="the channels to decode (below)")
    ap.add_argument("--edge", type=parse_hz, help="the farthest a channel's centre may be from "
                    "the stream's (default: half the rate less half a channel: 8M at 20 MS/s)")
    ap.add_argument("-t", "--seconds", type=float, help="stop after this much of the stream")
    ap.add_argument("--dec", type=lambda s: int(s, 16), default=DEC_DEFAULT,
                    help="decoders, hex (below)")
    ap.add_argument("--ratio", type=float, default=4.0, help="a burst opens this many times "
                    "over a channel's floor, power (default 4)")
    ap.add_argument("--analyses", type=int, default=1000, help="generic analyses a second of "
                    "the stream, all channels (default 1000; the rest are recorded unanalysed)")
    ap.add_argument("--aa", type=parse_aa, action="append", default=[],
                    help="a Bluetooth LE access address to follow, AA[:CRCINIT] in hex")
    ap.add_argument("--no-merge", action="store_true", help="write every channel's records as "
                    "it saw them (no crosstalk, duplicate or wideband merging)")
    ap.add_argument("--lib", help="a built libesdr_pcap (default: built and cached in "
                    "$XDG_CACHE_HOME/esdr-pcap)")
    ap.add_argument("-q", "--quiet", action="store_true", help="no progress lines")
    return ap.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    rate = round(args.rate)
    try:
        ch_rate, decim = channel_rate(rate)
        edge = args.edge if args.edge is not None else max(0.0, rate / 2 - ch_rate / 2)
        freqs = channel_plan(args.channels, round(args.freq), edge)
    except ValueError as e:
        raise SystemExit(f"esdr_gr_pcap: {e}") from None
    if not freqs:
        raise SystemExit("esdr_gr_pcap: no channel in the band (--channels, --edge)")
    if args.gain == "auto":
        gain = GAIN_AUTO
    else:
        gain = int(args.gain)
    lib = load_lib(args.lib or build_lib())

    if args.file:
        t0_ns, hw = file_start_ns(args, rate), f"ESP-SDR recording {Path(args.file).name}"
    else:
        t0_ns, hw = None, "ESP-SDR (SoapySDR driver=esdr)"
    out = Output(lib, None, ch_rate, freqs, args.dec, gain, t0_ns, hw,
                 merge=not args.no_merge, flush=args.write == "-")
    cap = Capture(lib, freqs, ch_rate, out, args.dec, gain, args.ratio, args.analyses, args.aa)
    if not args.quiet:
        print(f"esdr_gr_pcap: {args.freq / 1e6:g} MHz at {rate / 1e6:g} MS/s, {len(freqs)} "
              f"channels at {ch_rate / 1e6:g} MS/s: " + ", ".join(map(channel_name, freqs)),
              file=sys.stderr)
    # The device first: no file is made when it is not there.
    tb = build_flowgraph(args, cap, freqs, rate, decim, ch_rate, gain)
    if args.write == "-":
        # The pcapng gets stdout's descriptor; what libraries print goes to stderr.
        sys.stdout.flush()
        stream = os.fdopen(os.dup(1), "wb")
        os.dup2(2, 1)
    else:
        stream = open(args.write, "wb")
    out.open(stream)
    t0 = run(tb, cap, out, args.quiet)
    try:
        cap.finish()
        stream.flush()
    except BrokenPipeError:
        pass
    if not args.quiet:
        print("esdr_gr_pcap: " + progress(cap, out, t0), file=sys.stderr)
        print(f"esdr_gr_pcap: written BLE {out.written[0]}, 802.15.4 {out.written[1]}, bursts "
              f"{out.written[2]}; left out: crosstalk {out.crosstalk}, duplicates "
              f"{out.duplicates}, merged into wideband records {out.merged}", file=sys.stderr)
        for ch in cap.chans:
            s = ch.stats()
            print(f"  {channel_name(ch.freq_hz):>24}: BLE {s['ble']:5} 802.15.4 {s['wpan']:5} "
                  f"bursts {s['bursts']:6} wide {s['wide']:6} analysed {s['generic']:5} "
                  f"unanalysed {s['skipped']:5}", file=sys.stderr)
    try:
        stream.close()
    except BrokenPipeError:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
