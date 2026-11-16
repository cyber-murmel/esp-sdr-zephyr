#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bridge the chancap app's USB stream to GNU Radio PDUs.

The board sends VITA 49.2 packets in 4 KiB blocks on a vendor bulk IN endpoint: one stream
per channel (stream ID base + channel), a context packet before each capture (RF reference
frequency, sample rate, the noise floor as the reference level in dBFS, the peak above it
as gain stage 1), then signal data packets whose trailer marks the capture's first packet
(sample loss), its last (user bit 8), a cut at max_capture (bit 9) and a chunk of a
continuous stream (bit 10). Every capture becomes one PDU: a metadata dictionary and a
complex float vector (full scale 32768 -> 1.0).

Usage: chancap_bridge.py [--serial SN] [--seconds N] [--print] [--save DIR]
                         [--zmq tcp://*:5555] [--rate-check]

--print    one line per capture (channel, time, length, power, flags)
--save     every capture as <dir>/<n>_ch<c>.npz (iq, meta)
--zmq      publish serialized PDUs (gr-zeromq "ZMQ SUB Message Source" reads them)

Exit status 0 when captures arrived with no packet counter gaps.
"""

import argparse
import importlib.util
import json
import os
import struct
import sys
import time

import numpy as np

try:
    import usb.core
    import usb.util
except ImportError:  # pragma: no cover
    sys.exit("pyusb is needed (python3-usb)")

# The VRT parser of apps/stream/tools/vrt_rx.py.
_HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "vrt_rx", os.path.join(_HERE, "..", "..", "stream", "tools", "vrt_rx.py"))
vrt_rx = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(vrt_rx)

VID, PID = 0x2FE3, 0x0005
PRODUCT = "ESP-SDR chancap"
BLOCK = 4096
STREAM_BASE = 0x5D5D0200
FULL_SCALE = 32768.0
# Trailer indicator bits (their enable bits sit 12 above): over-range 13, sample loss 12,
# user defined 11 to 8.
TR_OVER_RANGE, TR_SAMPLE_LOSS, TR_LAST, TR_TRUNCATED, TR_CHUNK = 13, 12, 8, 9, 10


def q9_7(word):
    """A 16-bit radix-7 fixed point field."""
    return struct.unpack(">h", struct.pack(">H", word & 0xFFFF))[0] / 128.0


def trailer_bit(word, bit):
    """Indicator bit of a trailer word, when its enable bit (bit + 12) is set."""
    return bool(word >> (bit + 12) & 1) and bool(word >> bit & 1)


def find_device(serial=None):
    for dev in usb.core.find(find_all=True, idVendor=VID, idProduct=PID):
        try:
            if usb.util.get_string(dev, dev.iProduct) != PRODUCT:
                continue
            if serial and usb.util.get_string(dev, dev.iSerialNumber) != serial:
                continue
        except usb.core.USBError:
            continue
        return dev
    return None


def open_bulk_in(dev):
    """Claim the vendor interface; return (interface number, IN endpoint)."""
    cfg = dev.get_active_configuration()
    for intf in cfg:
        if intf.bInterfaceClass != 0xFF:
            continue
        for ep in intf:
            if usb.util.endpoint_direction(ep.bEndpointAddress) == usb.util.ENDPOINT_IN and \
               usb.util.endpoint_type(ep.bmAttributes) == usb.util.ENDPOINT_TYPE_BULK:
                if dev.is_kernel_driver_active(intf.bInterfaceNumber):
                    dev.detach_kernel_driver(intf.bInterfaceNumber)
                usb.util.claim_interface(dev, intf.bInterfaceNumber)
                return intf.bInterfaceNumber, ep
    raise RuntimeError("no vendor bulk IN interface")


class PacketStream:
    """The bulk IN data as one byte stream of VRT packets. The board never splits a packet
    across its 4 KiB blocks, but host reads need not line up with the blocks (a short block
    that ends on a 64-byte boundary runs on into the next read when its zero-length packet
    goes missing), so a packet's tail carries over to the next read. A word that cannot start
    one of the board's packets (type, size up to a block, a stream ID of ours) is skipped,
    counted in skipped_words, until one can."""

    def __init__(self, base=STREAM_BASE):
        self.buf = b""
        self.base = base
        self.skipped_words = 0

    def _size(self, at):
        hdr, sid = struct.unpack_from(">II", self.buf, at)
        size = hdr & 0xFFFF
        if hdr >> 28 not in vrt_rx.WITH_STREAM_ID or not 3 <= size <= BLOCK // 4 or \
           not 0 <= sid - self.base <= 255:
            return 0
        return size

    def feed(self, data):
        """Whole packets the data completes."""
        self.buf += data
        at = 0
        out = []
        while len(self.buf) - at >= 8:
            size = self._size(at)
            if size == 0:
                at += 4
                self.skipped_words += 1
                continue
            if len(self.buf) - at < 4 * size:
                break
            out.append(self.buf[at:at + 4 * size])
            at += 4 * size
        self.buf = self.buf[at:]
        return out


class Stream:
    """One channel's stream: its context and the capture being assembled."""

    def __init__(self, channel):
        self.channel = channel
        self.context = {}
        self.count = None
        self.gaps = 0
        self.parts = []
        self.start = None
        self.flags = {}

    def take(self, p):
        """Feed one parsed packet; return a finished capture (dict) or None."""
        if p["type"] in vrt_rx.CONTEXT_TYPES:
            self.context.update(p)
            return None
        if self.count is not None and p["count"] != (self.count + 1) & 15:
            self.gaps += 1
        self.count = p["count"]
        tw = p.get("trailer_word", 0)
        if trailer_bit(tw, TR_SAMPLE_LOSS):  # a capture's first packet
            self.parts = []
            self.start = p
            self.flags = {"over_range": False}
        if self.start is None:
            return None
        self.parts.append(p["body"])
        self.flags["truncated"] = trailer_bit(tw, TR_TRUNCATED)
        self.flags["chunk"] = trailer_bit(tw, TR_CHUNK)
        self.flags["over_range"] |= trailer_bit(tw, TR_OVER_RANGE)
        if not trailer_bit(tw, TR_LAST):
            return None
        body = b"".join(self.parts)
        n = len(body) // 4
        raw = np.frombuffer(body, dtype=">i2")
        iq = (raw[0::2].astype(np.float32) + 1j * raw[1::2].astype(np.float32)) / FULL_SCALE
        ctx = self.context
        rate = ctx.get("sample_rate", 0.0)
        noise = q9_7(ctx.get("reference_level", 0))
        snr = q9_7(ctx.get("gain", 0))
        start = self.start
        cap = {
            "channel": self.channel,
            "rx_time": (start.get("seconds", 0), start.get("fraction", 0) / 1e12),
            "rx_freq": ctx.get("rf_frequency", 0.0),
            "rx_rate": rate,
            "start_index": start.get("seconds", 0) * 16000000 + start.get("fraction", 0) // 62500,
            "power_db": noise + snr,
            "noise_db": noise,
            "truncated": self.flags["truncated"],
            "chunk": self.flags["chunk"],
            "over_range": self.flags["over_range"],
            "n": n,
            "iq": iq,
        }
        self.parts = []
        self.start = None
        return cap


def pdu_bytes(cap):
    """The capture as a serialized GNU Radio PDU (pmt pair of metadata and c32 vector)."""
    import pmt

    meta = pmt.make_dict()
    meta = pmt.dict_add(meta, pmt.intern("rx_time"),
                        pmt.make_tuple(pmt.from_uint64(cap["rx_time"][0]),
                                       pmt.from_double(cap["rx_time"][1])))
    for key in ("rx_freq", "rx_rate", "power_db", "noise_db"):
        meta = pmt.dict_add(meta, pmt.intern(key), pmt.from_double(float(cap[key])))
    for key in ("channel", "start_index"):
        meta = pmt.dict_add(meta, pmt.intern(key), pmt.from_uint64(int(cap[key])))
    for key in ("truncated", "chunk"):
        meta = pmt.dict_add(meta, pmt.intern(key), pmt.from_bool(bool(cap[key])))
    vec = pmt.init_c32vector(len(cap["iq"]), cap["iq"].astype(np.complex64))
    return pmt.serialize_str(pmt.cons(meta, vec))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--serial", help="USB serial of the board")
    ap.add_argument("--seconds", type=float, default=10.0, help="run time (0: forever)")
    ap.add_argument("--print", action="store_true", help="one line per capture")
    ap.add_argument("--save", metavar="DIR", help="write every capture as .npz")
    ap.add_argument("--zmq", metavar="ENDPOINT", help="publish PDUs on a ZMQ PUB socket")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=STREAM_BASE,
                    help="stream ID of channel 0")
    args = ap.parse_args()

    dev = find_device(args.serial)
    if dev is None:
        sys.exit("no chancap board found")
    intf, ep = open_bulk_in(dev)
    pub = None
    if args.zmq:
        import zmq

        pub = zmq.Context().socket(zmq.PUB)
        pub.bind(args.zmq)
    if args.save:
        os.makedirs(args.save, exist_ok=True)

    streams = {}
    ps = PacketStream(args.base)
    captures = 0
    blocks = 0
    nbytes = 0
    errors = 0
    t0 = time.monotonic()
    last_report = t0
    try:
        while args.seconds == 0 or time.monotonic() - t0 < args.seconds:
            try:
                block = bytes(dev.read(ep.bEndpointAddress, BLOCK, timeout=500))
            except usb.core.USBTimeoutError:
                continue
            blocks += 1
            nbytes += len(block)
            for raw in ps.feed(block):
                try:
                    p = vrt_rx.parse(raw)
                except (ValueError, IndexError, struct.error) as e:
                    errors += 1
                    print(f"bad packet: {e}", file=sys.stderr)
                    continue
                ch = p["stream_id"] - args.base
                st = streams.setdefault(ch, Stream(ch))
                cap = st.take(p)
                if cap is None:
                    continue
                captures += 1
                if args.print:
                    t = cap['rx_time'][0] + cap['rx_time'][1]
                    print(f"ch {cap['channel']:2d} t {t:12.6f} s"
                          f" idx {cap['start_index']:12d} n {cap['n']:6d}"
                          f" {cap['n'] / max(cap['rx_rate'], 1) * 1e6:8.1f} us"
                          f" {cap['power_db']:6.1f} dBFS (floor {cap['noise_db']:6.1f})"
                          f"{' truncated' if cap['truncated'] else ''}"
                          f"{' chunk' if cap['chunk'] else ''}")
                if args.save:
                    meta = {k: v for k, v in cap.items() if k != "iq"}
                    np.savez(os.path.join(args.save, f"{captures:06d}_ch{cap['channel']}.npz"),
                             iq=cap["iq"], meta=json.dumps(meta))
                if pub is not None:
                    pub.send(pdu_bytes(cap))
            now = time.monotonic()
            if now - last_report >= 1.0 and not args.print:
                print(f"{captures} captures, {blocks} blocks, {nbytes / 1024:.0f} kB, "
                      f"{errors} errors, {ps.skipped_words} words skipped", file=sys.stderr)
                last_report = now
    except KeyboardInterrupt:
        pass
    finally:
        usb.util.release_interface(dev, intf)
    gaps = sum(s.gaps for s in streams.values())
    print(f"{captures} captures on {len(streams)} channels, {blocks} blocks, "
          f"{nbytes / 1024:.0f} kB, {gaps} counter gaps, {errors} errors, "
          f"{ps.skipped_words} words skipped")
    return 0 if captures and not gaps and not errors and not ps.skipped_words else 1


if __name__ == "__main__":
    sys.exit(main())
