# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/pcap/tools/esdr_pcap.py (the pcap app's Wireshark extcap) and tools/esdr_burst.lua.

The extcap's interface, DLT and configuration output on a fake pyusb (CI has neither a badge
nor pyusb: the tool imports without it), the vendor requests of a capture against a fake device
that serves a canned pcapng stream, the copy loop's resynchronisation and its stop paths
(SIGTERM, the reader gone). A real stream comes from the firmware's pcapng writer and records
(apps/pcap/src/pcapng.c and records.c, built with the host compiler): a reader written here
from the pcapng specification parses it and, where tshark is installed, tshark reads it (the
burst records through the Lua dissector). The numbers the tool carries are the C headers'."""

import functools
import io
import operator
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import threading
import time
import types

import pytest

from conftest import REPO, load

ep = load("apps/pcap/tools/esdr_pcap.py")
le = load("apps/ubertooth/tools/le_ref.py")

SRC = REPO / "apps/pcap/src"
LUA = REPO / "apps/pcap/tools/esdr_burst.lua"
PROTO = "apps/pcap/src/pcap_proto.h"
ESDR = "apps/osmosdr/src/esdr_proto.h"
PCAPNG = "apps/pcap/src/pcapng.h"
RECORDS = "apps/pcap/src/records.h"


# ---- the headers ----

def header_text(rel):
    """A header without its comments, continued lines joined."""
    text = re.sub(r"/\*.*?\*/", " ", (REPO / rel).read_text(), flags=re.S)
    return text.replace("\\\n", " ")


def constants(*rels):
    """The integer constants of headers: "#define NAME value", enumerators "NAME = value" and
    defines that OR such names together."""
    out = {}
    for rel in rels:
        text = header_text(rel)
        for m in re.finditer(r"^\s*#define\s+(\w+)\s+(0x[0-9a-fA-F]+|\d+)U?\s*$", text, re.M):
            out[m.group(1)] = int(m.group(2), 0)
        for m in re.finditer(r"^\s*(\w+)\s*=\s*(0x[0-9a-fA-F]+|\d+)U?\s*,?\s*$", text, re.M):
            out[m.group(1)] = int(m.group(2), 0)
        for m in re.finditer(r"^\s*#define\s+(\w+)\s+\(([\w\s|]+)\)\s*$", text, re.M):
            names = [n.strip() for n in m.group(2).split("|")]
            out[m.group(1)] = functools.reduce(operator.or_, (out[n] for n in names))
    return out


C = constants(PROTO, ESDR, PCAPNG, RECORDS)
CTYPES = {"uint8_t": "B", "int8_t": "b", "uint16_t": "H", "int16_t": "h", "uint32_t": "I",
          "int32_t": "i", "uint64_t": "Q", "int64_t": "q"}


def c_struct(rel, name):
    """The fields of a packed struct of a header, in order, as (name, struct format); array
    elements as name[k]."""
    m = re.search(r"struct %s \{(.*?)\} __attribute__\(\(packed\)\);" % name, header_text(rel),
                  re.S)
    fields = []
    for decl in m.group(1).split(";"):
        if not decl.strip():
            continue
        ctype, rest = decl.split(None, 1)
        for item in rest.split(","):
            am = re.fullmatch(r"\s*(\w+)\s*\[\s*(\w+)\s*\]\s*", item)
            if am:
                n = C[am.group(2)] if am.group(2) in C else int(am.group(2), 0)
                fields += [(f"{am.group(1)}[{k}]", CTYPES[ctype]) for k in range(n)]
            else:
                fields.append((item.strip(), CTYPES[ctype]))
    return fields


def layout(rel, name):
    fields = c_struct(rel, name)
    return struct.Struct("<" + "".join(f for _, f in fields)), [n for n, _ in fields]


def unpack_named(rel, name, data):
    st, names = layout(rel, name)
    return dict(zip(names, st.unpack(data)))


def pack_named(rel, name, values):
    st, names = layout(rel, name)
    assert set(values) <= set(names)
    return st.pack(*(values.get(n, 0) for n in names))


def expand(fmt):
    """A struct format with its counts written out: "<3I" is "III"."""
    return "".join(c * int(n or 1) for n, c in re.findall(r"(\d*)([a-zA-Z])", fmt.lstrip("<")))


REQS = {C[n]: n for n in ("PCAP_REQ_START", "PCAP_REQ_STOP", "PCAP_REQ_SET_TIME",
                          "PCAP_REQ_SET_DECODERS", "PCAP_REQ_GET_STATS", "PCAP_REQ_SET_MODE",
                          "ESDR_REQ_SET_FREQ", "ESDR_REQ_SET_RX_GAIN", "ESDR_REQ_SET_GAPLESS",
                          "ESDR_REQ_SET_CHANNELIZER")}
SETUP = ["PCAP_REQ_SET_TIME", "ESDR_REQ_SET_FREQ", "ESDR_REQ_SET_RX_GAIN",
         "ESDR_REQ_SET_CHANNELIZER", "ESDR_REQ_SET_GAPLESS", "PCAP_REQ_SET_DECODERS",
         "PCAP_REQ_SET_MODE", "PCAP_REQ_START"]


def test_numbers_match_the_headers():
    names = [n for n in vars(ep) if re.fullmatch(r"(ESDR|PCAP|PCAPNG)_[A-Z0-9_]+", n) and
             isinstance(getattr(ep, n), int)]
    assert len(names) > 25
    for n in names:
        assert getattr(ep, n) == C[n], n
    # The decoders on by default are the header's default.
    assert functools.reduce(operator.or_, ep.DECODERS.values()) == C["PCAP_DEC_DEFAULT"]
    assert C["PCAP_LINKTYPE_USER0"] == ep.PCAP_LINKTYPE_USER0


@pytest.mark.parametrize("rel, name, attr", [
    (ESDR, "esdr_gapless_cfg", "GAPLESS_CFG"),
    (ESDR, "esdr_chan_cfg", "CHAN_CFG"),
    (PROTO, "pcap_mode_cfg", "MODE_CFG"),
    (PROTO, "pcap_decoders", "DECODERS_CFG"),
    (PROTO, "pcap_stats", "STATS"),
])
def test_struct_layouts(rel, name, attr):
    st, names = layout(rel, name)
    assert expand(getattr(ep, attr).format) == expand(st.format)
    if attr == "STATS":
        assert len(ep.STATS_FIELDS) == len(names)
        # The tool's names are the header's, the arrays' elements named by interface.
        ifs = {"[0]": "_ble", "[1]": "_wpan", "[2]": "_burst"}
        assert ep.STATS_FIELDS == [re.sub(r"\[\d\]", lambda m: ifs[m.group(0)], n)
                                   for n in names]


# ---- pcapng: blocks built here, and a reader from the specification ----

TS = 1761512345123456789


def block(btype, body):
    assert len(body) % 4 == 0
    return struct.pack("<II", btype, len(body) + 12) + body + struct.pack("<I", len(body) + 12)


def shb():
    return block(0x0A0D0D0A, struct.pack("<IHHq", 0x1A2B3C4D, 1, 0, -1))


def idb(linktype):
    return block(1, struct.pack("<HHI", linktype, 0, 65535))


