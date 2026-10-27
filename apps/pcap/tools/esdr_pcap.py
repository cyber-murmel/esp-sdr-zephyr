#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The pcap app's capture into Wireshark: an extcap, and a standalone tool.

The badge (apps/pcap, doc/pcap.md) sends one pcapng section on a vendor bulk IN endpoint: the
section header, an interface block per PCAP_IF_* (Bluetooth LE, IEEE 802.15.4 TAP, burst
records), then packets and statistics. This tool sets the radio, the channelizer and the
decoders up with vendor requests (esdr_proto.h's below 0x40, pcap_proto.h's from 0x40), starts
the capture and copies the stream as it comes, whole blocks only, into Wireshark's fifo or a
file until it is stopped (SIGTERM, SIGINT, the reader gone), then stops the capture.

  esdr_pcap.py --list                  the badges found
  esdr_pcap.py -w FILE [options]       a capture into a file ("-w -": stdout), e.g.
  esdr_pcap.py -w - | wireshark -k -i -

As an extcap (a symbolic link to this file in Wireshark's personal extcap folder, Help > About
> Folders) every badge is an interface "esdr-pcap-<serial>" with the options below in its
dialog. The burst records need tools/esdr_burst.lua among the personal Lua plugins.
"""

import argparse
import contextlib
import re
import select
import signal
import struct
import sys
import time

try:
    import usb.core
    import usb.util
except ImportError:  # pragma: no cover
    usb = None

VID, PID = 0x2FE3, 0x0005
PRODUCT = "ESP-SDR pcap"
IFACE_PREFIX = "esdr-pcap-"
HELP_URL = "https://github.com/cyber-murmel/esp-sdr-zephyr"
VENDOR_CLASS = 0xFF
CTRL_OUT, CTRL_IN = 0x40, 0xC0
CTRL_TIMEOUT_MS = 2000
READ_SIZE = 16384
READ_TIMEOUT_MS = 500
# After STOP: what the badge still sends (its last statistics), for at most this long.
DRAIN_S = 0.5
DRAIN_TIMEOUT_MS = 100

# The numbers of the headers (the host tests compare them): esdr_proto.h (apps/osmosdr/src),
# the radio's requests, which the P4 passes to the C6 or answers itself (the channelizer).
ESDR_REQ_SET_FREQ = 0x03
ESDR_REQ_SET_RX_GAIN = 0x05
ESDR_REQ_SET_GAPLESS = 0x11
ESDR_REQ_SET_CHANNELIZER = 0x13
ESDR_GAIN_AUTO = 0xFFFF
ESDR_GAPLESS_F_WIDE = 0x0004
# struct esdr_gapless_cfg: ratio_x16, pre, post, level, budget_bps, flags, headroom_lo, _hi.
GAPLESS_CFG = struct.Struct("<HBBIIHBB")
# struct esdr_chan_cfg: channels, open_db_x10, mask[3].
CHAN_CFG = struct.Struct("<HH3I")
CHAN_MASK_ALL = (0xFFFFFFFF,) * 3

# pcap_proto.h (apps/pcap/src): the app's own requests.
PCAP_REQ_START = 0x40
PCAP_REQ_STOP = 0x41
PCAP_REQ_SET_TIME = 0x42
PCAP_REQ_SET_DECODERS = 0x43
PCAP_REQ_GET_STATS = 0x44
PCAP_REQ_SET_MODE = 0x46
PCAP_MODE_SURVEY = 0
PCAP_MODE_CHANNEL = 1
PCAP_MODE_F_C6_WIDE = 0x0001
PCAP_MODE_F_HALF = 0x0002
PCAP_DEC_BLE = 0x0001
PCAP_DEC_WPAN = 0x0002
PCAP_DEC_GENERIC = 0x0004
PCAP_DEC_WIDEBAND = 0x0008
PCAP_DEC_BAD_CRC = 0x0010
PCAP_DEC_BLE_DATA = 0x0020
PCAP_DEC_COMMENTS = 0x0040
PCAP_LINKTYPE_USER0 = 147
# struct pcap_mode_cfg: mode, flags, channel_rate_hz, channel_freq_hz, survey_ms, channel_ms.
MODE_CFG = struct.Struct("<HHIQII")
# struct pcap_decoders: flags, min_burst_us, max_bits.
DECODERS_CFG = struct.Struct("<IHH")
# struct pcap_stats, its arrays per PCAP_IF_* (BLE, 802.15.4, bursts).
STATS = struct.Struct("<3I2Q3IIQ3I3IQIIIIQII3IQ")
STATS_FIELDS = ("seg_blocks seg_dropped other_blocks pairs chan_cycles bursts bursts_lost "
                "bursts_wide decoded dec_cycles packets_ble packets_wpan packets_burst "
                "dropped_ble dropped_wpan dropped_burst usb_bytes ring_peak cpu_mhz running "
                "mode channel_samples channel_gaps c6_wide generic generic_skipped "
                "generic_wide generic_cycles").split()

# pcapng.h: the blocks of the stream.
PCAPNG_BT_SHB = 0x0A0D0D0A
PCAPNG_BT_IDB = 0x00000001
PCAPNG_BT_ISB = 0x00000005
PCAPNG_BT_EPB = 0x00000006
PCAPNG_BYTE_ORDER_MAGIC = 0x1A2B3C4D
# Bytes of the smallest block of each type (no options, no data).
BLOCK_MIN = {PCAPNG_BT_SHB: 28, PCAPNG_BT_IDB: 20, PCAPNG_BT_ISB: 24, PCAPNG_BT_EPB: 32}
# Longest block taken: the badge's output ring. While resynchronising: a candidate this long at
# most, so that a false one cannot hold the stream up for long.
BLOCK_MAX = 65536
SCAN_MAX = 4096
_SHB_START = re.compile(re.escape(struct.pack("<I", PCAPNG_BT_SHB)))
_SCAN_START = re.compile(b"|".join(re.escape(struct.pack("<I", t))
                                   for t in (PCAPNG_BT_SHB, PCAPNG_BT_EPB, PCAPNG_BT_ISB)))

# --decoders names.
DECODERS = {"ble": PCAP_DEC_BLE, "wpan": PCAP_DEC_WPAN, "generic": PCAP_DEC_GENERIC,
            "wideband": PCAP_DEC_WIDEBAND, "ble-data": PCAP_DEC_BLE_DATA,
            "comments": PCAP_DEC_COMMENTS}


class Option:
    """A capture option: its command line argument and its line in the extcap dialog."""

    def __init__(self, call, kind, default, group, display, tooltip, lo=None, hi=None,
                 values=()):
        self.call, self.kind, self.default, self.group = call, kind, default, group
        self.display, self.tooltip, self.lo, self.hi, self.values = (display, tooltip, lo, hi,
                                                                     values)


# The command line and the extcap dialog (--extcap-config) both come from here.
OPTIONS = [
    Option("--mode", "selector", "survey", "Capture", "Mode",
           "Survey: the 80 MHz through the channelizer, bursts as they come (long ones cut "
           "short). Channel: one channel without gaps at 2 MS/s (whole LE 1M packets). "
           "Alternate: the two in turn",
           values=[("survey", "Survey (80 MHz)"), ("channel", "Channel (2 MS/s)"),
                   ("alternate", "Alternate")]),
    Option("--freq", "double", 2440.0, "Capture", "Centre frequency (MHz)",
           "The survey's centre: it covers +-40 MHz around it (+-20 MHz at 40 MS/s)",
           2400, 2500),
    Option("--survey-rate", "selector", 80, "Capture", "Survey rate (MS/s)",
           "80: the whole 80 MHz, bursts kept whole up to some 50 us. 40: 40 MHz around the "
           "centre, bursts kept whole some three times as long; what lies beyond 20 MHz out "
           "folds in (a few dB down near 20 MHz, 15 to 25 dB from 25 MHz on), no width sorting, "
           "not with 16 channels",
           values=[(80, "80 (80 MHz)"), (40, "40 (40 MHz, longer bursts)")]),
    Option("--channel-freq", "double", 2402.0, "Capture", "Channel frequency (MHz)",
           "The channel mode's channel (BLE advertising: 2402, 2426, 2480; 802.15.4: "
           "2405 + 5 (n - 11))",
           2400, 2500),
    Option("--gain", "integer", -1, "Capture", "Receive gain",
           "The radio's gain index (0 to 79), -1 for automatic: the C6's gain servo in the "
           "survey, gain 60 on the channel", -1, 79),
    Option("--survey-ms", "integer", 1000, "Capture", "Survey time (ms)",
           "Alternate mode: the time in the survey (a switch costs some 15 ms)", 20, 60000),
    Option("--channel-ms", "integer", 1000, "Capture", "Channel time (ms)",
           "Alternate mode: the time on the channel", 20, 60000),
    Option("--channels", "selector", 40, "Channelizer", "Channels",
           "Channels of the filterbank over the 80 MHz (their spacing): the decoders want "
           "2 MHz; at 40 MS/s half of them run, at the same spacing (16 needs 80 MS/s)",
           values=[(16, "16 (5 MHz)"), (32, "32 (2.5 MHz)"), (40, "40 (2 MHz)"),
                   (64, "64 (1.25 MHz)"), (80, "80 (1 MHz)")]),
    Option("--open-db", "double", 6.0, "Channelizer", "Opening level (dB)",
           "A channel's burst opens this far above its noise floor", 1, 40),
    Option("--ratio", "double", 2.0, "Channelizer", "Gapless threshold",
           "The C6 keeps 10 us windows this many times above the quiet baseline (power, the "
           "floor of its servo)", 1, 64),
    Option("--pre", "integer", 1, "Channelizer", "Windows before",
           "10 us windows the C6 keeps before a loud one", 0, 4),
    Option("--post", "integer", 1, "Channelizer", "Windows after",
           "10 us windows the C6 keeps after a loud one", 0, 4),
    Option("--c6-wide", "boolflag", False, "Channelizer", "Width sorting on the C6",
           "Signals wider than some 10 MHz (Wi-Fi) come as records without samples and leave "
           "the link to the narrow ones"),
    Option("--decoders", "multicheck", tuple(DECODERS), "Decoders", "Decoders",
           "What runs over the bursts",
           values=[("ble", "Bluetooth LE"), ("wpan", "IEEE 802.15.4"),
                   ("generic", "Records of the other bursts"),
                   ("wideband", "Records of wideband signals"),
                   ("ble-data", "BLE connections set up in view followed"),
                   ("comments", "Signal metadata as packet comments")]),
    Option("--bad-crc", "boolflag", False, "Decoders", "Packets with a bad CRC",
           "BLE packets and 802.15.4 frames whose CRC fails go out too, flagged"),
]


class BadgeError(Exception):
    """A problem with one clear line of explanation."""


def need_usb():
    if usb is None:
        raise BadgeError("pyusb is needed: pip install pyusb (or the python3-usb package)")


def ranged(conv, lo, hi):
    """An argparse type: @conv of the text within lo to hi."""
    def parse(text):
        v = conv(text)
        if not lo <= v <= hi:
            raise argparse.ArgumentTypeError(f"{text} is not within {lo} to {hi}")
        return v
    return parse


def decoder_list(text):
    """--decoders: names separated by commas (Wireshark's multicheck), maybe none."""
    names = [n.strip() for n in text.split(",") if n.strip()]
    for n in names:
        if n not in DECODERS:
            raise argparse.ArgumentTypeError(f"no decoder {n!r} (there are {', '.join(DECODERS)})")
    return names


def parse_args(argv):
    ap = argparse.ArgumentParser(prog="esdr_pcap.py", description=__doc__.split("\n")[0],
                                 allow_abbrev=False)
    g = ap.add_argument_group("standalone")
    g.add_argument("-w", dest="write", metavar="FILE", help="capture into FILE, - for stdout")
    g.add_argument("--list", action="store_true", help="the badges found")
    g.add_argument("--serial", help="the badge's USB serial number (with several badges)")
    g = ap.add_argument_group("extcap (Wireshark calls these)")
    g.add_argument("--extcap-interfaces", action="store_true")
    g.add_argument("--extcap-dlts", action="store_true")
    g.add_argument("--extcap-config", action="store_true")
    g.add_argument("--extcap-interface", metavar="IF")
    g.add_argument("--extcap-version", nargs="?", const="", metavar="VERSION")
    g.add_argument("--extcap-capture-filter", metavar="FILTER")
    g.add_argument("--extcap-control-in", help=argparse.SUPPRESS)
    g.add_argument("--extcap-control-out", help=argparse.SUPPRESS)
    g.add_argument("--capture", action="store_true")
    g.add_argument("--fifo", metavar="PATH")
    g = ap.add_argument_group("capture")
    for o in OPTIONS:
        if o.kind == "boolflag":
            g.add_argument(o.call, action="store_true", help=o.tooltip)
        elif o.kind == "selector":
            g.add_argument(o.call, type=type(o.default), choices=[v for v, _ in o.values],
                           default=o.default, help=f"{o.tooltip} (default {o.default})")
        elif o.kind == "multicheck":
            g.add_argument(o.call, type=decoder_list, default=list(o.default), metavar="LIST",
                           help=f"{o.tooltip}: some of {','.join(o.default)} (default all)")
        else:
            conv = float if o.kind == "double" else int
            g.add_argument(o.call, type=ranged(conv, o.lo, o.hi), default=o.default,
                           help=f"{o.tooltip} ({o.lo} to {o.hi}, default {o.default:g})")
    args = ap.parse_args(argv)
    # At 40 MS/s the survey's bank runs half the channels: 8 is not one the filterbank has.
    if args.mode != "channel" and args.channels == 16 and args.survey_rate == 40:
        ap.error("--channels 16 needs --survey-rate 80 (survey and alternate modes)")
    return ap, args


def _num(v):
    return f"{v:g}" if isinstance(v, float) else str(v)


def config_lines():
    """--extcap-config: the capture options as Wireshark's dialog shows them."""
    lines = []
    for n, o in enumerate(OPTIONS):
        line = (f"arg {{number={n}}}{{call={o.call}}}{{display={o.display}}}"
                f"{{tooltip={o.tooltip}}}{{type={o.kind}}}")
        if o.kind == "boolflag":
            line += f"{{default={'true' if o.default else 'false'}}}"
        elif o.kind not in ("selector", "multicheck"):
            line += f"{{default={_num(o.default)}}}{{range={o.lo},{o.hi}}}"
        lines.append(line + f"{{group={o.group}}}")
        for value, display in o.values:
            default = value in o.default if o.kind == "multicheck" else value == o.default
            extra = "{enabled=true}" if o.kind == "multicheck" else ""
            lines.append(f"value {{arg={n}}}{{value={value}}}{{display={display}}}"
                         f"{{default={'true' if default else 'false'}}}{extra}")
    return lines


def badges():
    """The badges that run the pcap app, as (serial, device), and the devices with their IDs
    whose strings could not be read (on Linux: no permission without a udev rule)."""
    need_usb()
    found, unreadable = [], []
    for dev in usb.core.find(find_all=True, idVendor=VID, idProduct=PID):
        try:
            product = usb.util.get_string(dev, dev.iProduct)
            serial = usb.util.get_string(dev, dev.iSerialNumber) if dev.iSerialNumber else ""
        except (usb.core.USBError, ValueError):
            unreadable.append(dev)
            continue
        if product == PRODUCT:
            found.append((serial or "", dev))
    return found, unreadable


def find_badge(serial):
    found, unreadable = badges()
    if serial:
        found = [b for b in found if b[0] == serial]
    if len(found) == 1:
        return found[0][1]
    if found:
        raise BadgeError("several badges, choose one with --serial: " +
                         ", ".join(s for s, _ in found))
    hint = (f" ({len(unreadable)} device(s) {VID:04x}:{PID:04x} unreadable: permissions?)"
            if unreadable else "")
    which = f" with serial {serial}" if serial else ""
    raise BadgeError(f"no badge{which} running the pcap app found{hint}")


def claim(dev):
    """Claims the vendor interface (detaching a kernel driver that holds it); returns its
    number and its bulk IN endpoint's address."""
    for intf in dev.get_active_configuration():
        if intf.bInterfaceClass != VENDOR_CLASS:
            continue
        for ep in intf:
            if ep.bEndpointAddress & 0x80 and (ep.bmAttributes & 0x03) == 0x02:
                n = intf.bInterfaceNumber
                try:
                    if dev.is_kernel_driver_active(n):
                        dev.detach_kernel_driver(n)
                except NotImplementedError:  # pragma: no cover (not on Linux)
                    pass
                usb.util.claim_interface(dev, n)
                return n, ep.bEndpointAddress
    raise BadgeError("the badge has no vendor bulk IN interface (not the pcap app?)")


class Badge:
    """A badge's pcap interface: vendor requests (recipient device) and the bulk IN stream."""

    def __init__(self, dev):
        self.dev = dev
        self.intf, self.ep = claim(dev)

    def out(self, req, data=b"", value=0):
        self.dev.ctrl_transfer(CTRL_OUT, req, value, 0, data, CTRL_TIMEOUT_MS)

    def get(self, req, length):
        return bytes(self.dev.ctrl_transfer(CTRL_IN, req, 0, 0, length, CTRL_TIMEOUT_MS))

    def read(self, timeout_ms):
        return bytes(self.dev.read(self.ep, READ_SIZE, timeout_ms))

    def close(self):
        try:
            usb.util.release_interface(self.dev, self.intf)
        except usb.core.USBError:
            pass
        usb.util.dispose_resources(self.dev)


def mhz_hz(mhz):
    return int(round(mhz * 1e6))


def setup_requests(args, now_ns):
    """The vendor requests before PCAP_REQ_START, in order, as (bRequest, wValue, data)."""
    gain = ESDR_GAIN_AUTO if args.gain < 0 else args.gain
    mode = PCAP_MODE_CHANNEL if args.mode == "channel" else PCAP_MODE_SURVEY
    survey_ms, channel_ms = ((args.survey_ms, args.channel_ms) if args.mode == "alternate"
                             else (0, 0))
    decoders = PCAP_DEC_BAD_CRC if args.bad_crc else 0
    for name in args.decoders:
        decoders |= DECODERS[name]
    return [
        (PCAP_REQ_SET_TIME, 0, struct.pack("<Q", now_ns)),
        (ESDR_REQ_SET_FREQ, 0, struct.pack("<Q", mhz_hz(args.freq))),
        (ESDR_REQ_SET_RX_GAIN, gain, b""),
        (ESDR_REQ_SET_CHANNELIZER, 0,
         CHAN_CFG.pack(args.channels, int(round(args.open_db * 10)), *CHAN_MASK_ALL)),
        (ESDR_REQ_SET_GAPLESS, 0,
         GAPLESS_CFG.pack(int(round(args.ratio * 16)), args.pre, args.post, 0, 0,
                          ESDR_GAPLESS_F_WIDE if args.c6_wide else 0, 0, 0)),
        (PCAP_REQ_SET_DECODERS, 0, DECODERS_CFG.pack(decoders, 0, 0)),
        (PCAP_REQ_SET_MODE, 0,
         MODE_CFG.pack(mode, (PCAP_MODE_F_C6_WIDE if args.c6_wide else 0) |
                       (PCAP_MODE_F_HALF if args.survey_rate == 40 else 0), 0,
                       mhz_hz(args.channel_freq), survey_ms, channel_ms)),
    ]


class Blocks:
    """Whole pcapng blocks out of the bulk IN byte stream, by their lengths.

    Reads need not line up with the blocks: a block's tail carries over to the next read.
    Nothing goes out before a section header block: the bytes before the first one (a read
    that starts within a block, the tail of an earlier capture) are skipped. A block whose
    lengths do not hold (a gap in the stream) is dropped, and the stream resynchronises at the
    next block that checks out: a section header (a restarted capture sends a fresh one), or,
    within the section, a packet or statistics block of one of its interfaces, whole as the
    badge writes them (an EPB's captured length its original length; the interface blocks only
    follow a section header).
    """

    def __init__(self):
        self.buf = bytearray()
        self.section = False
        self.interfaces = 0
        self.synced = False
        # Bytes dropped, and the times the stream lost its blocks.
        self.skipped = 0
        self.resyncs = 0

    def _length(self, at):
        """The length of a block that starts at @at and checks out; 0 when more bytes are
        needed to tell, -1 when none starts there."""
        buf = self.buf
        avail = len(buf) - at
        if avail < 12:
            return 0
        btype, blen, word = struct.unpack_from("<III", buf, at)
        if blen < BLOCK_MIN.get(btype, 12) or blen % 4 or blen > BLOCK_MAX:
            return -1
        if btype == PCAPNG_BT_SHB:
            if word != PCAPNG_BYTE_ORDER_MAGIC:
                return -1
        elif not self.section:
            return -1
        elif btype in (PCAPNG_BT_EPB, PCAPNG_BT_ISB):
            if word >= self.interfaces:
                return -1
        elif not self.synced:
            return -1
        if not self.synced and blen > SCAN_MAX:
            return -1
        if btype == PCAPNG_BT_EPB:
            if avail < 28:
                return 0
            caplen, origlen = struct.unpack_from("<II", buf, at + 20)
            if BLOCK_MIN[PCAPNG_BT_EPB] + ((caplen + 3) & ~3) > blen or (
                    not self.synced and caplen != origlen):
                return -1
        if avail < blen:
            return 0
        if struct.unpack_from("<I", buf, at + blen - 4)[0] != blen:
            return -1
        return blen

    def feed(self, data):
        """The whole blocks @data completes, as bytes to copy (b"" for none)."""
        self.buf += data
        out = bytearray()
        at = 0
        while True:
            n = self._length(at)
            if n == 0:
                break
            if n > 0:
                btype = struct.unpack_from("<I", self.buf, at)[0]
                if btype == PCAPNG_BT_SHB:
                    self.section, self.interfaces = True, 0
                elif btype == PCAPNG_BT_IDB:
                    self.interfaces += 1
                self.synced = True
                out += self.buf[at:at + n]
                at += n
                continue
            if self.synced:
                self.synced = False
                self.resyncs += 1
            pattern = _SCAN_START if self.section else _SHB_START
            m = pattern.search(self.buf, at + 1)
            nxt = m.start() if m else max(at + 1, len(self.buf) - 3)
            self.skipped += nxt - at
            at = nxt
        del self.buf[:at]
        return bytes(out)


class Output:
    """Where the stream goes: a binary file, written through and flushed at once."""

    def __init__(self, f):
        self.f = f
        self.bytes = 0
        try:
            self.fd = f.fileno()
        except (AttributeError, OSError, ValueError):
            self.fd = None

    def write(self, data):
        view = memoryview(data)
        while view:
            view = view[self.f.write(view):]
        self.f.flush()
        self.bytes += len(data)

    def gone(self):
        """True when the reader of a pipe or fifo has closed it."""
        if self.fd is None or not hasattr(select, "poll"):
            return False
        p = select.poll()
        p.register(self.fd, 0)
        return any(ev & (select.POLLERR | select.POLLHUP | select.POLLNVAL)
                   for _, ev in p.poll(0))


class Stop:
    """Set by SIGTERM (Wireshark ends an extcap with it) and SIGINT."""

    def __init__(self):
        self.signum = None

    def __call__(self, signum, frame):
        self.signum = signum

    def __bool__(self):
        return self.signum is not None


class Session:
    """One capture: the badge set up and started, its stream copied, the capture stopped."""

    def __init__(self, badge, out):
        self.badge, self.out = badge, out
        self.blocks = Blocks()
        self.stop = Stop()

    def start(self, args):
        for req, value, data in setup_requests(args, time.time_ns()):
            self.badge.out(req, data, value)
        self.badge.out(PCAP_REQ_START)

    def copy(self):
        """Copies the stream until a signal (True) or until its reader is gone (False)."""
        try:
            while not self.stop:
                try:
                    data = self.badge.read(READ_TIMEOUT_MS)
                except usb.core.USBTimeoutError:
                    if self.out.gone():
                        return False
                    continue
                chunk = self.blocks.feed(data)
                if chunk:
                    self.out.write(chunk)
        except BrokenPipeError:
            return False
        return True

    def finish(self, drain):
        """Stops the capture; with @drain, copies what the badge sends after (its last
        statistics) for a moment."""
        self.badge.out(PCAP_REQ_STOP)
        end = time.monotonic() + DRAIN_S
        try:
            while drain and time.monotonic() < end:
                chunk = self.blocks.feed(self.badge.read(DRAIN_TIMEOUT_MS))
                if chunk:
                    self.out.write(chunk)
        except (usb.core.USBTimeoutError, BrokenPipeError):
            pass

    def summary(self):
        """One line on the capture and the badge's counters."""
        line = f"{self.out.bytes} bytes of pcapng"
        if self.blocks.skipped:
            line += f", {self.blocks.skipped} bytes skipped"
        try:
            s = dict(zip(STATS_FIELDS, STATS.unpack(self.badge.get(PCAP_REQ_GET_STATS,
                                                                   STATS.size))))
        except (usb.core.USBError, struct.error):
            return line
        return (line + f"; badge: packets {s['packets_ble']} BLE, {s['packets_wpan']} "
                f"802.15.4, {s['packets_burst']} bursts, dropped {s['dropped_ble']}, "
                f"{s['dropped_wpan']}, {s['dropped_burst']}; {s['bursts']} bursts, "
                f"{s['bursts_lost']} lost, {s['bursts_wide']} wideband; analysed "
                f"{s['generic']}, skipped {s['generic_skipped']} (busy), "
                f"{s['generic_wide']} filling the channel")


def open_output(path):
    """The output: a fifo or file unbuffered (nothing left to flush when its reader is gone),
    stdout for "-"."""
    if path != "-":
        return open(path, "wb", buffering=0)
    if sys.stdout.isatty():
        raise BadgeError("not writing pcapng to a terminal: -w FILE, or a pipe")
    try:
        return open(sys.stdout.fileno(), "wb", buffering=0, closefd=False)
    except (AttributeError, OSError, ValueError):
        return contextlib.nullcontext(sys.stdout.buffer)


def capture(args, serial, path):
    """A whole capture into @path; 0 when it ended as it should (a signal, the reader gone)."""
    with open_output(path) as f:
        out = Output(f)
        badge = Badge(find_badge(serial))
        s = Session(badge, out)
        old = {sig: signal.signal(sig, s.stop) for sig in (signal.SIGTERM, signal.SIGINT)}
        try:
            s.start(args)
            try:
                alive = s.copy()
            except Exception:
                with contextlib.suppress(usb.core.USBError):
                    badge.out(PCAP_REQ_STOP)
                raise
            s.finish(drain=alive)
            if args.write is not None:
                print(f"esdr_pcap.py: {s.summary()}", file=sys.stderr)
        finally:
            for sig, handler in old.items():
                signal.signal(sig, handler if handler is not None else signal.SIG_DFL)
            badge.close()
    return 0


def interface_serial(args):
    name = args.extcap_interface
    if not name or not name.startswith(IFACE_PREFIX):
        raise BadgeError(f"not an interface of this tool: {name!r}")
    return name[len(IFACE_PREFIX):]


def run(ap, args):
    if args.extcap_interfaces:
        found, _ = badges()
        print(f"extcap {{version=1.0}}{{help={HELP_URL}}}")
        for serial, _ in found:
            print(f"interface {{value={IFACE_PREFIX}{serial}}}"
                  f"{{display=ESP-SDR pcap {serial} (WHY2025 badge)}}")
        return 0
    if args.extcap_dlts:
        interface_serial(args)
        print(f"dlt {{number={PCAP_LINKTYPE_USER0}}}{{name=USER0}}{{display=ESP-SDR pcapng}}")
        return 0
    if args.extcap_config:
        interface_serial(args)
        print("\n".join(config_lines()))
        return 0
    if args.capture:
        serial = interface_serial(args)
        if not args.fifo:
            raise BadgeError("--capture needs --fifo")
        return capture(args, args.serial or serial, args.fifo)
    if args.extcap_capture_filter is not None:
        # Wireshark checks a capture filter so: any output marks it invalid.
        if args.extcap_capture_filter.strip():
            print("no capture filters: the badge sends what it decodes (use a display filter)")
        return 0
    if args.list:
        found, unreadable = badges()
        for serial, dev in found:
            print(f"{IFACE_PREFIX}{serial}\tbus {dev.bus} address {dev.address}")
        for dev in unreadable:
            print(f"bus {dev.bus} address {dev.address}: {VID:04x}:{PID:04x} unreadable "
                  "(permissions?)", file=sys.stderr)
        if not found:
            raise BadgeError("no badge running the pcap app found")
        return 0
    if args.write is not None:
        return capture(args, args.serial, args.write)
    ap.error("nothing to do: -w FILE, --list, or Wireshark's --extcap-* arguments")


def main():
    ap, args = parse_args(sys.argv[1:])
    try:
        return run(ap, args)
    except BadgeError as e:
        print(f"esdr_pcap.py: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        # Before the capture's own handler (a fifo nobody opens, the USB scan).
        return 130
    except Exception as e:
        if usb is not None and isinstance(e, usb.core.USBError):
            print(f"esdr_pcap.py: USB: {e}", file=sys.stderr)
            return 1
        raise


if __name__ == "__main__":
    sys.exit(main())
