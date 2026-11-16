# SPDX-License-Identifier: GPL-3.0-or-later
"""The Pcap app's packets and its capture view, without the display (tests/host/test_pcap_egg.py).

A packet comes from esdr.pcap_feed() as apps/pcap's pcapng payload (pcap_proto.h, records.h):
Bluetooth LE with its LINKTYPE 256 header, IEEE 802.15.4 behind its TAP header (LINKTYPE 283),
or a burst record (LINKTYPE_USER0). Pkt reads what the table shows: the protocol, its type (the
advertising PDU, the frame type, the modulation), the source and destination addresses and a
line of information (a device name, a sequence number, a symbol rate).

The view is the capture filtered (FILTERS), grouped (GROUPS: one row per source, type, protocol
or channel, with its count, last time and loudest packet) and sorted (SORTS), as rows of text
that fit the table's width (columns(), line(), detail())."""

import binascii
import struct

try:
    # The board's dissector in C (cmod/esdr_pcap_dissect.c): the same fields, some 30 times
    # faster (tests/host/test_pcap_egg.py holds the two to each other).
    from esdr import pcap_dissect as _c_dissect
except ImportError:
    _c_dissect = None

IF_BLE, IF_WPAN, IF_BURST = 0, 1, 2
PROTO = ("BLE", "802.15.4", "Burst")
PROTO_SHORT = ("BLE", "154", "Bst")

ADV_AA = 0x8E89BED6
# The advertising channel PDU types (Core 5.x, Vol 6, Part B, 2.3).
ADV_TYPES = ("ADV_IND", "ADV_DIRECT", "ADV_NONCONN", "SCAN_REQ", "SCAN_RSP", "CONNECT_IND",
             "ADV_SCAN", "ADV_EXT", "AUX_CONN_RSP")
# Link layer control opcodes worth a name; the rest show as LL_CTRL 0xNN.
LL_CTRL = {0x00: "CONN_UPDATE", 0x01: "CHAN_MAP", 0x02: "TERMINATE", 0x03: "ENC_REQ",
           0x04: "ENC_RSP", 0x0C: "VERSION", 0x14: "LENGTH_REQ", 0x15: "LENGTH_RSP"}
# A few company identifiers of manufacturer data (Bluetooth SIG assigned numbers).
COMPANIES = {0x004C: "Apple", 0x0006: "Microsoft", 0x0075: "Samsung", 0x00E0: "Google",
             0x0059: "Nordic", 0x02E5: "Espressif", 0x0499: "Ruuvi", 0x0157: "Huami",
             0x038F: "Xiaomi", 0x0087: "Garmin"}
WPAN_TYPES = ("Beacon", "Data", "Ack", "Cmd", "Rsvd", "Multi", "Frag", "Ext")
WPAN_CMDS = {0x01: "AssocReq", 0x02: "AssocRsp", 0x03: "Disassoc", 0x04: "DataReq",
             0x05: "PanConflict", 0x06: "Orphan", 0x07: "BeaconReq", 0x08: "Realign",
             0x09: "GtsReq"}
MODULATIONS = ("?", "CW", "OOK", "2FSK", "4FSK", "MSK", "BPSK", "QPSK", "Wide")
# struct pcap_burst_hdr (pcap_proto.h), packed.
BURST_HDR = "<BBHQiIIIIBBBBhhhHII"
BURST_HDR_LEN = struct.calcsize(BURST_HDR)
BF_SKIPPED = 0x0200
# A record too short for its header: MicroPython's struct raises ValueError, CPython's struct.error.
_SHORT = (IndexError, ValueError, getattr(struct, "error", ValueError))


def _hex(b):
    return "".join("{:02x}".format(x) for x in b)


def _mac(b):
    """A Bluetooth device address, sent least significant byte first."""
    return binascii.hexlify(bytes(reversed(b)), ":").decode()


def _eng(v, unit):
    for div, pre in ((1e6, "M"), (1e3, "k")):
        if abs(v) >= div:
            s = "{:.2f}".format(v / div).rstrip("0").rstrip(".")
            return s + " " + pre + unit
    return "{:.0f} {}".format(v, unit)