def epb(ifid, ts, data):
    return block(6, struct.pack("<IIIII", ifid, ts >> 32, ts & 0xFFFFFFFF, len(data), len(data)) +
                 data + bytes(-len(data) % 4))


def isb(ifid, ts):
    return block(5, struct.pack("<III", ifid, ts >> 32, ts & 0xFFFFFFFF))


def canned(n=5, ts=TS):
    """A capture's head (SHB and the three IDBs) and its blocks: packets on each interface,
    a statistics block."""
    head = shb() + idb(256) + idb(283) + idb(147)
    return head, [epb(k % 3, ts + k, bytes(range(k, 7 + 2 * k))) for k in range(n)] + [isb(2, ts)]


def split(data, sizes):
    """@data in chunks of the @sizes, over and over."""
    out, at, k = [], 0, 0
    while at < len(data):
        out.append(data[at:at + sizes[k % len(sizes)]])
        at += sizes[k % len(sizes)]
        k += 1
    return out


def options(raw):
    """A block's options as {code: [values]}: a list that is there ends with opt_endofopt."""
    opts, at = {}, 0
    while at < len(raw):
        code, length = struct.unpack_from("<HH", raw, at)
        if code == 0:
            assert length == 0 and at + 4 == len(raw)
            return opts
        padded = (length + 3) & ~3
        assert raw[at + 4 + length:at + 4 + padded] == bytes(padded - length)
        opts.setdefault(code, []).append(raw[at + 4:at + 4 + length])
        at += 4 + padded
    assert not raw, "options without opt_endofopt"
    return opts


def read_pcapng(data):
    """The blocks of a little-endian pcapng stream as dictionaries."""
    out, at, nif = [], 0, 0
    while at < len(data):
        btype, blen = struct.unpack_from("<II", data, at)
        assert blen >= 12 and blen % 4 == 0 and at + blen <= len(data)
        assert struct.unpack_from("<I", data, at + blen - 4)[0] == blen
        body = data[at + 8:at + blen - 4]
        if btype == 0x0A0D0D0A:
            bom, major, minor, seclen = struct.unpack_from("<IHHq", body)
            assert bom == 0x1A2B3C4D
            b = {"type": "SHB", "version": (major, minor), "section_length": seclen,
                 "options": options(body[16:])}
            nif = 0
        elif btype == 1:
            linktype, reserved, snaplen = struct.unpack_from("<HHI", body)
            assert reserved == 0
            b = {"type": "IDB", "linktype": linktype, "snaplen": snaplen,
                 "options": options(body[8:])}
            nif += 1
        elif btype == 6:
            ifid, hi, lo, caplen, origlen = struct.unpack_from("<5I", body)
            padded = (caplen + 3) & ~3
            assert ifid < nif and caplen <= origlen
            assert body[20 + caplen:20 + padded] == bytes(padded - caplen)
            b = {"type": "EPB", "interface": ifid, "timestamp": hi << 32 | lo,
                 "data": body[20:20 + caplen], "options": options(body[20 + padded:])}
        elif btype == 5:
            ifid, hi, lo = struct.unpack_from("<3I", body)
            assert ifid < nif
            b = {"type": "ISB", "interface": ifid, "timestamp": hi << 32 | lo,
                 "options": options(body[12:])}
        else:
            raise AssertionError(f"block type {btype:#x}")
        out.append(b)
        at += blen
    return out


def test_canned_blocks_read_back():
    head, pkts = canned()
    blocks = read_pcapng(head + b"".join(pkts))
    assert [b["type"] for b in blocks] == ["SHB", "IDB", "IDB", "IDB"] + ["EPB"] * 5 + ["ISB"]
    assert blocks[5]["data"] == bytes(range(1, 9)) and blocks[5]["timestamp"] == TS + 1


# ---- a fake pyusb and badge ----

class FakeUSBError(OSError):
    pass


class FakeTimeout(FakeUSBError):
    pass


class FakeIntf(list):
    def __init__(self, cls, number, eps):
        super().__init__(types.SimpleNamespace(bEndpointAddress=a, bmAttributes=t) for a, t in eps)
        self.bInterfaceClass, self.bInterfaceNumber = cls, number


class FakeDev:
    """A badge as pyusb shows it (CDC-ACM, DFU runtime and the vendor interface). It logs the
    control transfers and the reads, and serves @stream on bulk reads: bytes are returned,
    exceptions raised, callables called (what they return is returned, a timeout when None)."""

    idVendor, idProduct, iProduct, iSerialNumber = 0x2FE3, 0x0005, 2, 3
    EP = 0x83

    def __init__(self, serial="0123ABCD", product="ESP-SDR pcap", stream=(), unreadable=False,
                 bus=1, address=9):
        self.strings = {2: product, 3: serial}
        self.unreadable, self.bus, self.address = unreadable, bus, address
        self.stream = list(stream)
        self.log = []
        self.claimed, self.released, self.detached = [], [], []
        self.kernel_driver = False
        self.stats = bytes(ep.STATS.size)
        self.timeouts = 0

    def get_active_configuration(self):
        return [FakeIntf(0x02, 0, [(0x81, 0x03)]), FakeIntf(0x0A, 1, [(0x82, 0x02), (0x02, 0x02)]),
                FakeIntf(0xFE, 2, []), FakeIntf(0xFF, 3, [(self.EP, 0x02)])]

    def is_kernel_driver_active(self, n):
        return self.kernel_driver

    def detach_kernel_driver(self, n):
        self.detached.append(n)

    def ctrl_transfer(self, rtype, req, value=0, index=0, data=None, timeout=None):
        assert index == 0
        if rtype == 0xC0:
            self.log.append(("in", req, value, data))
            if self.stats is None:
                raise FakeUSBError("Pipe error")
            return self.stats[:data]
        assert rtype == 0x40
        self.log.append(("out", req, value, bytes(data)))
        return len(data)

    def read(self, endpoint, size, timeout):
        assert endpoint == self.EP and size == 16384
        self.log.append(("read", timeout))
        if self.stream:
            item = self.stream.pop(0)
            if isinstance(item, BaseException):
                raise item
            if callable(item):
                item = item()
            if item is not None:
                return item
        self.timeouts += 1
        assert self.timeouts < 100, "the capture did not stop"
        raise FakeTimeout("Operation timed out")

    def requests(self):
        """The OUT requests by name, with wValue and data."""
        return [(REQS[req], value, data) for kind, req, value, data in
                (e for e in self.log if e[0] == "out")]


def fake_usb(*devices):
    def find(find_all=False, idVendor=None, idProduct=None):
        assert find_all
        return [d for d in devices if (d.idVendor, d.idProduct) == (idVendor, idProduct)]

    def get_string(dev, index):
        if dev.unreadable:
            raise ValueError("The device has no langid (permission issue?)")
        return dev.strings[index]

    return types.SimpleNamespace(
        core=types.SimpleNamespace(find=find, USBError=FakeUSBError, USBTimeoutError=FakeTimeout),
        util=types.SimpleNamespace(get_string=get_string,
                                   claim_interface=lambda d, n: d.claimed.append(n),
                                   release_interface=lambda d, n: d.released.append(n),
                                   dispose_resources=lambda d: None))


@pytest.fixture
def handlers(monkeypatch):
    """The signal handlers the tool installs, kept here instead."""
    installed = {}

    def fake_signal(sig, handler):
        old = installed.get(sig, signal.SIG_DFL)
        installed[sig] = handler
        return old

    monkeypatch.setattr(ep.signal, "signal", fake_signal)
    return installed


