#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""BLE packets from the chancap app's captures, as pcap for Wireshark.

Every capture (from the board through chancap_bridge.py's reader, or .npz files that
chancap_bridge.py --save wrote) is demodulated as LE 1M: an FM discriminator summed over a
symbol, the access address looked for at every sampling phase (the advertising one, or --aa),
the carrier offset taken from the access address's own bits, then the PDU header, payload and
CRC dewhitened with the channel's index and the CRC-24 checked (0x555555 on the advertising
channels, --crc-init elsewhere). The link layer bit handling is apps/ubertooth/tools/le_ref.py.

Each packet becomes a pcap record of LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR (256): the radio
header (RF channel, the reference access address and its bit errors, dewhitened, CRC checked
and valid) and the access address, PDU and CRC as on air. The board's levels are dBFS, not
dBm, so the header's power fields stay unset. Timestamps are the board's sample clock, placed
at the host's time of the first capture.

Usage: ble_pcap.py [-w FILE|-] [--serial SN] [--seconds N] [--npz DIR] [--aa AA]
                   [--crc-init X] [--max-errors N] [--bad] [--print]

  -w -        pcap on stdout, e.g. ble_pcap.py -w - | wireshark -k -i -
  --npz DIR   the captures chancap_bridge.py --save wrote, instead of the board
  --bad       also packets whose CRC failed (flagged in the header)
  --print     one line per packet on stderr

Exit status 0 when at least one packet passed its CRC.
"""

import argparse
import glob
import importlib.util
import json
import os
import struct
import sys
import time

import numpy as np

_HERE = os.path.dirname(os.path.abspath(__file__))


def _load(name, *path):
    spec = importlib.util.spec_from_file_location(name, os.path.join(_HERE, *path))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


le = _load("le_ref", "..", "..", "ubertooth", "tools", "le_ref.py")

ADV_AA = le.ADV_AA
ADV_CRC_INIT = 0x555555
LINKTYPE_BLE_LL_PHDR = 256
# LINKTYPE 256 header flags.
F_DEWHITENED = 0x0001
F_REF_AA_VALID = 0x0010
F_AA_OFFENSES_VALID = 0x0020
F_CRC_CHECKED = 0x0400
F_CRC_VALID = 0x0800
SYMBOL_RATE = 1e6


def aa_bits(aa):
    return np.array([(aa >> i) & 1 for i in range(32)], dtype=np.int8)


def symbol_sums(iq, sps):
    """Phase advance over one symbol from every sample on (FM discriminator, summed)."""
    d = np.angle(iq[1:] * np.conj(iq[:-1]))
    return np.convolve(d, np.ones(sps), mode="valid")


def slice_bits(soft, aa_at, aa):
    """Bits of the soft symbols from the access address on, sliced at the carrier offset, and
    that offset in radians a symbol: the mean over the preamble's last 6 symbols (alternating,
    so the filter's intersymbol interference cancels), else the middle between the access
    address's ones and zeros (biased by it: their runs differ)."""
    if aa_at >= 6:
        mid = soft[aa_at - 6:aa_at].mean()
    else:
        ref = aa_bits(aa).astype(bool)
        s = soft[aa_at:aa_at + 32]
        mid = (s[ref].mean() + s[~ref].mean()) / 2
    return (soft[aa_at:] > mid).astype(np.int8), mid


def find_aa(soft_by_phase, aa, max_errors, start):
    """(phase, symbol index, bit errors) of the best access address match at or after
    symbol index start (in every phase's own symbols), or None. The match is the normalized
    correlation of 32 soft symbols with the address (no threshold, so no carrier offset to
    know first); its bit errors count against the middle between that window's ones and
    zeros."""
    ref = aa_bits(aa).astype(bool)
    p = np.where(ref, 1.0, -1.0)
    psum = p.sum()
    var_p = 32 - psum * psum / 32
    best = None
    for ph, soft in enumerate(soft_by_phase):
        s = soft[start:]
        if len(s) < 32:
            continue
        c = np.correlate(s, p, mode="valid")
        cs = np.concatenate([[0.0], np.cumsum(s)])
        cs2 = np.concatenate([[0.0], np.cumsum(s * s)])
        s1 = cs[32:] - cs[:-32]
        var_s = cs2[32:] - cs2[:-32] - s1 * s1 / 32
        r = (c - s1 * psum / 32) / np.sqrt(np.maximum(var_s * var_p, 1e-12))
        k = int(np.argmax(r))
        w = s[k:k + 32]
        mid = (w[ref].mean() + w[~ref].mean()) / 2
        errors = int(np.count_nonzero((w > mid) != ref))
        key = (errors, -r[k])
        if errors <= max_errors and (best is None or key < best[3]):
            best = (ph, start + k, errors, key)
    return None if best is None else best[:3]


