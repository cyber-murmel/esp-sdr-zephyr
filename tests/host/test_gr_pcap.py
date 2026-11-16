# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/pcap/tools/esdr_gr_pcap.py: every channel of a 20 MS/s stream through the Pcap app's
capture core, as pcapng.

A synthetic recording at 2409 MHz holds Bluetooth LE packets (the golden vectors of
tests/unit/ubertooth_le, GFSK modulated here) on advertising channel 37 and on data channel 0,
whose access address only the CONNECT_IND on channel 37 tells, 802.15.4 frames (the PHY's own
modulator) on channels 11, 12 and 13, and a band of noise 12 MHz wide, as Wi-Fi fills many
channels. Each must come out once, on its channel: the data packet through the address the
channels share, no burst record for what a neighbouring channel decoded, one wideband record
for the band. The capture runs on channels cut here with numpy, and, where GNU Radio's Python
is there, the whole tool runs on the recording."""

import ctypes
import io
import shutil
import struct

import numpy as np
import pytest

from conftest import load
from test_ble_pcap import VECS, gfsk
from test_esdr_pcap import C, c_struct, layout, read_pcapng

gp = load("apps/pcap/tools/esdr_gr_pcap.py")

CENTER = 2409000000
RATE = 20000000
CH_RATE = 4000000
START_NS = 1700000000 * 10 ** 9
SIGMA = 0.01

VEC = {v["name"]: v for v in VECS}
# (ms from the start, what, MHz, payload)
EVENTS = [
    (3.0, "ble", 2402, "adv_ind_37"),
    (5.0, "ble", 2402, "connect_req_37"),
    (7.0, "wpan", 2405, 1),
    (10.0, "wpan", 2415, 2),
    (14.0, "wide", 2409, 300),
    (30.0, "ble", 2404, "ll_conn_update_0"),
    (33.0, "wpan", 2410, 3),
]
LENGTH_MS = 40.0


@pytest.fixture(scope="session")
def built(tmp_path_factory):
    """The tool's library (path, ctypes) with the 802.15.4 modulator's signatures added."""
    if shutil.which("cc") is None and shutil.which("gcc") is None:
        pytest.skip("no C compiler")
    path = gp.build_lib(tmp_path_factory.mktemp("grpcap"))
    lib = gp.load_lib(path)
    lib.ieee154_phy_init.restype = None
    lib.ieee154_phy_modulate.restype = ctypes.c_size_t
    lib.ieee154_phy_modulate.argtypes = [ctypes.c_char_p, ctypes.c_uint8, ctypes.c_int16,
                                         ctypes.c_void_p, ctypes.c_size_t]
    lib.ieee154_phy_append_fcs.restype = ctypes.c_size_t
    lib.ieee154_phy_append_fcs.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    # The FCS and the modulator read the tables this builds.
    lib.ieee154_phy_init()
    return path, lib


def wpan_psdu(lib, seq):
    """A data frame (PAN 0xabcd to the broadcast address) with its FCS."""
    payload = bytes([0x41, 0x88, seq, 0xcd, 0xab, 0xff, 0xff, 0x34, 0x12]) + b"esdr gr pcap"
    buf = ctypes.create_string_buffer(payload + bytes(2), len(payload) + 2)
    n = lib.ieee154_phy_append_fcs(buf, len(payload))
    return buf.raw[:n]


def wpan_wave(lib, psdu):
    """The PHY's 4 MS/s waveform, interpolated to the stream's rate."""
    cap = (6 + len(psdu)) * 128 + 4
    out = (ctypes.c_int16 * (2 * cap))()
    n = lib.ieee154_phy_modulate(psdu, len(psdu), 16384, out, cap)
    a = np.frombuffer(out, np.int16)[:2 * n].astype(float) / 16384
    x = a[0::2] + 1j * a[1::2]
    up = np.zeros(5 * len(x), complex)
    up[::5] = x
    k = np.arange(-60, 61)
    return np.convolve(up, np.sinc(k / 5) * np.kaiser(len(k), 8), mode="same")


def recording(lib):
    """The stream as the device's 8-bit items (interleaved int8), and what is in it."""
    rng = np.random.default_rng(7)
    n = int(LENGTH_MS * RATE / 1000)
    x = SIGMA * (rng.standard_normal(n) + 1j * rng.standard_normal(n))
    t = np.arange(n)
    want = []
    for ms, kind, mhz, what in EVENTS:
        at = int(ms * RATE / 1000)
        if kind == "ble":
            s = 0.2 * gfsk(VEC[what]["air"], RATE // 1000000)
            want.append(("ble", mhz, VEC[what]["clear"], ms, ms + len(s) / RATE * 1e3))
        elif kind == "wpan":
            psdu = wpan_psdu(lib, what)
            s = 0.2 * wpan_wave(lib, psdu)
            want.append(("wpan", mhz, psdu, ms, ms + len(s) / RATE * 1e3))
        else:
            m = int(what * RATE / 1e6)
            spec = np.fft.fft(rng.standard_normal(m) + 1j * rng.standard_normal(m))
            spec[np.abs(np.fft.fftfreq(m, 1 / RATE)) > 6e6] = 0
            s = np.fft.ifft(spec)
            s *= 0.15 / np.sqrt(np.mean(np.abs(s) ** 2))
            want.append(("wide", mhz, None, ms, ms + what / 1e3))
        f = (mhz * 1e6 - CENTER) / RATE
        x[at:at + len(s)] += s * np.exp(2j * np.pi * f * t[at:at + len(s)])
    q = np.empty(2 * n, np.int8)
    q[0::2] = np.clip(np.round(x.real * 128), -128, 127)
    q[1::2] = np.clip(np.round(x.imag * 128), -128, 127)
    return q, want


def channelize(q, freqs):
    """The flowgraph's channels in numpy: mix, low-pass, every fifth sample."""
    x = (q[0::2] + 1j * q[1::2]) / 128.0
    # As the tool's filter: cut off 1.3 MHz of 20 MS/s, 60 dB down from 1.6 MHz.
    k = np.arange(-75, 76)
    h = 0.13 * np.sinc(0.13 * k) * np.kaiser(len(k), 5.65)
    t = np.arange(len(x))
    return [np.convolve(x * np.exp(-2j * np.pi * (f - CENTER) / RATE * t), h,
                        mode="same")[::5].astype(np.complex64) for f in freqs]


def records(data):
    """The capture's packets: (interface, timestamp ns, data, comment)."""
    blocks = read_pcapng(data)
    assert [b["linktype"] for b in blocks if b["type"] == "IDB"] == [256, 283, 147]
    return [(b["interface"], b["timestamp"], b["data"],
             b["options"].get(1, [b""])[0].decode())
            for b in blocks if b["type"] == "EPB"]


def tap_channel(d):
    """802.15.4 TAP: the channel TLV's number, and the PSDU after the TLVs."""
    hlen = struct.unpack_from("<H", d, 2)[0]
    at, chan = 4, None
    while at < hlen:
        typ, length = struct.unpack_from("<HH", d, at)
        if typ == 3:
            chan = struct.unpack_from("<H", d, at + 4)[0]
        at += 4 + (length + 3) // 4 * 4
    return chan, d[hlen:]


def check(data, want, freqs):
    recs = records(data)
    ms = [(t - START_NS) / 1e6 for _, t, _, _ in recs]
    ble = sorted((d[0], d[10:]) for i, _, d, _ in recs if i == gp.IF_BLE)
    assert ble == sorted(((mhz - 2402) // 2, clear) for kind, mhz, clear, _, _ in want
                         if kind == "ble")
    for i, _, d, _ in recs:
        if i == gp.IF_BLE:
            assert struct.unpack_from("<H", d, 8)[0] & 0x0c00 == 0x0c00, "CRC checked and good"
    wpan = sorted(tap_channel(d) for i, _, d, _ in recs if i == gp.IF_WPAN)
    assert wpan == sorted((11 + (mhz - 2405) // 5, psdu) for kind, mhz, psdu, _, _ in want
                          if kind == "wpan")

    st, names = layout("apps/pcap/src/pcap_proto.h", "pcap_burst_hdr")
    bursts = [(m, dict(zip(names, st.unpack_from(d))), c)
              for (i, _, d, c), m in zip(recs, ms) if i == gp.IF_BURST]
    wide = [(m, h, c) for m, h, c in bursts if h["modulation"] == C["PCAP_MOD_WIDEBAND"]]
    assert len(wide) == 1, wide
    m, h, c = wide[0]
    lo = (h["freq_hz"] - h["bandwidth_hz"] / 2) / 1e6
    hi = (h["freq_hz"] + h["bandwidth_hz"] / 2) / 1e6
    assert 13.9 <= m <= 14.1 and 2402.5 <= lo <= 2403.5 and 2414.5 <= hi <= 2415.5, (m, lo, hi)
    assert c.startswith(f"esdr: {h['freq_hz'] / 1e6:.3f} MHz, ch {h['channel']}/{len(freqs)} at "
                        "4.000 MS/s") and "wideband in" in c, c
    # No record for a decoded packet's signal, here or in a neighbouring channel.
    for m, h, c in bursts:
        f = (h["freq_hz"] + h["offset_hz"]) / 1e6
        for kind, mhz, _, t0, t1 in want:
            if kind != "wide":
                assert not (t0 - 0.1 <= m <= t1 and abs(f - mhz) < 1.5), (m, f, c)
        # The channel's index and count in the header; the core comments packets only.
        assert h["channels"] == len(freqs)
        if h is not wide[0][1]:
            assert freqs[h["channel"]] == h["freq_hz"] and c == ""


def test_channel_plan():
    assert gp.channel_plan("all", CENTER, 8e6) == [
        2402000000, 2404000000, 2405000000, 2406000000, 2408000000, 2410000000, 2412000000,
        2414000000, 2415000000, 2416000000]
    assert gp.channel_plan("ble37, wpan11, 2403", 2404e6, 3e6) == [2402000000, 2403000000,
                                                                   2405000000]
    assert gp.channel_plan("ble38,wpan15", 2426e6, 2e6) == [2425000000, 2426000000]
    with pytest.raises(ValueError, match="2480 MHz"):
        gp.channel_plan("ble39", CENTER, 8e6)
    assert [gp.ble_index(gp.ble_channel_hz(k)) for k in range(40)] == list(range(40))
    assert gp.channel_name(2410000000) == "2410 (BLE 3, 802.15.4 12)"
    assert gp.channel_rate(20000000) == (4000000, 5)
    assert gp.channel_rate(10000000) == (2000000, 5)
    with pytest.raises(ValueError):
        gp.channel_rate(25000000)


def test_burst_header_is_pcap_proto():
    st, _ = layout("apps/pcap/src/pcap_proto.h", "pcap_burst_hdr")
    fields = c_struct("apps/pcap/src/pcap_proto.h", "pcap_burst_hdr")
    names = [n for n, _ in fields]
    assert gp.BURST_HDR.format == st.format
    assert gp.BURST_CHANNEL_AT == struct.calcsize(
        "<" + "".join(f for _, f in fields[:names.index("channel")]))
    assert names[names.index("channel") - 2] == "modulation"
    assert gp.MOD_WIDEBAND == C["PCAP_MOD_WIDEBAND"]
    assert (gp.DEC_BLE, gp.DEC_WPAN, gp.DEC_GENERIC, gp.DEC_WIDEBAND, gp.DEC_BAD_CRC,
            gp.DEC_BLE_DATA, gp.DEC_COMMENTS, gp.DEC_DEFAULT) == tuple(
        C["PCAP_DEC_" + n] for n in ("BLE", "WPAN", "GENERIC", "WIDEBAND", "BAD_CRC",
                                     "BLE_DATA", "COMMENTS", "DEFAULT"))


def test_capture_on_numpy_channels(built):
    _, lib = built
    q, want = recording(lib)
    freqs = gp.channel_plan("all", CENTER, 8e6)
    xs = channelize(q, freqs)
    buf = io.BytesIO()
    out = gp.Output(lib, buf, CH_RATE, freqs, t0_ns=START_NS)
    cap = gp.Capture(lib, freqs, CH_RATE, out)
    for at in range(0, len(xs[0]), 16384):
        cap.feed([x[at:at + 16384] for x in xs], at)
    cap.finish()
    check(buf.getvalue(), want, freqs)
    assert out.crosstalk > 0 and out.merged > 0
    assert 0x332211d6 in cap.known


def test_data_channel_needs_the_shared_address(built):
    """Without the address from channel 37 the data packet on channel 0 stays undecoded."""
    _, lib = built
    q, want = recording(lib)
    freqs = [2404000000]
    xs = channelize(q, freqs)
    buf = io.BytesIO()
    out = gp.Output(lib, buf, CH_RATE, freqs, t0_ns=START_NS)
    cap = gp.Capture(lib, freqs, CH_RATE, out)
    cap.feed(xs, 0)
    cap.finish()
    assert out.written[gp.IF_BLE] == 0
    buf = io.BytesIO()
    out = gp.Output(lib, buf, CH_RATE, freqs, t0_ns=START_NS)
    cap = gp.Capture(lib, freqs, CH_RATE, out, aas=[(0x332211d6, 0x012345)])
    cap.feed(xs, 0)
    cap.finish()
    assert out.written[gp.IF_BLE] == 1


def test_flowgraph_on_a_recording(built, tmp_path, run_main):
    pytest.importorskip("gnuradio")
    path, lib = built
    q, want = recording(lib)
    rec = tmp_path / "rec.cs8"
    q.tofile(rec)
    out = tmp_path / "out.pcapng"
    assert run_main(gp, "--file", rec, "-f", "2409M", "-w", out, "--start",
                    START_NS / 1e9, "--lib", path, "-q") == 0
    check(out.read_bytes(), want, gp.channel_plan("all", CENTER, 8e6))


def record(chan, mhz, at_us, dur_us, iface=gp.IF_BURST, offset=0, bw=0, mod=0, flags=0,
           power=-200):
    """A packet of one channel as the core hands it over (a burst record's header filled)."""
    hdr = gp.BURST_HDR.pack(1, gp.BURST_HDR.size, flags, mhz * 1000000, offset, bw, dur_us, 0, 0,
                            mod, 255, 0, 1, power, -450, power + 30, 0, CH_RATE, 0)
    buf = ctypes.create_string_buffer(hdr, len(hdr))
    c = gp.CPkt(iface, True, power, -450, at_us * CH_RATE // 1000000, offset, dur_us,
                ctypes.addressof(buf), len(hdr), None)
    return gp.Pkt(c, chan, mhz * 1000000, CH_RATE)


def test_output_rules(built):
    """The cross-channel rules on records made by hand."""
    _, lib = built
    freqs = [f * 1000000 for f in (2402, 2404, 2405, 2406, 2408)]
    wide = dict(mod=C["PCAP_MOD_WIDEBAND"], bw=2000000)
    pkts = [
        # One wide signal over three channels, and a burst far enough off at the same time.
        record(1, 2404, 1000, 300, **wide), record(2, 2405, 1000, 300, **wide),
        record(3, 2406, 1001, 300, **wide), record(0, 2402, 1100, 100, offset=50000, bw=100000),
        # A packet decoded on 2404: its record from 2405, also one the analysis skipped.
        record(1, 2404, 5000, 200, iface=gp.IF_BLE, power=-140),
        record(2, 2405, 5003, 190, offset=-1000000, bw=1000000, power=-150),
        record(2, 2405, 5003, 190, flags=gp.BF_SKIPPED, power=-150),
        # An 802.15.4 frame on 2405: 17 dB down 2.8 MHz away is it, 1 dB down 3 MHz away is not.
        record(2, 2405, 9000, 900, iface=gp.IF_WPAN, power=-140),
        record(0, 2402, 9003, 890, offset=1200000, bw=500000, power=-310),
        record(4, 2408, 9003, 890, bw=500000, power=-150),
        # One signal between two channels: the record nearer its channel's centre stays.
        record(1, 2404, 12000, 400, offset=600000, bw=300000),
        record(2, 2405, 12001, 400, offset=-400000, bw=300000),
        # Weak Wi-Fi: two channels far apart lifted at once, then a piece between them.
        record(0, 2402, 15000, 30, flags=gp.BF_SKIPPED), record(4, 2408, 15002, 30,
                                                                 flags=gp.BF_SKIPPED),
        record(2, 2405, 15080, 40, flags=gp.BF_SKIPPED),
        # A signal between two neighbours from the same moment, unanalysed: the stronger stays.
        record(1, 2404, 20000, 100, flags=gp.BF_SKIPPED, power=-200),
        record(2, 2405, 20003, 100, flags=gp.BF_SKIPPED, power=-180),
    ]
    buf = io.BytesIO()
    out = gp.Output(lib, buf, CH_RATE, freqs, t0_ns=START_NS)
    out.add(pkts)
    out.drain(0, final=True)
    assert out.written == [1, 1, 6]
    assert (out.crosstalk, out.duplicates, out.merged) == (3, 2, 4)
    st, names = layout("apps/pcap/src/pcap_proto.h", "pcap_burst_hdr")
    heads = [dict(zip(names, st.unpack_from(d))) for i, _, d, _ in records(buf.getvalue())
             if i == gp.IF_BURST]
    assert [(h["freq_hz"] + h["offset_hz"]) / 1e6 for h in heads] == [2405.0, 2402.05, 2408.0,
                                                                       2404.6, 2405.0, 2405.0]
    assert heads[0]["bandwidth_hz"] == 4000000 and heads[0]["channel"] == 2
    assert [h["channel"] for h in heads[1:]] == [0, 4, 2, 2, 2]
    assert heads[4]["modulation"] == C["PCAP_MOD_WIDEBAND"] and heads[4]["duration_us"] == 120
    assert heads[4]["bandwidth_hz"] == 8000000 and not heads[4]["flags"] & gp.BF_SKIPPED
    assert heads[5]["power_dbfs_x10"] == -180