def sigterm(handlers):
    """A stream item: Wireshark's SIGTERM, delivered to the handler the tool installed."""
    return lambda: handlers[signal.SIGTERM](signal.SIGTERM, None)


def test_imports_and_explains_without_pyusb(monkeypatch, run_main, capsys):
    for name in ("usb", "usb.core", "usb.util"):
        monkeypatch.setitem(sys.modules, name, None)
    mod = load("apps/pcap/tools/esdr_pcap.py", "esdr_pcap_nousb")
    assert mod.usb is None
    for args in (["--list"], ["--extcap-interfaces"], ["-w", os.devnull]):
        assert run_main(mod, *args) == 1
        err = capsys.readouterr().err
        assert err.count("\n") == 1 and "pyusb is needed" in err
    # What needs no badge works without it.
    assert run_main(mod, "--extcap-config", "--extcap-interface", "esdr-pcap-1") == 0
    assert "{call=--mode}" in capsys.readouterr().out


def test_extcap_interfaces_and_list(monkeypatch, run_main, capsys):
    other_vid = FakeDev("Z")
    other_vid.idVendor = 0x1234
    devs = [FakeDev("0123ABCD"), FakeDev("FEED", bus=2, address=3),
            FakeDev("X", product="ESP-SDR osmosdr"), FakeDev("Y", unreadable=True, address=4),
            other_vid]
    monkeypatch.setattr(ep, "usb", fake_usb(*devs))
    assert run_main(ep, "--extcap-interfaces", "--extcap-version=4.6") == 0
    out = capsys.readouterr().out.splitlines()
    assert out == [f"extcap {{version=1.0}}{{help={ep.HELP_URL}}}",
                   "interface {value=esdr-pcap-0123ABCD}"
                   "{display=ESP-SDR pcap 0123ABCD (WHY2025 badge)}",
                   "interface {value=esdr-pcap-FEED}{display=ESP-SDR pcap FEED (WHY2025 badge)}"]
    assert run_main(ep, "--list") == 0
    cap = capsys.readouterr()
    assert cap.out.splitlines() == ["esdr-pcap-0123ABCD\tbus 1 address 9",
                                    "esdr-pcap-FEED\tbus 2 address 3"]
    assert cap.err == "bus 1 address 4: 2fe3:0005 unreadable (permissions?)\n"
    monkeypatch.setattr(ep, "usb", fake_usb())
    assert run_main(ep, "--extcap-interfaces") == 0
    assert len(capsys.readouterr().out.splitlines()) == 1
    assert run_main(ep, "--list") == 1
    assert "no badge" in capsys.readouterr().err


def test_extcap_dlts(run_main, capsys):
    assert run_main(ep, "--extcap-dlts", "--extcap-interface", "esdr-pcap-0123ABCD") == 0
    assert capsys.readouterr().out == "dlt {number=147}{name=USER0}{display=ESP-SDR pcapng}\n"


def extcap_lines(text):
    """Lines of {key=value} items, as (kind, {key: value})."""
    out = []
    for line in text.splitlines():
        kind, rest = line.split(" ", 1)
        items = re.findall(r"\{(\w+)=([^{}]*)\}", rest)
        assert "".join(f"{{{k}={v}}}" for k, v in items) == rest, line
        assert len(dict(items)) == len(items), line
        out.append((kind, dict(items)))
    return out


def test_extcap_config(run_main, capsys):
    assert run_main(ep, "--extcap-config", "--extcap-interface", "esdr-pcap-0123ABCD") == 0
    lines = extcap_lines(capsys.readouterr().out)
    args = [f for k, f in lines if k == "arg"]
    assert [int(a["number"]) for a in args] == list(range(len(args)))
    by_call = {a["call"]: a for a in args}
    assert set(by_call) == {"--mode", "--freq", "--survey-rate", "--channel-freq", "--gain",
                            "--survey-ms", "--channel-ms", "--channels", "--open-db", "--ratio",
                            "--pre", "--post", "--c6-wide", "--decoders", "--bad-crc"}
    for a in args:
        assert {"call", "display", "tooltip", "type", "group"} <= set(a)
        assert a["type"] in ("selector", "double", "integer", "boolflag", "multicheck")

    def number(call):
        a = by_call[call]
        return a["type"], a["default"], a["range"]

    assert number("--freq") == ("double", "2440", "2400,2500")
    assert number("--channel-freq") == ("double", "2402", "2400,2500")
    assert number("--gain") == ("integer", "-1", "-1,79")
    assert number("--open-db") == ("double", "6", "1,40")
    assert number("--ratio") == ("double", "2", "1,64")
    assert number("--pre") == number("--post") == ("integer", "1", "0,4")
    assert by_call["--bad-crc"]["type"] == by_call["--c6-wide"]["type"] == "boolflag"
    assert by_call["--bad-crc"]["default"] == by_call["--c6-wide"]["default"] == "false"
    values = {}
    for k, f in lines:
        if k == "value":
            values.setdefault(int(f["arg"]), []).append(f)

    def choices(call):
        return [(v["value"], v["default"]) for v in values[int(by_call[call]["number"])]]

    assert choices("--mode") == [("survey", "true"), ("channel", "false"),
                                 ("alternate", "false")]
    assert choices("--survey-rate") == [("80", "true"), ("40", "false")]
    assert choices("--channels") == [("16", "false"), ("32", "false"), ("40", "true"),
                                     ("64", "false"), ("80", "false")]
    assert by_call["--decoders"]["type"] == "multicheck"
    assert choices("--decoders") == [(n, "true") for n in ep.DECODERS]
    assert set(values) == {int(by_call[c]["number"]) for c in ("--mode", "--survey-rate",
                                                               "--channels", "--decoders")}

    # A command line of the dialog's defaults means what no options mean.
    cmd = []
    for a in args:
        if a["type"] in ("selector", "multicheck"):
            picked = [v["value"] for v in values[int(a["number"])] if v["default"] == "true"]
            cmd += [a["call"], ",".join(picked)]
        elif a["type"] != "boolflag":
            cmd += [a["call"], a["default"]]
    assert vars(ep.parse_args(cmd)[1]) == vars(ep.parse_args([])[1])


def test_options(capsys):
    a = ep.parse_args(["--decoders", "ble, comments", "--channels", "80", "--gain", "-1"])[1]
    assert (a.decoders, a.channels, a.gain) == (["ble", "comments"], 80, -1)
    assert ep.parse_args(["--decoders", ""])[1].decoders == []
    assert ep.parse_args(["--channels", "16"])[1].survey_rate == 80
    # The channel mode alone runs no survey: its rate does not matter.
    assert ep.parse_args(["--mode", "channel", "--channels", "16", "--survey-rate", "40"])[1]
    for bad in (["--freq", "2300"], ["--gain", "80"], ["--decoders", "ble,zigbee"],
                ["--channels", "50"], ["--mode", "scan"], ["--pre", "5"], ["--fre", "2440"],
                ["--survey-rate", "20"], ["--channels", "16", "--survey-rate", "40"],
                ["--mode", "alternate", "--channels", "16", "--survey-rate", "40"]):
        with pytest.raises(SystemExit) as e:
            ep.parse_args(bad)
        assert e.value.code == 2, bad
    capsys.readouterr()