def ble_channel(rf):
    """The channel index (0 to 39) of an RF channel (0 at 2402 MHz to 39 at 2480 MHz)."""
    if rf == 0:
        return 37
    if rf == 12:
        return 38
    if rf == 39:
        return 39
    return rf - 1 if rf < 12 else rf - 2


# What the dissector fills in, at the first use of any of them: most records (a busy band's
# bursts) are never shown, filtered, grouped or sorted by these (in Python some 6 ms a packet on
# the S3).
_LAZY = ("channel", "phy", "kind", "src", "dst", "random", "info", "length")


class Pkt:
    """One packet of the capture: what esdr.pcap_feed() gave, and what the table shows of it."""

    def __init__(self, no, t, freq, raw):
        (self.iface, self.at, self.ok, power, floor, self.offset, self.us, self.rec,
         self.comment) = raw
        self.no = no
        self.t = t
        self.freq = freq
        self.power = power / 10
        self.floor = floor / 10

    def __getattr__(self, name):
        if name not in _LAZY:
            raise AttributeError(name)
        self._dissect()
        return getattr(self, name)

    def _dissect(self):
        if _c_dissect is not None:
            (self.channel, self.phy, self.kind, self.src, self.dst, self.random, self.info,
             self.length, ok) = _c_dissect(self.iface, self.rec)
            if ok is not None:
                self.ok = ok
            return
        self.channel = ""
        self.phy = ""
        self.kind = ""
        self.src = ""
        self.dst = ""
        self.random = False
        self.info = ""
        self.length = 0
        try:
            (_ble, _wpan, _burst)[self.iface](self)
        except _SHORT:
            self.kind = "malformed"

    def proto(self):
        return PROTO[self.iface] if self.iface < len(PROTO) else "?"

    def short(self):
        return PROTO_SHORT[self.iface] if self.iface < len(PROTO_SHORT) else "?"


def _ad(data):
    """A device name and the manufacturer from advertising data (AD structures)."""
    name = maker = ""
    k = 0
    while k + 1 < len(data):
        n = data[k]
        if n == 0 or k + 1 + n > len(data):
            break
        t, v = data[k + 1], data[k + 2:k + 1 + n]
        if t in (0x08, 0x09):
            try:
                name = v.decode()
            except UnicodeError:
                name = _hex(v)
        elif t == 0xFF and len(v) >= 2:
            cid = v[0] | v[1] << 8
            maker = COMPANIES.get(cid, "0x{:04x}".format(cid))
        k += 1 + n
    return name, maker


def _ble(p):
    rec = p.rec
    rf, _, _, _, _, flags = struct.unpack_from("<BbbBIH", rec, 0)
    air = rec[10:]
    aa = struct.unpack_from("<I", air, 0)[0]
    p.channel = str(ble_channel(rf))
    p.phy = ("LE 1M", "LE 2M", "LE Coded", "?")[(flags >> 14) & 3]
    if flags & 0x0400:
        p.ok = bool(flags & 0x0800)
    h0, n = air[4], air[5]
    pl = air[6:6 + n]
    p.length = n + 2
    if aa != ADV_AA:
        _ble_data(p, aa, h0, n, pl)
        return
    t = h0 & 0x0F
    p.kind = ADV_TYPES[t] if t < len(ADV_TYPES) else "ADV 0x{:x}".format(t)
    p.random = bool(h0 & 0x40)
    if t in (0, 2, 4, 6) and len(pl) >= 6:
        p.src = _mac(pl[0:6])
        name, maker = _ad(pl[6:])
        p.info = " ".join(x for x in (name and '"' + name + '"', maker) if x)
    elif t == 1 and len(pl) >= 12:
        p.src, p.dst = _mac(pl[0:6]), _mac(pl[6:12])
    elif t == 3 and len(pl) >= 12:
        p.src, p.dst = _mac(pl[0:6]), _mac(pl[6:12])
    elif t == 5 and len(pl) >= 34:
        p.src, p.dst = _mac(pl[0:6]), _mac(pl[6:12])
        caa = struct.unpack_from("<I", pl, 12)[0]
        interval = struct.unpack_from("<H", pl, 22)[0] * 1.25
        p.info = "AA {:08x} {:g} ms".format(caa, interval)
    elif t == 7 and len(pl) >= 2:
        _ble_ext(p, pl)


