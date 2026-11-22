#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A pure Python VITA 49.2 source block for the S31 stream, as a teaching example.

vrt_source subscribes to the board over unicast UDP (IPv6 preferred; the cookie handshake of
apps/stream_eth: VRTS, VRTC + cookie, VRTS + cookie, VRTK; VRTB means another host has the stream,
takeover=True sends VRTT instead), parses every packet with struct and outputs complex floats (the
cs8 payload scaled to +-1).

Tags, as UHD does: rx_time (seconds, fraction) on the first sample and after every discontinuity,
rx_freq and rx_rate from context packets, "overflow" (value: lost samples) on the first zero filled
sample after a loss, and "restart" (value: the count of restarts) where the board's time index jumped
by more than a second (no fill then). The gap is the jump of the packet time, which shows lost packets
of any number and samples the board dropped without a counter gap; before the rate is known the 4 bit
counter is used. Packets are held back four deep so that swapped ones are put in order; a packet too
late for that is dropped and counted in .late. A VrtBusy raised in work() ends the flowgraph.
Live on the board it took 8 to 10 MS/s (20 MS/s lost 4 %); on loopback 24 MS/s (bench.py). Above that
use vrt_pipe.py.
"""
import bisect
import socket
import struct
import sys
import time

import numpy as np
import pmt
from gnuradio import gr

# CIF0 fields before the sample rate, in transmission order: (bit, words)
CIF0_HEAD = [(30, 1), (29, 2), (28, 2), (27, 2), (26, 2), (25, 2), (24, 1), (23, 1), (22, 1), (21, 2),
             (20, 2), (19, 1), (18, 1), (17, 2), (16, 1)]
Q20 = 1 << 20
HOLD = 4                                           # packets held back to restore the order


def parse(pkt):
    """One big endian packet to a dict: type, count, and for data packets samples (cs8 bytes), time,
    lost (trailer sample loss); for context packets rf_frequency and sample_rate when present."""
    w = struct.unpack(f">{len(pkt) // 4}I", pkt)
    hdr = w[0]
    typ, trailer, tsi, tsf, count, size = hdr >> 28, hdr >> 26 & 1, hdr >> 22 & 3, hdr >> 20 & 3, hdr >> 16 & 15, hdr & 0xFFFF
    if size != len(w):
        raise ValueError("size field %d, packet has %d words" % (size, len(w)))
    i = 2 if typ in (1, 3, 4, 5) else 1                  # header, stream ID
    p = {"type": typ, "count": count, "stream_id": w[1] if i == 2 else 0}
    if hdr >> 27 & 1:
        i += 2                                            # class ID
    sec = w[i] if tsi else None
    i += bool(tsi)
    ps = (w[i] << 32 | w[i + 1]) if tsf else None
    i += 2 * bool(tsf)
    p["time"] = None if sec is None else (sec, (ps or 0) * 1e-12)
    p["sec"], p["ps"] = sec or 0, ps or 0
    if typ in (0, 1, 2, 3):
        end = len(w) - trailer
        p["samples"] = pkt[4 * i:4 * end]
        p["lost"] = bool(trailer and w[-1] >> 24 & 1 and w[-1] >> 12 & 1)
    elif typ in (4, 5):
        cif0, i = w[i], i + 1
        i += sum(cif0 >> b & 1 for b in (1, 2, 3))      # VITA 49.2: CIF1, CIF2, CIF3 words follow CIF0
        for bit, n in CIF0_HEAD:
            if cif0 >> bit & 1:
                if bit in (27, 21):
                    raw = struct.unpack(">q", struct.pack(">II", w[i], w[i + 1]))[0]
                    p["rf_frequency" if bit == 27 else "sample_rate"] = raw / Q20
                if bit == 16:
                    p["epoch"] = w[i] & 0xFF         # the settings epoch of the run whose data follows
                i += n
    return p


def command(freq=None, gain=None, rate=None, message_id=1, stream_id=0x5D5D0001):
    """The settings as a VITA 49.2 command packet (EXECUTE with the error words asked for, as vrt_udp.c
    writes it with libvrt, so that a refusal says which setting and cannot pass for an execution): CAM,
    message ID, CIF0, then the fields in CIF0 order: rf frequency and sample rate in Q44.20 Hz, gain
    stage 1 in Q9.7 (None: leave; -1: automatic)."""
    cif0, body = 0, []
    if freq is not None:
        cif0 |= 1 << 27
        body += list(struct.unpack(">II", struct.pack(">q", round(freq * Q20))))
    if gain is not None:
        cif0 |= 1 << 23
        body.append(round(gain * 128) & 0xFFFF)
    if rate is not None:
        cif0 |= 1 << 21
        body += list(struct.unpack(">II", struct.pack(">q", round(rate * Q20))))
    words = [6 << 28 | 5 + len(body), stream_id, 2 << 23 | 1 << 19 | 1 << 18 | 1 << 16, message_id, cif0] + body
    return struct.pack(f">{len(words)}I", *words)


def parse_ack(pkt):
    """An execution acknowledge (src/ctl.c send_ack): None for other command packets (query-state), else
    (executed, [names of the refused settings]). The board marks a refusal by leaving executed (CAM bit 10)
    out, and by the error words (bit 16: EIF0 and one response word per field) when they were asked for."""
    w = struct.unpack(f">{len(pkt) // 4}I", pkt)
    cam = w[2]
    if not cam >> 19 & 1:
        return None
    names = {27: "rf frequency", 23: "gain", 21: "sample rate", 15: "payload format"}
    refused = []
    if cam >> 16 & 1:
        eif0, resp = w[4], w[5:]
        for bit in (b for b in range(31, -1, -1) if eif0 >> b & 1):
            code = resp[len(refused)] if len(refused) < len(resp) else 0
            refused.append("%s%s" % (names.get(bit, "CIF0 bit %d" % bit), " (out of range)" if code & 0x10000000 else ""))
    return bool(cam >> 10 & 1), refused


class VrtBusy(RuntimeError):
    """The board streams to another host (VRTB); use takeover=True to take the stream."""


class vrt_source(gr.sync_block):
    def __init__(self, board="fe80::1%eth0", port=4992, takeover=False, rate=None, freq=None, gain=None):
        gr.sync_block.__init__(self, "vrt_source", [], [np.complex64])
        ai = socket.getaddrinfo(board, port, type=socket.SOCK_DGRAM)
        ai = sorted(ai, key=lambda x: x[0] != socket.AF_INET6)[0]          # IPv6 first
        self.sock = socket.socket(ai[0], socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 16 << 20)
        self.sock.connect(ai[4])                                           # only the board's packets
        self.cookie = None
        # settings (gain: "auto" or a gain index) go as a command once subscribed, until the board answers
        self.cmd = command(freq, -1 if gain == "auto" else gain, rate) if any(x is not None for x in (rate, freq, gain)) else None
        self.acked, self.tries = self.cmd is None, 0
        self.verb = b"VRTT" if takeover else b"VRTS"                       # until subscribed
        self.subscribed = False
        self.buf = np.zeros(0, np.complex64)     # received, not yet output
        self.tags = []                           # (position in buf, key, value)
        self.last = None                         # counter of the last data packet
        self.next = None                         # index of the sample expected next (from the packet times)
        self.high = None                         # end index of the newest packet that arrived
        self.seq = 0
        self.hold = []                           # packets held back to put swapped ones in order
        self.late = self.restarts = 0            # packets too late to place, restarts of the board's index
        self.per_packet = 0                      # samples in the last data packet
        self.bad = 0                             # datagrams that did not parse
        self.lost = 0                            # total lost samples
        self.sent_sub = self.sent_cmd = 0
        self.error = None                        # the exception that ended the flowgraph, if any
        self.rate = self.freq = None

    def stop(self):
        try:
            if self.cookie:
                self.sock.send(b"VRTX" + self.cookie)    # the board stops sending to us
        except OSError:                              # it is gone already (ICMP port unreachable)
            pass
        return True

    def _subscribe(self):
        now = time.monotonic()
        if now - self.sent_sub > 0.25:               # renew (the board wants one in 5 s, and loses many in a flood)
            self.sent_sub = now
            try:
                self.sock.send((self.verb if self.cookie else b"VRTS") + (self.cookie or b""))
                if self.subscribed and not self.acked and now - self.sent_cmd > 1:
                    self.sent_cmd = now
                    self.tries += 1
                    if self.tries > 5:                    # as vrt_udp: five tries, then an error
                        raise RuntimeError("no acknowledge of the settings")
                    self.sock.send(self.cmd)
            except ConnectionRefusedError:               # nothing listens yet (ICMP from an earlier datagram)
                pass

    def _control(self, raw):
        if raw[:4] == b"VRTC" and len(raw) == 12:    # a cookie: new, or ours gone stale
            self.cookie = raw[4:]
            self.sock.send(self.verb + self.cookie)
            self.sent_sub = time.monotonic()
        elif raw[:4] == b"VRTK":
            self.verb, self.subscribed = b"VRTS", True
            if not self.acked and self.tries == 0:
                self.tries, self.sent_cmd = 1, time.monotonic()
                self.sock.send(self.cmd)
        elif raw[:4] == b"VRTB":                    # at the start, or on a renewal when another host took it
            raise VrtBusy("the board streams to another host (or it was taken from us): "
                          "pass takeover=True (VRTT) to take it")

    def _packet(self, raw):
        if raw[:3] == b"VRT":
            return self._control(raw)
        if raw[:1] and raw[0] >> 4 == 6:                          # the answer to a command
            ack = parse_ack(raw) if len(raw) >= 16 else None
            if ack is not None:
                if not ack[0] or ack[1]:
                    raise RuntimeError("the board refused the settings%s" % (": " + ", ".join(ack[1]) if ack[1] else ""))
                self.acked = True
            return
        try:
            p = parse(raw)
        except (ValueError, struct.error, IndexError):
            self.bad += 1                                 # empty, short or inconsistent datagram
            return
        self.verb, self.subscribed = b"VRTS", True        # it streams to us: as good as VRTK, which may be lost
        if p["type"] == 4:
            for key, name, attr in (("rf_frequency", "rx_freq", "freq"), ("sample_rate", "rx_rate", "rate")):
                if key in p and p[key] != getattr(self, attr):
                    setattr(self, attr, p[key])
                    self.tags.append((len(self.buf), pmt.intern(name), pmt.from_double(p[key])))
            return
        if p["type"] != 1:
            return
        p["n"] = len(p["samples"]) // 2                   # samples (I, Q byte pairs) in the packet
        if self.rate and p["time"]:                       # index of the first sample, from the packet time
            r = int(self.rate)
            p["idx"] = p["sec"] * r + (p["ps"] * r + 500000000000) // 1000000000000
            if self.high is not None and abs(p["idx"] - self.high) > r:   # more than 1 s off: a restart
                self.flush()
                self.high = p["idx"] + p["n"]
                return self._emit(p, restart=True)
            self.high = max(self.high or 0, p["idx"] + p["n"])
            self.seq += 1
            bisect.insort(self.hold, (p["idx"], self.seq, p))       # hold back to put swaps in order
            if len(self.hold) > HOLD:
                self._emit(self.hold.pop(0)[2])
        else:                                             # no rate yet: the 4 bit counter, in arrival order
            self._emit(p)

    def flush(self):
        while self.hold:
            self._emit(self.hold.pop(0)[2])

    def _emit(self, p, restart=False):
        """Append one packet in index order: zeros for a gap up to 1 s, tags for every discontinuity."""
        iq = np.frombuffer(p["samples"], np.int8).astype(np.float32) * (1 / 128)
        iq = iq[:2 * p["n"]].view(np.complex64)
        idx, gap, first = p.get("idx"), 0, self.last is None
        if idx is None:
            gap = 0 if first else ((p["count"] - self.last - 1) & 15) * self.per_packet
        elif self.next is not None and not restart:
            gap = idx - self.next
            if gap < 0:                                   # older than what was output: a late or double packet
                self.late += 1
                return
        discont = first or restart or gap > 0
        if restart:                                       # time went far back or ahead: new index, no fill
            self.restarts += 1
            self.tags.append((len(self.buf), pmt.intern("restart"), pmt.from_long(self.restarts)))
        if discont and p["time"]:
            self.tags.append((len(self.buf) + gap, pmt.intern("rx_time"), pmt.make_tuple(
                pmt.from_uint64(p["time"][0]), pmt.from_double(p["time"][1]))))
        if gap:
            self.lost += gap
            self.tags.append((len(self.buf), pmt.intern("overflow"), pmt.from_long(gap)))
            self.buf = np.concatenate([self.buf, np.zeros(gap, np.complex64)])
        if idx is not None:
            self.next = idx + p["n"]
        self.last, self.per_packet = p["count"], p["n"]
        self.buf = np.concatenate([self.buf, iq])

    def work(self, input_items, output_items):
        """An error that cannot be recovered (VrtBusy, a refused setting) is kept in .error, printed, and
        ends the flowgraph (WORK_DONE): raised in a block thread it would leave top_block.wait() hanging."""
        try:
            return self._work(output_items[0])
        except (VrtBusy, RuntimeError) as e:
            self.error = e
            print(f"vrt_source: {e}", file=sys.stderr)
            return -1

    def _work(self, out):
        self._subscribe()
        try:
            while len(self.buf) < len(out):
                self.sock.settimeout(0.2 if len(self.buf) == 0 else 0)    # have data: take only what is queued
                self._packet(self.sock.recv(65536))
        except socket.timeout:                       # the stream paused: nothing is held back any more
            self.flush()
        except (BlockingIOError, ConnectionRefusedError):
            pass
        n = min(len(out), len(self.buf))
        for pos, key, val in [t for t in self.tags if t[0] < n]:
            self.add_item_tag(0, self.nitems_written(0) + pos, key, val)
        self.tags = [(pos - n, k, v) for pos, k, v in self.tags if pos >= n]
        out[:n] = self.buf[:n]
        self.buf = self.buf[n:]
        return n