def test_capture_filter_check(run_main, capsys):
    # Wireshark asks so; any output marks the filter invalid.
    base = ["--extcap-interface", "esdr-pcap-0123ABCD", "--extcap-capture-filter"]
    assert run_main(ep, *base, "") == 0
    assert capsys.readouterr().out == ""
    assert run_main(ep, *base, "tcp port 80") == 0
    assert capsys.readouterr().out.count("\n") == 1


# ---- captures against the fake badge ----

def test_capture_requests(monkeypatch, run_main, handlers, tmp_path, capsys):
    """Standalone, every option away from its default; SIGTERM ends it, the badge's last
    statistics after STOP are copied too, then a summary."""
    head, pkts = canned()
    stream = head + b"".join(pkts)
    final = isb(0, TS + 99)
    junk = pkts[0][3:]  # the tail of an earlier capture
    dev = FakeDev(stream=split(junk + stream, [5, 100, 3, 64, 1000]) +
                  [sigterm(handlers), final[:10], final[10:]])
    dev.kernel_driver = True
    dev.stats = pack_named(PROTO, "pcap_stats", {
        "packets[0]": 3, "packets[1]": 1, "packets[2]": 2, "dropped[2]": 4, "bursts": 9,
        "bursts_lost": 1, "bursts_wide": 2, "generic": 5, "generic_skipped": 6,
        "generic_wide": 7})
    other = FakeDev("OTHER", product="ESP-SDR osmosdr")
    monkeypatch.setattr(ep, "usb", fake_usb(other, dev))
    out = tmp_path / "x.pcapng"
    t0 = time.time_ns()
    assert run_main(ep, "-w", out, "--mode", "alternate", "--freq", 2450, "--channel-freq", 2426,
                    "--gain", -1, "--survey-ms", 300, "--channel-ms", 200, "--channels", 80,
                    "--open-db", 8.5, "--decoders", "ble,wpan", "--bad-crc", "--c6-wide",
                    "--ratio", 2.5, "--pre", 2, "--post", 0, "--survey-rate", 40) == 0
    t1 = time.time_ns()
    assert out.read_bytes() == stream + final

    reqs = dev.requests()
    assert [r[0] for r in reqs] == SETUP + ["PCAP_REQ_STOP"]
    req = {name: (value, data) for name, value, data in reqs}
    assert t0 <= struct.unpack("<Q", req["PCAP_REQ_SET_TIME"][1])[0] <= t1
    assert req["ESDR_REQ_SET_FREQ"] == (0, struct.pack("<Q", 2_450_000_000))
    assert req["ESDR_REQ_SET_RX_GAIN"] == (C["ESDR_GAIN_AUTO"], b"")
    assert unpack_named(ESDR, "esdr_chan_cfg", req["ESDR_REQ_SET_CHANNELIZER"][1]) == {
        "channels": 80, "open_db_x10": 85, "mask[0]": 0xFFFFFFFF, "mask[1]": 0xFFFFFFFF,
        "mask[2]": 0xFFFFFFFF}
    assert unpack_named(ESDR, "esdr_gapless_cfg", req["ESDR_REQ_SET_GAPLESS"][1]) == {
        "ratio_x16": 40, "pre": 2, "post": 0, "level": 0, "budget_bps": 0,
        "flags": C["ESDR_GAPLESS_F_WIDE"], "headroom_lo": 0, "headroom_hi": 0}
    assert unpack_named(PROTO, "pcap_decoders", req["PCAP_REQ_SET_DECODERS"][1]) == {
        "flags": C["PCAP_DEC_BLE"] | C["PCAP_DEC_WPAN"] | C["PCAP_DEC_BAD_CRC"],
        "min_burst_us": 0, "max_bits": 0}
    assert unpack_named(PROTO, "pcap_mode_cfg", req["PCAP_REQ_SET_MODE"][1]) == {
        "mode": C["PCAP_MODE_SURVEY"], "flags": C["PCAP_MODE_F_C6_WIDE"] | C["PCAP_MODE_F_HALF"],
        "channel_rate_hz": 0,
        "channel_freq_hz": 2_426_000_000, "survey_ms": 300, "channel_ms": 200}
    assert req["PCAP_REQ_START"] == req["PCAP_REQ_STOP"] == (0, b"")
    assert all(value == 0 for name, value, _ in reqs if name != "ESDR_REQ_SET_RX_GAIN")

    # After STOP: reads until the first timeout (the last statistics), then the counters.
    stop = dev.log.index(("out", C["PCAP_REQ_STOP"], 0, b""))
    assert dev.log[stop + 1:] == [("read", ep.DRAIN_TIMEOUT_MS)] * 3 + [
        ("in", C["PCAP_REQ_GET_STATS"], 0, ep.STATS.size)]
    assert all(e == ("read", ep.READ_TIMEOUT_MS) for e in dev.log[:stop] if e[0] == "read")
    assert (dev.claimed, dev.released, dev.detached) == ([3], [3], [3])
    assert other.log == [] and other.claimed == []
    assert handlers == {signal.SIGTERM: signal.SIG_DFL, signal.SIGINT: signal.SIG_DFL}
    err = capsys.readouterr().err
    assert err.startswith(f"esdr_pcap.py: {len(stream + final)} bytes of pcapng, "
                          f"{len(junk)} bytes skipped; badge: ")
    assert "packets 3 BLE, 1 802.15.4, 2 bursts, dropped 0, 0, 4; 9 bursts, 1 lost, " \
           "2 wideband; analysed 5, skipped 6 (busy), 7 filling the channel" in err


@pytest.mark.parametrize("mode", ["survey", "channel"])
def test_extcap_capture(mode, monkeypatch, run_main, handlers, tmp_path, capsys):
    """The way Wireshark runs it, with the defaults; the interface picks the badge."""
    head, pkts = canned()
    stream = head + b"".join(pkts)
    dev = FakeDev(stream=[stream, sigterm(handlers)])
    other = FakeDev("FEED")
    monkeypatch.setattr(ep, "usb", fake_usb(other, dev))
    fifo = tmp_path / "fifo"  # a file in place of Wireshark's fifo
    args = ["--capture", "--extcap-interface", "esdr-pcap-0123ABCD", "--fifo", fifo]
    assert run_main(ep, *args, *(["--mode", mode] if mode != "survey" else [])) == 0
    assert fifo.read_bytes() == stream
    # Wireshark shows an extcap's stderr: nothing there.
    assert capsys.readouterr().err == ""
    assert other.log == []
    reqs = dev.requests()
    assert [r[0] for r in reqs] == SETUP + ["PCAP_REQ_STOP"]
    req = {name: (value, data) for name, value, data in reqs}
    assert req["ESDR_REQ_SET_FREQ"][1] == struct.pack("<Q", 2_440_000_000)
    assert req["ESDR_REQ_SET_RX_GAIN"] == (0xFFFF, b"")
    chan = unpack_named(ESDR, "esdr_chan_cfg", req["ESDR_REQ_SET_CHANNELIZER"][1])
    assert (chan["channels"], chan["open_db_x10"]) == (40, 60)
    gap = unpack_named(ESDR, "esdr_gapless_cfg", req["ESDR_REQ_SET_GAPLESS"][1])
    assert (gap["ratio_x16"], gap["pre"], gap["post"], gap["flags"]) == (32, 1, 1, 0)
    dec = unpack_named(PROTO, "pcap_decoders", req["PCAP_REQ_SET_DECODERS"][1])
    assert dec["flags"] == C["PCAP_DEC_DEFAULT"]
    assert unpack_named(PROTO, "pcap_mode_cfg", req["PCAP_REQ_SET_MODE"][1]) == {
        "mode": C["PCAP_MODE_SURVEY" if mode == "survey" else "PCAP_MODE_CHANNEL"], "flags": 0,
        "channel_rate_hz": 0, "channel_freq_hz": 2_402_000_000, "survey_ms": 0,
        "channel_ms": 0}