def _ble_ext(p, pl):
    """ADV_EXT_IND: the extended header's address and auxiliary pointer."""
    hl = pl[0] & 0x3F
    if hl == 0 or len(pl) < 1 + hl:
        return
    fl = pl[1]
    k = 2
    if fl & 0x01:
        p.src = _mac(pl[k:k + 6])
        k += 6
    if fl & 0x02:
        p.dst = _mac(pl[k:k + 6])
        k += 6
    if fl & 0x04:
        k += 1
    if fl & 0x08:
        k += 2
    if fl & 0x10 and k + 3 <= 1 + hl:
        p.info = "aux ch {} {}".format(pl[k] & 0x3F, ("1M", "2M", "Coded", "?")[pl[k + 2] >> 5 & 3])


def _ble_data(p, aa, h0, n, pl):
    """A data channel PDU of the connection with access address aa."""
    llid = h0 & 3
    p.src = "AA {:08x}".format(aa)
    if llid == 3 and n:
        p.kind = "LL_" + LL_CTRL.get(pl[0], "CTRL 0x{:02x}".format(pl[0]))
    elif n == 0:
        p.kind = "Empty"
    else:
        p.kind = "L2CAP" if llid == 2 else "L2CAP+"
    p.info = "SN {} NESN {}{}".format(h0 >> 3 & 1, h0 >> 2 & 1, " MD" if h0 & 0x10 else "")


def _wpan_addr(psdu, k, mode):
    if mode == 2:
        return "{:04x}".format(psdu[k] | psdu[k + 1] << 8), k + 2
    if mode == 3:
        return _hex(bytes(psdu[k + j] for j in range(7, -1, -1))), k + 8
    return "", k


def _wpan(p):
    rec = p.rec
    hlen = struct.unpack_from("<H", rec, 2)[0]
    k = 4
    while k + 4 <= hlen:
        t, n = struct.unpack_from("<HH", rec, k)
        if t == 3 and n >= 2:
            p.channel = str(struct.unpack_from("<H", rec, k + 4)[0])
        k += 4 + (n + 3) // 4 * 4
    psdu = rec[hlen:]
    p.phy = "O-QPSK"
    p.length = len(psdu)
    fcf = psdu[0] | psdu[1] << 8
    ft = fcf & 7
    p.kind = WPAN_TYPES[ft]
    ver = fcf >> 12 & 3
    dmode, smode = fcf >> 10 & 3, fcf >> 14 & 3
    k = 2
    seq = None
    if not (ver == 2 and fcf & 0x100):
        seq = psdu[2]
        k = 3
    dpan = span = ""
    if dmode:
        dpan = "{:04x}".format(psdu[k] | psdu[k + 1] << 8)
        k += 2
    dst, k = _wpan_addr(psdu, k, dmode)
    if smode:
        if not fcf & 0x40:
            span = "{:04x}".format(psdu[k] | psdu[k + 1] << 8)
            k += 2
        else:
            span = dpan
    src, k = _wpan_addr(psdu, k, smode)
    if src:
        p.src = (span + ":" if span else "") + src
    if dst:
        p.dst = (dpan + ":" if dpan else "") + dst
    if ft == 3 and not fcf & 0x08 and k < len(psdu) - 2:
        p.kind = WPAN_CMDS.get(psdu[k], "Cmd 0x{:02x}".format(psdu[k]))
    bits = []
    if seq is not None:
        bits.append("#{}".format(seq))
    if fcf & 0x20:
        bits.append("ack?")
    if fcf & 0x08:
        bits.append("sec")
    if fcf & 0x10:
        bits.append("pending")
    p.info = " ".join(bits)