def decode(iq, rate, mhz, aa=ADV_AA, crc_init=ADV_CRC_INIT, max_errors=2):
    """LE 1M packets of one capture: dicts with the on-air bytes (access address, PDU, CRC),
    whether the CRC holds, the access address's bit errors, the first sample of the access
    address, the carrier offset (Hz) and the channel index."""
    sps = int(round(rate / SYMBOL_RATE))
    if sps < 2 or abs(rate - sps * SYMBOL_RATE) > 1:
        raise ValueError(f"{rate} S/s is not a whole number of samples a symbol (2 or more)")
    idx = le.channel_index(int(round(mhz)))
    sums = symbol_sums(np.asarray(iq), sps)
    phases = [sums[p::sps] for p in range(sps)]
    packets = []
    start = 0
    while True:
        hit = find_aa(phases, aa, max_errors, start)
        if hit is None:
            break
        ph, at, _ = hit
        bits, mid = slice_bits(phases[ph], at, aa)
        errors = int(np.count_nonzero(bits[:32] != aa_bits(aa)))
        body = bits[32:]
        if len(body) < 16:
            break
        wh = np.array(le.whitening_bits(idx, len(body)), dtype=np.int8)
        clear = body ^ wh
        length = int(sum(int(clear[8 + j]) << j for j in range(8)))
        n = 8 * (2 + length + 3)
        if len(clear) < n:
            # Cut off by the end of the capture: the next search starts past this one.
            start = at + 32
            continue
        data = le.bits_to_bytes([int(b) for b in clear[:n]])
        pdu, crc = data[:-3], data[-3:]
        ok = le.crc_wire_bytes(le.crc24(crc_init, pdu)) == crc
        packets.append({
            "air": bytes([(aa >> (8 * k)) & 0xFF for k in range(4)] + data),
            "pdu": bytes(pdu),
            "crc_ok": ok,
            "aa": aa,
            "aa_errors": errors,
            "sample": ph + at * sps,
            "offset_hz": mid / (2 * np.pi) * SYMBOL_RATE,
            "channel": idx,
        })
        start = at + 32 + n
    return packets


def rf_channel(mhz):
    """RF channel number of the LINKTYPE 256 header: 0 is 2402 MHz, 39 is 2480 MHz."""
    return int(round((mhz - 2402) / 2))


class PcapWriter:
    """A classic pcap stream (microseconds, little endian) of LINKTYPE 256 records."""

    def __init__(self, f):
        self.f = f
        f.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, LINKTYPE_BLE_LL_PHDR))
        f.flush()

    def write(self, t, mhz, pkt, crc_checked=True):
        flags = F_DEWHITENED | F_REF_AA_VALID | F_AA_OFFENSES_VALID
        if crc_checked:
            flags |= F_CRC_CHECKED | (F_CRC_VALID if pkt["crc_ok"] else 0)
        hdr = struct.pack("<BbbBIH", rf_channel(mhz), 0, 0, pkt["aa_errors"], pkt["aa"], flags)
        rec = hdr + pkt["air"]
        sec = int(t)
        usec = int(round((t - sec) * 1e6))
        if usec >= 1000000:
            sec, usec = sec + 1, usec - 1000000
        self.f.write(struct.pack("<IIII", sec, usec, len(rec), len(rec)) + rec)
        self.f.flush()


PDU_TYPES = {0: "ADV_IND", 1: "ADV_DIRECT_IND", 2: "ADV_NONCONN_IND", 3: "SCAN_REQ",
             4: "SCAN_RSP", 5: "CONNECT_IND", 6: "ADV_SCAN_IND", 7: "ADV_EXT_IND"}