def fifo_reader(path, nbytes):
    """Wireshark's end of the fifo: reads @nbytes, then closes it."""
    got = []

    def read():
        with open(path, "rb") as f:
            got.append(f.read(nbytes))

    t = threading.Thread(target=read)
    t.start()
    return t, got


@pytest.mark.parametrize("how", ["poll", "write"])
def test_stop_when_the_reader_goes(how, monkeypatch, run_main, handlers, tmp_path):
    """The fifo's reader closes it: seen while the badge sends nothing (poll) or as the next
    write fails (a broken pipe); STOP then, nothing read after it."""
    head, pkts = canned()
    fifo = tmp_path / "fifo"
    os.mkfifo(fifo)
    t, got = fifo_reader(fifo, len(head))
    after = t.join if how == "poll" else (lambda: (t.join(), pkts[0])[1])
    dev = FakeDev(stream=[head, after])
    monkeypatch.setattr(ep, "usb", fake_usb(dev))
    assert run_main(ep, "--capture", "--extcap-interface", "esdr-pcap-0123ABCD",
                    "--fifo", fifo) == 0
    t.join()
    assert got == [head]
    assert [r[0] for r in dev.requests()] == SETUP + ["PCAP_REQ_STOP"]
    assert dev.log[-1] == ("out", C["PCAP_REQ_STOP"], 0, b"")
    assert dev.released == [3]


def test_output_sees_its_reader_go():
    r, w = os.pipe()
    with open(w, "wb", buffering=0) as f:
        out = ep.Output(f)
        out.write(b"abc")
        assert not out.gone() and out.bytes == 3
        os.close(r)
        assert out.gone()
        with pytest.raises(BrokenPipeError):
            out.write(b"x")
    assert not ep.Output(io.BytesIO()).gone()


def test_stdout(monkeypatch, run_main, handlers, capsys):
    head, pkts = canned()
    stream = head + b"".join(pkts)
    dev = FakeDev(stream=[stream, sigterm(handlers)])
    dev.stats = None  # GET_STATS fails: the summary goes without the badge's counters
    monkeypatch.setattr(ep, "usb", fake_usb(dev))
    r, w = os.pipe()
    with monkeypatch.context() as m, open(w, "w") as stdout:
        m.setattr(ep.sys, "stdout", stdout)
        assert run_main(ep, "-w", "-") == 0
    assert os.read(r, 65536) == stream
    os.close(r)
    assert capsys.readouterr().err == f"esdr_pcap.py: {len(stream)} bytes of pcapng\n"

    # A stdout without a file descriptor (pytest's, an IDE's): its buffer.
    dev.stream = [stream, sigterm(handlers)]
    stdout = io.TextIOWrapper(io.BytesIO())
    with monkeypatch.context() as m:
        m.setattr(ep.sys, "stdout", stdout)
        assert run_main(ep, "-w", "-") == 0
    assert stdout.buffer.getvalue() == stream

    class Tty(io.StringIO):
        def isatty(self):
            return True

    monkeypatch.setattr(ep.sys, "stdout", Tty())
    assert run_main(ep, "-w", "-") == 1
    assert "terminal" in capsys.readouterr().err


def test_badge_errors(monkeypatch, run_main, capsys, tmp_path):
    out = tmp_path / "x.pcapng"

    def err(*args):
        assert run_main(ep, *args) == 1
        text = capsys.readouterr().err
        assert text.count("\n") == 1
        return text

    monkeypatch.setattr(ep, "usb", fake_usb())
    assert "no badge running the pcap app found" in err("-w", out)
    monkeypatch.setattr(ep, "usb", fake_usb(FakeDev("A"), FakeDev("B")))
    assert "several badges, choose one with --serial: A, B" in err("-w", out)
    assert "no badge with serial C" in err("-w", out, "--serial", "C")
    monkeypatch.setattr(ep, "usb", fake_usb(FakeDev("Y", unreadable=True)))
    assert "unreadable: permissions?" in err("-w", out)
    plain = FakeDev("A")
    plain.get_active_configuration = lambda: [FakeIntf(0x02, 0, [(0x81, 0x03)]),
                                              FakeIntf(0xFF, 1, [(0x01, 0x02), (0x84, 0x03)])]
    monkeypatch.setattr(ep, "usb", fake_usb(plain))
    assert "no vendor bulk IN interface" in err("-w", out)
    assert "not an interface of this tool" in err("--extcap-dlts", "--extcap-interface", "eth0")
    assert "--fifo" in err("--capture", "--extcap-interface", "esdr-pcap-A")
    with pytest.raises(SystemExit):
        run_main(ep)
    capsys.readouterr()

    def interrupted(**kw):
        raise KeyboardInterrupt

    monkeypatch.setattr(ep, "usb", types.SimpleNamespace(core=types.SimpleNamespace(
        find=interrupted, USBError=FakeUSBError)))
    assert run_main(ep, "--list") == 130
    assert capsys.readouterr().err == ""


def test_usb_error_mid_capture(monkeypatch, run_main, handlers, tmp_path, capsys):
    """The badge gone: STOP tried, the interface let go, one line, exit 1."""
    head, _ = canned()
    dev = FakeDev(stream=[head, FakeUSBError("No such device")])
    usb = fake_usb(dev)

    def release(d, n):
        raise FakeUSBError("No such device")

    usb.util.release_interface = release
    monkeypatch.setattr(ep, "usb", usb)
    out = tmp_path / "x.pcapng"
    assert run_main(ep, "-w", out) == 1
    assert capsys.readouterr().err == "esdr_pcap.py: USB: No such device\n"
    assert out.read_bytes() == head
    assert [r[0] for r in dev.requests()] == SETUP + ["PCAP_REQ_STOP"]
    assert handlers == {signal.SIGTERM: signal.SIG_DFL, signal.SIGINT: signal.SIG_DFL}
    # Anything else is a bug: a traceback.
    dev.stream = [RuntimeError("bug")]
    with pytest.raises(RuntimeError):
        run_main(ep, "-w", out)


# ---- the copy loop's blocks ----

def feed(data, sizes):
    b = ep.Blocks()
    return b, b"".join(b.feed(c) for c in split(data, sizes))


SIZES = ([1 << 20], [1], [3, 7, 64], [500], [13, 512])


def test_blocks_start_at_a_section_header():
    """Bytes before the first SHB are skipped: an earlier capture's tail and an SHB's magic cut
    short just before the real one."""
    head, pkts = canned()
    stream = head + b"".join(pkts)
    junk = pkts[1][5:] + pkts[2] + bytes([0x0A, 0x0D, 0x0D])
    for sizes in SIZES:
        b, out = feed(junk + stream, sizes)
        assert out == stream, sizes
        assert (b.skipped, b.resyncs) == (len(junk), 0)