def _burst(p):
    h = struct.unpack_from(BURST_HDR, p.rec, 0)
    (_, hlen, flags, freq, offset, bw, us, rate, dev, mod, _, _, _, power, floor, _, _, _,
     nbits) = h
    p.phy = MODULATIONS[mod] if mod < len(MODULATIONS) else "?"
    p.kind = p.phy
    p.length = (nbits + 7) // 8
    if flags & BF_SKIPPED:
        p.kind = "?"
    bits = []
    if rate:
        bits.append(_eng(rate, "Bd"))
        p.src = "{} {}".format(p.kind, _eng(rate, "Bd").replace(" ", ""))
    else:
        p.src = p.kind
    if bw:
        bits.append("bw " + _eng(bw, "Hz"))
    if dev:
        bits.append("dev " + _eng(dev, "Hz"))
    if nbits:
        bits.append("{} bits".format(nbits))
    if flags & BF_SKIPPED:
        bits.append("not analysed")
    p.info = " ".join(bits)


# ---- the view ----

FILTERS = ("Decoded", "All", "BLE", "802.15.4", "Bursts", "Bad CRC", "Source")
GROUPS = ("None", "Source", "Type", "Protocol", "Channel")
SORTS = ("Newest", "Oldest", "Loudest", "Longest", "Count", "Source", "Type")


def keep(p, filt, source=None):
    """Whether packet p passes the filter (source: the address the Source filter keeps)."""
    if filt == "Decoded":
        return p.iface != IF_BURST and p.ok
    if filt == "BLE":
        return p.iface == IF_BLE
    if filt == "802.15.4":
        return p.iface == IF_WPAN
    if filt == "Bursts":
        return p.iface == IF_BURST
    if filt == "Bad CRC":
        return not p.ok
    if filt == "Source":
        return source is None or p.src == source
    return True


def group_key(p, group):
    if group == "Source":
        return p.src or "-"
    if group == "Type":
        return p.short() + " " + p.kind
    if group == "Protocol":
        return p.proto()
    if group == "Channel":
        return p.short() + " " + (p.channel or "-")
    return None


class Group:
    """The packets of one key: how many, when, how loud, of which types."""

    def __init__(self, key):
        self.key = key
        self.count = 0
        self.first = self.last = 0.0
        self.no = 0
        self.power = -999.0
        self.power_sum = 0.0
        self.us = 0
        self.bytes = 0
        self.kinds = {}
        self.protos = {}
        self.latest = None

    def add(self, p):
        if not self.count or p.t < self.first:
            self.first = p.t
        if not self.count or (p.t, p.no) > (self.last, self.no):
            self.last, self.no, self.latest = p.t, p.no, p
        self.count += 1
        self.power = max(self.power, p.power)
        self.power_sum += p.power
        self.us += p.us
        self.bytes += p.length
        k = p.short() + " " + p.kind
        self.kinds[k] = self.kinds.get(k, 0) + 1
        self.protos[p.short()] = 1

    def kind(self):
        """The most frequent type, the first in order among equals."""
        best = None
        for k, n in self.kinds.items():
            if best is None or n > best[1] or (n == best[1] and k < best[0]):
                best = (k, n)
        return best[0] if best else ""

    def src(self):
        return self.latest.src if self.latest is not None else ""

    def short(self):
        return "/".join(sorted(self.protos)) if len(self.protos) > 1 else \
            (self.latest.short() if self.latest is not None else "")


def _keyed_sort(rows, key):
    """rows.sort(key=key) with each key made once: MicroPython's sort calls the key at every
    comparison. Each key ends in something unique (a packet's number, a group's key), so the
    rows themselves are never compared."""
    keyed = [(key(r), r) for r in rows]
    keyed.sort()
    rows[:] = [r for _, r in keyed]