def describe(pkt, mhz):
    pdu = pkt["pdu"]
    kind = PDU_TYPES.get(pdu[0] & 0x0F, "type %d" % (pdu[0] & 0x0F)) \
        if pkt["aa"] == ADV_AA else "data"
    addr = ":".join("%02x" % b for b in reversed(pdu[2:8])) if len(pdu) >= 8 else "-"
    return (f"{mhz:.0f} MHz ch {pkt['channel']:2d} {kind:15s} {addr} len {len(pdu) - 2:3d}"
            f" crc {'ok ' if pkt['crc_ok'] else 'BAD'} aa errors {pkt['aa_errors']}"
            f" offset {pkt['offset_hz'] / 1e3:+6.1f} kHz")


class Clock:
    """Host time of a board time: the board's clock placed at the host's time of the first."""

    def __init__(self):
        self.offset = None

    def __call__(self, board_t):
        if self.offset is None:
            self.offset = time.time() - board_t
        return self.offset + board_t


def npz_captures(directory):
    for path in sorted(glob.glob(os.path.join(directory, "*.npz"))):
        z = np.load(path)
        meta = json.loads(str(z["meta"]))
        meta["iq"] = z["iq"]
        yield meta


def board_captures(serial, seconds, base):
    bridge = _load("chancap_bridge", "chancap_bridge.py")
    dev = bridge.find_device(serial)
    if dev is None:
        sys.exit("no chancap board found")
    intf, ep = bridge.open_bulk_in(dev)
    ps = bridge.PacketStream(base)
    streams = {}
    t0 = time.monotonic()
    try:
        while seconds == 0 or time.monotonic() - t0 < seconds:
            try:
                block = bytes(dev.read(ep.bEndpointAddress, bridge.BLOCK, timeout=500))
            except bridge.usb.core.USBTimeoutError:
                continue
            for raw in ps.feed(block):
                try:
                    p = bridge.vrt_rx.parse(raw)
                except (ValueError, IndexError, struct.error) as e:
                    print(f"bad packet: {e}", file=sys.stderr)
                    continue
                ch = p["stream_id"] - base
                cap = streams.setdefault(ch, bridge.Stream(ch)).take(p)
                if cap is not None:
                    yield cap
    finally:
        bridge.usb.util.release_interface(dev, intf)
        if ps.skipped_words:
            print(f"{ps.skipped_words} words skipped in the USB stream", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("-w", dest="out", default="ble.pcap", help="pcap file, - for stdout")
    ap.add_argument("--serial", help="USB serial of the board")
    ap.add_argument("--seconds", type=float, default=10.0, help="run time (0: forever)")
    ap.add_argument("--npz", metavar="DIR", help="decode saved captures instead")
    ap.add_argument("--aa", type=lambda s: int(s, 0), default=ADV_AA, help="access address")
    ap.add_argument("--crc-init", type=lambda s: int(s, 0), default=ADV_CRC_INIT)
    ap.add_argument("--max-errors", type=int, default=2, help="access address bit errors")
    ap.add_argument("--bad", action="store_true", help="also packets whose CRC failed")
    ap.add_argument("--print", action="store_true", help="one line per packet on stderr")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0x5D5D0200,
                    help="stream ID of channel 0")
    args = ap.parse_args()

    out = sys.stdout.buffer if args.out == "-" else open(args.out, "wb")
    pcap = PcapWriter(out)
    clock = Clock()
    caps = npz_captures(args.npz) if args.npz else \
        board_captures(args.serial, args.seconds, args.base)
    n_caps = good = bad = 0
    try:
        for cap in caps:
            n_caps += 1
            mhz = cap["rx_freq"] / 1e6
            rate = cap["rx_rate"]
            t0 = cap["rx_time"][0] + cap["rx_time"][1]
            for pkt in decode(cap["iq"], rate, mhz, args.aa, args.crc_init, args.max_errors):
                if pkt["crc_ok"]:
                    good += 1
                else:
                    bad += 1
                    if not args.bad:
                        continue
                pcap.write(clock(t0 + pkt["sample"] / rate), mhz, pkt)
                if args.print:
                    print(describe(pkt, mhz), file=sys.stderr)
    except (KeyboardInterrupt, BrokenPipeError):
        pass
    finally:
        if out is not sys.stdout.buffer:
            out.close()
    print(f"{n_caps} captures, {good} packets with a good CRC, {bad} bad", file=sys.stderr)
    return 0 if good else 1


if __name__ == "__main__":
    sys.exit(main())