def test_blocks_after_a_torn_block_and_a_restart():
    """A block cut short, then a restarted capture's fresh SHB: the torn block is dropped, the
    new section copied."""
    head, pkts = canned()
    head2, pkts2 = canned(n=3, ts=TS + 10**9)
    torn = pkts[2][:len(pkts[2]) // 2]
    for sizes in SIZES:
        b, out = feed(head + pkts[0] + pkts[1] + torn + head2 + b"".join(pkts2), sizes)
        assert out == head + pkts[0] + pkts[1] + head2 + b"".join(pkts2), sizes
        assert (b.skipped, b.resyncs) == (len(torn), 1)
    blocks = read_pcapng(out)
    assert [b["type"] for b in blocks].count("SHB") == 2


def test_blocks_gap_within_the_section():
    """Bytes lost inside a section: the next block of the section's interfaces goes on."""
    head, pkts = canned()
    for cut in (4, 9, 13, len(pkts[1]) - 4):
        for sizes in SIZES:
            b, out = feed(head + pkts[0] + pkts[1][:cut] + b"".join(pkts[2:]), sizes)
            assert out == head + pkts[0] + b"".join(pkts[2:]), (cut, sizes)
            assert (b.skipped, b.resyncs) == (cut, 1)


def test_blocks_of_other_kinds():
    """In step, a block of another type is copied; a packet on an interface the section lacks
    is not, nor anything but an SHB before the first SHB."""
    head, pkts = canned()
    custom = block(0x00000BAD, b"abcd")
    b, out = feed(head + custom + pkts[0], [1 << 20])
    assert out == head + custom + pkts[0]
    b, out = feed(head + epb(3, TS, b"xyz!") + pkts[0], [1 << 20])
    assert out == head + pkts[0] and b.resyncs == 1
    b, out = feed(idb(256) + pkts[0] + head, [1 << 20])
    assert out == head and b.skipped == len(idb(256) + pkts[0])
    assert ep.Blocks().feed(b"") == b""
    # A big-endian section (the badge writes none) is not one.
    swapped = block(0x0A0D0D0A, struct.pack(">IHHq", 0x1A2B3C4D, 1, 0, -1))
    b, out = feed(swapped + head, [1 << 20])
    assert out == head and b.skipped == len(swapped)
    # A long block is taken in step, not while resynchronising.
    big = epb(0, TS, bytes(ep.SCAN_MAX))
    b, out = feed(head + big + epb(3, TS, b"xyz!") + big + pkts[0], [1 << 20])
    assert out == head + big + pkts[0] and b.resyncs == 1
    # While resynchronising, only the blocks the badge writes within a section.
    b = ep.Blocks()
    b.feed(head + epb(3, TS, b"xyz!"))
    assert not b.synced
    b.buf = bytearray(custom)
    assert b._length(0) == -1


# ---- a real stream: the firmware's writer and records ----

BLE_PDU = (bytes([0x40, 0x0F]) + bytes([0xC6, 0x55, 0x44, 0x33, 0x22, 0x11]) +
           bytes([0x02, 0x01, 0x06, 0x05, 0x09]) + b"esdr")
BLE_AIR = (struct.pack("<I", 0x8E89BED6) + BLE_PDU +
           bytes(le.crc_wire_bytes(le.crc24(0x555555, BLE_PDU))))


def fcs16(data):
    """The 802.15.4 FCS (ITU-T CRC-16, LSB first)."""
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return crc


# A beacon request: MAC command 7 to the broadcast address and PAN, sequence 42.
WPAN_MPDU = struct.pack("<HBHHB", 0x0803, 42, 0xFFFF, 0xFFFF, 0x07)
WPAN_PSDU = WPAN_MPDU + struct.pack("<H", fcs16(WPAN_MPDU))
BURST = {"version": 1, "hdr_len": 52, "flags": C["PCAP_BF_TRUNCATED"] | C["PCAP_BF_BITS"],
         "freq_hz": 2_426_000_000, "offset_hz": -12300, "bandwidth_hz": 250000,
         "duration_us": 376, "symbol_rate": 100000, "deviation_hz": 50000,
         "modulation": C["PCAP_MOD_2FSK"], "rx_gain": 30, "channel": 12, "channels": 40,
         "power_dbfs_x10": -421, "floor_dbfs_x10": -780, "peak_dbfs_x10": -305, "reserved": 0,
         "sample_rate_hz": 4_000_000, "nbits": 13}
BITS = bytes([0xB5, 0xA8])

C_STREAM = r"""
/* The host test's stream (tests/host/test_esdr_pcap.py): the blocks the pcap app writes. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pcapng.h"
#include "records.h"

static uint8_t buf[8192];

static int ok(int n, const char *what)
{
	if (n < 0) {
		fprintf(stderr, "%s: %d\n", what, n);
		exit(1);
	}
	return n;
}

int main(void)
{
	static const uint8_t ble_air[] = {@BLE_AIR@};
	static const uint8_t psdu[] = {@PSDU@};
	static const uint8_t bits[] = {@BITS@};
	const struct pcapng_idb idb[3] = {
		{.linktype = PCAP_LINKTYPE_BLE_LL_WITH_PHDR, .snaplen = 65535, .name = "ble",
		 .description = "Bluetooth LE", .tsresol = 9},
		{.linktype = PCAP_LINKTYPE_IEEE802_15_4_TAP, .snaplen = 65535, .name = "wpan",
		 .description = "IEEE 802.15.4", .tsresol = 9},
		{.linktype = PCAP_LINKTYPE_USER0, .snaplen = 65535, .name = "bursts",
		 .description = "Burst records", .tsresol = 9},
	};
	struct dec_ble_pkt ble = {.phy = DEC_BLE_PHY_1M, .aa = DEC_BLE_ADV_AA, .crc_checked = true,
				  .crc_ok = true, .len = sizeof(ble_air)};
	struct dec_wpan_pkt wpan = {.len = sizeof(psdu), .fcs_ok = true, .lqi = 200, .channel = 15};
	const struct pcap_burst_hdr h = {@BURST@};
	const struct rec_meta meta = {.freq_hz = 2402000000ULL, .offset_hz = 12300,
				      .power_dbfs_x10 = -421, .floor_dbfs_x10 = -780, .gain = 30,
				      .channel = 21, .channels = 40, .rate_hz = 4000000,
				      .duration_us = 376, .phy = "LE 1M"};
	const uint64_t ts = @TS@ULL;
	char comment[REC_COMMENT_MAX];
	uint8_t rec[512];
	size_t used = 0;
	int n;

	memcpy(ble.air, ble_air, sizeof(ble_air));
	memcpy(wpan.psdu, psdu, sizeof(psdu));
	used += ok(pcapng_shb(buf, sizeof(buf), "WHY2025 badge", "Zephyr", "esp-sdr pcap test"),
		   "shb");
	for (int i = 0; i < 3; i++) {
		used += ok(pcapng_idb(buf + used, sizeof(buf) - used, &idb[i]), "idb");
	}
	ok(rec_comment(comment, sizeof(comment), &meta), "comment");
	n = ok(rec_ble(rec, sizeof(rec), &ble, 0), "rec_ble");
	used += ok(pcapng_epb(buf + used, sizeof(buf) - used, &(struct pcapng_epb){
		.interface = PCAP_IF_BLE, .timestamp = ts, .data = rec, .len = (uint32_t)n,
		.comment = comment, .flags = PCAPNG_EPB_FLAG_INBOUND, .dropcount = UINT64_MAX}),
		"epb ble");
	n = ok(rec_wpan(rec, sizeof(rec), &wpan, 2425000000ULL), "rec_wpan");
	used += ok(pcapng_epb(buf + used, sizeof(buf) - used, &(struct pcapng_epb){
		.interface = PCAP_IF_WPAN, .timestamp = ts + 1000U, .data = rec,
		.len = (uint32_t)n, .dropcount = 2}), "epb wpan");
	n = ok(rec_burst(rec, sizeof(rec), &h, bits), "rec_burst");
	used += ok(pcapng_epb(buf + used, sizeof(buf) - used, &(struct pcapng_epb){
		.interface = PCAP_IF_BURST, .timestamp = ts + 2000U, .data = rec,
		.len = (uint32_t)n, .dropcount = UINT64_MAX}), "epb burst");
	used += ok(pcapng_isb(buf + used, sizeof(buf) - used, &(struct pcapng_isb){
		.interface = PCAP_IF_BURST, .timestamp = ts + 3000U,
		.starttime = ts - 1000000000U, .ifrecv = 1, .ifdrop = 0}), "isb");
	fwrite(buf, 1, used, stdout);
	return 0;
}
"""


def c_bytes(data):
    return ", ".join(f"0x{b:02x}" for b in data)


@pytest.fixture(scope="module")
def c_stream(tmp_path_factory):
    """The stream of C_STREAM, built with the host compiler from the firmware's sources."""
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        pytest.skip("no C compiler")
    d = tmp_path_factory.mktemp("pcapng")
    burst = ", ".join(f".{k} = {v}" for k, v in BURST.items() if k not in ("version", "hdr_len"))
    src = C_STREAM
    for key, value in {"@BLE_AIR@": c_bytes(BLE_AIR), "@PSDU@": c_bytes(WPAN_PSDU),
                       "@BITS@": c_bytes(BITS), "@BURST@": burst, "@TS@": str(TS)}.items():
        src = src.replace(key, value)
    (d / "stream.c").write_text(src)
    subprocess.run([cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(SRC),
                    "-o", str(d / "stream"), str(d / "stream.c"), str(SRC / "pcapng.c"),
                    str(SRC / "records.c")], check=True)
    return subprocess.run([str(d / "stream")], check=True, capture_output=True).stdout


def test_c_stream(c_stream):
    blocks = read_pcapng(c_stream)
    assert [b["type"] for b in blocks] == ["SHB", "IDB", "IDB", "IDB", "EPB", "EPB", "EPB", "ISB"]
    shb_ = blocks[0]
    assert shb_["version"] == (1, 0) and shb_["section_length"] == -1
    assert shb_["options"] == {2: [b"WHY2025 badge"], 3: [b"Zephyr"], 4: [b"esp-sdr pcap test"]}
    idbs = blocks[1:4]
    assert [i["linktype"] for i in idbs] == [C["PCAP_LINKTYPE_BLE_LL_WITH_PHDR"],
                                             C["PCAP_LINKTYPE_IEEE802_15_4_TAP"],
                                             C["PCAP_LINKTYPE_USER0"]]
    assert [i["options"][2] for i in idbs] == [[b"ble"], [b"wpan"], [b"bursts"]]
    assert all(i["options"][9] == [b"\x09"] for i in idbs)  # if_tsresol: nanoseconds
    ble, wpan, burst, stats = blocks[4:]
    assert [b["interface"] for b in (ble, wpan, burst)] == [C["PCAP_IF_BLE"], C["PCAP_IF_WPAN"],
                                                            C["PCAP_IF_BURST"]]
    assert [b["timestamp"] for b in blocks[4:]] == [TS, TS + 1000, TS + 2000, TS + 3000]

    # LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR: the header, then access address, PDU and CRC.
    ch, _, _, offenses, ref_aa, flags = struct.unpack_from("<BbbBIH", ble["data"])
    assert (ch, offenses, ref_aa) == (0, 0, 0x8E89BED6)
    assert flags == (C["REC_BLE_F_DEWHITENED"] | C["REC_BLE_F_REF_AA_VALID"] |
                     C["REC_BLE_F_OFFENSES_VALID"] | C["REC_BLE_F_CRC_CHECKED"] |
                     C["REC_BLE_F_CRC_VALID"])
    assert ble["data"][10:] == BLE_AIR
    assert ble["options"][1][0].startswith(b"esdr: 2402.000 MHz, ch 21/40 at 4.000 MS/s, LE 1M")
    assert ble["options"][2] == [struct.pack("<I", 1)]  # epb_flags: inbound

    # LINKTYPE_IEEE802_15_4_TAP: version, reserved, header length, TLVs, then the PSDU.
    version, _, hlen = struct.unpack_from("<BBH", wpan["data"])
    tlvs, at = {}, 4
    while at < hlen:
        t, n = struct.unpack_from("<HH", wpan["data"], at)
        tlvs[t] = wpan["data"][at + 4:at + 4 + n]
        at += 4 + ((n + 3) & ~3)
    assert (version, at) == (0, hlen)
    assert tlvs == {0: b"\x01", 2: struct.pack("<I", 250000), 3: struct.pack("<HB", 15, 0),
                    10: bytes([200]), 11: struct.pack("<f", 2425000.0)}
    assert wpan["data"][hlen:] == WPAN_PSDU
    assert wpan["options"] == {4: [struct.pack("<Q", 2)]}  # epb_dropcount

    # LINKTYPE_USER0: struct pcap_burst_hdr, then the bits.
    assert unpack_named(PROTO, "pcap_burst_hdr", burst["data"][:52]) == BURST
    assert burst["data"][52:] == BITS and burst["options"] == {}

    start = TS - 10**9
    assert stats["interface"] == C["PCAP_IF_BURST"]
    assert stats["options"] == {2: [struct.pack("<II", start >> 32, start & 0xFFFFFFFF)],
                                4: [struct.pack("<Q", 1)], 5: [struct.pack("<Q", 0)]}

    # The copy loop passes it as it is, however the reads cut it.
    for sizes in SIZES:
        assert feed(c_stream, sizes)[1] == c_stream


def test_lua_dissector_follows_the_header():
    """The Lua's offsets, flag bits and modulation names are pcap_proto.h's (tshark checks what
    it makes of them, below, where it is installed)."""
    lua = LUA.read_text()
    st, _ = layout(PROTO, "pcap_burst_hdr")
    items = {name: (int(off), int(n)) for name, off, n in
             re.findall(r"item\(f\.(\w+), (\d+), (\d+)\)", lua)}
    off = 0
    for name, fmt in c_struct(PROTO, "pcap_burst_hdr"):
        size = struct.calcsize("<" + fmt)
        assert items.pop(re.sub(r"_(hz|us|dbfs_x10)$", "", name)) == (off, size), name
        off += size
    assert items == {} and f"local V1_LEN = {st.size}\n" in lua
    bits = dict(re.findall(r'\{ "(\w+)", "[^"]+", (0x[0-9A-F]{4}),', lua))
    assert {k: int(v, 16) for k, v in bits.items()} == {
        n[len("PCAP_BF_"):].lower(): v for n, v in C.items() if n.startswith("PCAP_BF_")}
    mods = {int(k): v.upper().replace("-", "") for k, v in
            re.findall(r'\[(\d+)\] = "([^"]+)"', lua)}
    assert mods == {v: n[len("PCAP_MOD_"):] for n, v in C.items() if n.startswith("PCAP_MOD_")}
    assert "DissectorTable.get(\"wtap_encap\"):add((wtap_encaps or wtap).USER0, burst)" in lua


TSHARK = shutil.which("tshark")


def tshark(path, home, *args):
    """tshark with the Lua dissector, its personal configuration in @home (not the user's)."""
    env = dict(os.environ, HOME=str(home), XDG_CONFIG_HOME=str(home / "config"))
    res = subprocess.run([TSHARK, "-n", "-r", str(path), "-X", f"lua_script:{LUA}", *args],
                         env=env, capture_output=True, text=True, check=True)
    assert "Lua" not in res.stderr and "rror" not in res.stderr, res.stderr
    return res.stdout


@pytest.mark.skipif(TSHARK is None, reason="no tshark")
def test_c_stream_in_tshark(c_stream, tmp_path):
    rec = read_pcapng(c_stream)[6]["data"]
    # A record of a later version: four bytes more header, the bits after them.
    later = bytes([2, 56]) + rec[2:52] + bytes.fromhex("aabbccdd") + rec[52:]
    path = tmp_path / "stream.pcapng"
    path.write_bytes(c_stream + epb(C["PCAP_IF_BURST"], TS + 4000, later))
    fields = ["frame.interface_id", "frame.time_epoch", "frame.comment", "_ws.col.protocol",
              "_ws.col.info", "btle.access_address", "btle.advertising_header.pdu_type",
              "btle.advertising_address", "btle_rf.channel", "btle_rf.flags.crc_valid",
              "btle.crc.incorrect", "wpan.seq_no", "wpan.dst_pan", "wpan.cmd", "wpan.fcs_ok",
              "wpan-tap.ch_num", "wpan-tap.ch_freq", "wpan-tap.lqi", "esdr_burst.version",
              "esdr_burst.hdr_len", "esdr_burst.flags", "esdr_burst.flags.truncated",
              "esdr_burst.flags.bits", "esdr_burst.flags.cut", "esdr_burst.freq",
              "esdr_burst.offset", "esdr_burst.bandwidth", "esdr_burst.duration",
              "esdr_burst.symbol_rate", "esdr_burst.deviation", "esdr_burst.modulation",
              "esdr_burst.rx_gain", "esdr_burst.channel", "esdr_burst.channels",
              "esdr_burst.power", "esdr_burst.floor", "esdr_burst.peak",
              "esdr_burst.sample_rate", "esdr_burst.nbits", "esdr_burst.bits",
              "esdr_burst.extra"]
    out = tshark(path, tmp_path, "-T", "fields", *(a for f in fields for a in ("-e", f)))
    rows = [dict(zip(fields, line.split("\t"))) for line in out.splitlines()]
    assert len(rows) == 4
    ble, wpan, burst, later = rows

    def epoch(ts):
        return f"{ts // 10**9}.{ts % 10**9:09d}"

    def true(v):
        return v in ("1", "True")

    assert [r["frame.interface_id"] for r in rows] == ["0", "1", "2", "2"]
    assert [r["frame.time_epoch"] for r in rows] == [epoch(TS + d) for d in (0, 1000, 2000, 4000)]
    comment = read_pcapng(c_stream)[4]["options"][1][0].decode()
    assert ble["frame.comment"] == comment
    assert int(ble["btle.access_address"], 16) == 0x8E89BED6
    assert int(ble["btle.advertising_header.pdu_type"], 0) == 0  # ADV_IND
    assert ble["btle.advertising_address"] == "11:22:33:44:55:c6"
    assert ble["btle_rf.channel"] == "0" and true(ble["btle_rf.flags.crc_valid"])
    assert ble["btle.crc.incorrect"] == ""
    assert wpan["wpan.seq_no"] == "42" and int(wpan["wpan.dst_pan"], 16) == 0xFFFF
    assert int(wpan["wpan.cmd"], 0) == 7 and true(wpan["wpan.fcs_ok"])
    assert wpan["wpan-tap.ch_num"] == "15" and float(wpan["wpan-tap.ch_freq"]) == 2425000.0
    assert wpan["wpan-tap.lqi"] == "200"
    for r, version, hdr_len in ((burst, 1, 52), (later, 2, 56)):
        assert r["_ws.col.protocol"] == "ESDR"
        assert r["_ws.col.info"] == "2-FSK 2426.000 MHz 100000 Bd 13 bits 376 us"
        assert (int(r["esdr_burst.version"]), int(r["esdr_burst.hdr_len"])) == (version, hdr_len)
        assert int(r["esdr_burst.flags"], 16) == BURST["flags"]
        assert true(r["esdr_burst.flags.truncated"]) and true(r["esdr_burst.flags.bits"])
        assert not true(r["esdr_burst.flags.cut"])
        for name, key in (("freq", "freq_hz"), ("offset", "offset_hz"),
                          ("bandwidth", "bandwidth_hz"), ("duration", "duration_us"),
                          ("symbol_rate", "symbol_rate"), ("deviation", "deviation_hz"),
                          ("modulation", "modulation"), ("rx_gain", "rx_gain"),
                          ("channel", "channel"), ("channels", "channels"),
                          ("power", "power_dbfs_x10"), ("floor", "floor_dbfs_x10"),
                          ("peak", "peak_dbfs_x10"), ("sample_rate", "sample_rate_hz"),
                          ("nbits", "nbits")):
            assert int(r[f"esdr_burst.{name}"]) == BURST[key], name
        assert r["esdr_burst.bits"].replace(":", "") == BITS.hex()
    assert burst["esdr_burst.extra"] == ""
    assert later["esdr_burst.extra"].replace(":", "") == "aabbccdd"

    tree = tshark(path, tmp_path, "-V", "-Y", "esdr_burst.version == 1")
    for line in ("ESP-SDR burst, 2-FSK at 2426.000 MHz", "Frequency: 2426.000 MHz (2426000000 Hz)",
                 "Offset: -12.300 kHz (-12300 Hz)", "Bandwidth: 250.000 kHz (250000 Hz)",
                 "Duration: 376 us", "Symbol rate: 100000 Bd",
                 "Deviation: 50.000 kHz (50000 Hz)", "Modulation: 2-FSK (3)",
                 "Receive gain: 30", "Channel: 12", "Channels: 40", "Power: -42.1 dBFS",
                 "Floor: -78.0 dBFS", "Peak: -30.5 dBFS", "Sample rate: 4.000 MS/s (4000000 Hz)",
                 "Bit count: 13", "Bits: b5a8"):
        assert re.search(r"^\s*" + re.escape(line) + r"$", tree, re.M), line
    assert re.search(r"= Truncated: (True|Set)$", tree, re.M)
    assert re.search(r"= Cut: (False|Not set)$", tree, re.M)