def _sort_packets(rows, sort):
    # By time, then arrival: a record of a burst's numbers can come before a packet decoded
    # from an earlier burst.
    if sort == "Oldest":
        _keyed_sort(rows, lambda p: (p.t, p.no))
    elif sort == "Loudest":
        _keyed_sort(rows, lambda p: (-p.power, -p.no))
    elif sort == "Longest":
        _keyed_sort(rows, lambda p: (-p.us, -p.no))
    elif sort == "Count":
        n = {}
        for p in rows:
            n[p.src] = n.get(p.src, 0) + 1
        _keyed_sort(rows, lambda p: (-n[p.src], p.src, -p.no))
    elif sort == "Source":
        _keyed_sort(rows, lambda p: (p.src == "", p.src, -p.no))
    elif sort == "Type":
        _keyed_sort(rows, lambda p: (p.short(), p.kind, -p.no))
    else:
        _keyed_sort(rows, lambda p: (-p.t, -p.no))


def _sort_groups(rows, sort):
    if sort == "Oldest":
        _keyed_sort(rows, lambda g: (g.first, g.key))
    elif sort == "Loudest":
        _keyed_sort(rows, lambda g: (-g.power, g.key))
    elif sort == "Longest":
        _keyed_sort(rows, lambda g: (-g.us / g.count, g.key))
    elif sort == "Count":
        _keyed_sort(rows, lambda g: (-g.count, -g.last, -g.no))
    elif sort == "Source":
        _keyed_sort(rows, lambda g: (g.src() == "", g.src(), g.key))
    elif sort == "Type":
        _keyed_sort(rows, lambda g: (g.kind(), g.key))
    else:
        _keyed_sort(rows, lambda g: (-g.last, -g.no))


def _filtered(pkts, filt, source):
    # The common filters without a call a packet (2000 of them on a board).
    if filt == "Decoded":
        return [p for p in pkts if p.ok and p.iface != IF_BURST]
    if filt == "All":
        return list(pkts)
    return [p for p in pkts if keep(p, filt, source)]


def view(pkts, filt="Decoded", group="None", sort="Newest", source=None):
    """The rows of the table: packets (pkts in time order), or Groups when grouped, filtered and
    sorted."""
    rows = _filtered(pkts, filt, source)
    if group not in GROUPS[1:]:
        if sort == "Newest":
            rows.reverse()
        elif sort != "Oldest":
            _sort_packets(rows, sort)
        return rows
    groups = {}
    for p in rows:
        k = group_key(p, group)
        g = groups.get(k)
        if g is None:
            g = groups[k] = Group(k)
        g.add(p)
    out = list(groups.values())
    _sort_groups(out, sort)
    return out


def row_source(row):
    """The address a row stands for (the Source filter's): a packet's, a group's newest."""
    if isinstance(row, Group):
        return row.src()
    return row.src


# ---- text ----


def _time(t):
    return "{:7.2f}".format(t) if t < 10000 else "{:7.0f}".format(t)


def _db(v):
    return "{:4.0f}".format(max(-99, min(0, v)))


def _cut(s, w):
    return s[:w] if len(s) > w else s + " " * (w - len(s))


# The table's columns: (title, width, value, right aligned, kept), in the order shown. A
# narrow line keeps the "kept" ones (the protocol goes first: the colour tells it too, then the
# source gives way down to SOURCE_MIN and the type to TYPE_MIN), the others join while they fit
# with INFO_MIN left for the information, which takes the rest of the line.
_PKT_COLS = (
    ("Time", 7, lambda p: _time(p.t), True, True),
    ("dB", 4, lambda p: _db(p.power), True, True),
    ("Pro", 3, lambda p: p.short(), False, True),
    ("Source", 17, lambda p: p.src, False, True),
    ("Type", 11, lambda p: p.kind if p.ok else p.kind + "!", False, True),
    ("Destination", 17, lambda p: p.dst, False, False),
    ("Len", 3, lambda p: "{:3d}".format(p.length), True, False),
    ("Ch", 2, lambda p: "{:>2}".format(p.channel), True, False),
)
_GRP_COLS = (
    ("Count", 5, lambda g: "{:5d}".format(g.count), True, True),
    ("Last", 7, lambda g: _time(g.last), True, True),
    ("dB", 4, lambda g: _db(g.power), True, True),
    ("Pro", 3, lambda g: g.short(), False, True),
    ("Key", 17, lambda g: g.key, False, True),
    ("Type", 11, lambda g: g.kind().split(" ", 1)[-1], False, True),
    ("Bytes", 6, lambda g: "{:6d}".format(g.bytes), True, False),
)
_INFO = ("Info", lambda r: (r.latest.info if r.latest is not None else "")
         if isinstance(r, Group) else r.info)
SOURCE_MIN = 8
TYPE_MIN = 7
INFO_MIN = 10


def _width(cols):
    return sum(c[1] for c in cols) + len(cols) - 1 if cols else 0


def columns(chars, grouped):
    """The columns that fit a line of chars characters: [title, width, value, right aligned];
    the last takes the rest of the line. A source narrower than its address keeps the end of it
    (the last bytes tell devices apart)."""
    spec = _GRP_COLS if grouped else _PKT_COLS
    cols = [[t, w, fn, right] for t, w, fn, right, kept in spec if kept]
    if _width(cols) > chars:
        cols = [c for c in cols if c[0] != "Pro"]
    for name, least in (("Source", SOURCE_MIN), ("Key", SOURCE_MIN), ("Type", TYPE_MIN)):
        over = _width(cols) - chars
        for c in cols:
            if over > 0 and c[0] == name:
                c[1] = max(least, c[1] - over)
    order = [t for t, _, _, _, _ in spec]
    for t, w, fn, right, kept in spec:
        if not kept and _width(cols) + 1 + w + 1 + INFO_MIN <= chars:
            cols.append([t, w, fn, right])
    cols.sort(key=lambda c: order.index(c[0]))
    rest = chars - _width(cols) - 1
    if rest >= 6:
        cols.append([_INFO[0], rest, _INFO[1], False])
    elif cols:
        cols[-1][1] += max(0, chars - _width(cols))
    return cols


def header(cols):
    return " ".join(" " * (w - len(t)) + t if right and len(t) < w else _cut(t, w)
                    for t, w, _, right in cols)


def line(row, cols):
    out = []
    for t, w, fn, _ in cols:
        v = str(fn(row))
        if t in ("Source", "Key", "Destination") and len(v) > w:
            v = v[len(v) - w:]
        out.append(_cut(v, w))
    return " ".join(out)


def _wrap(text, chars, lines):
    """At most lines lines of at most chars characters, broken between words where it can."""
    out = []
    while text and len(out) < lines:
        if len(text) <= chars:
            out.append(text)
            break
        k = text.rfind(" ", 0, chars + 1)
        if k <= 0:
            k = chars
        out.append(text[:k].rstrip())
        text = text[k:].lstrip()
    return out


def detail(row, chars, lines=3):
    """The selected row in full, a few lines of chars characters."""
    if row is None:
        return []
    if isinstance(row, Group):
        kinds = sorted(row.kinds.items(), key=lambda kv: (-kv[1], kv[0]))
        text = "{}: {} packets, {:.1f} to {:.1f} s, max {:.0f} dBFS, mean {:.0f}; {}".format(
            row.key, row.count, row.first, row.last, row.power, row.power_sum / row.count,
            ", ".join("{} x{}".format(k, n) for k, n in kinds[:4]))
        return _wrap(text, chars, lines)
    p = row
    head = "#{} {:.3f} s {} {} ch {} {:.0f} dBFS snr {:.0f} dB {:+.1f} kHz {} us{}".format(
        p.no, p.t, p.proto(), p.phy, p.channel or "-", p.power, p.power - p.floor,
        p.offset / 1000, p.us, "" if p.ok else " BAD CRC")
    who = " ".join(x for x in (p.kind, p.src and "from " + p.src + (" random" if p.random
                                                                     else ""),
                               p.dst and "to " + p.dst, p.info) if x)
    # The packet's own line first, as many lines as it takes while one stays for the rest.
    first = _wrap(head, chars, max(1, lines - 1))
    return (first + _wrap(who, chars, lines - len(first)))[:lines]
