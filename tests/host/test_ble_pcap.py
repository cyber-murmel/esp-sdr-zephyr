# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/chancap/tools/ble_pcap.py: LE 1M packets out of channel captures, as pcap.

The packets are the golden vectors of tests/unit/ubertooth_le (le_vectors.h, from the
specification's bit handling and checked against upstream Ubertooth's tables): their on-air
bits, GFSK modulated here (BT 0.5, h 0.5) with noise and a carrier offset, must come back as
their access address, PDU and CRC bytes."""

import re
import struct

import numpy as np
import pytest

from conftest import REPO, load

bp = load("apps/chancap/tools/ble_pcap.py")


def vectors():
    text = (REPO / "tests/unit/ubertooth_le/src/le_vectors.h").read_text()
    arrays = {m.group(1): bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", m.group(2)))
              for m in re.finditer(r"static const uint8_t (\w+)\[\d+\] = \{(.*?)\};", text, re.S)}
    out = []
    for m in re.finditer(r'\{ "(\w+)", (\d+), (0x[0-9a-f]+), (0x[0-9a-f]+), (\d+), (\d+), '
                         r'(\w+), (\w+), (\w+) \}', text):
        name, mhz, aa, crc_init, pdu_len, air_len, pdu, air, usb = m.groups()
        # usb: access address, PDU and CRC dewhitened, as upstream's 50 byte USB packets
        # carry them (so cut at 50).
        out.append({"name": name, "mhz": int(mhz), "aa": int(aa, 16),
                    "crc_init": int(crc_init, 16), "pdu": arrays[pdu][:int(pdu_len)],
                    "air": arrays[air][:int(air_len)],
                    "clear": arrays[usb][:min(50, 4 + int(pdu_len) + 3)]})
    assert len(out) == 12
    return out


VECS = vectors()


def gfsk(air, sps, bt=0.5, h=0.5):
    """Unit amplitude baseband of on-air bytes (LSB first)."""
    bits = np.unpackbits(np.frombuffer(air, dtype=np.uint8), bitorder="little")
    nrz = np.repeat(2.0 * bits - 1.0, sps)
    t = np.arange(-1.5 * sps, 1.5 * sps + 1) / sps
    sigma = np.sqrt(np.log(2)) / (2 * np.pi * bt)
    g = np.exp(-t ** 2 / (2 * sigma ** 2))
    f = np.convolve(nrz, g / g.sum(), mode="same")
    return np.exp(1j * np.cumsum(f) * np.pi * h / sps)


def capture(parts, sps, offset_hz=0.0, snr_db=25.0, seed=1):
    """Noise, then each (on-air bytes, gap in samples after it), with a carrier offset."""
    rng = np.random.default_rng(seed)
    x = [np.zeros(40, complex)]
    for air, gap in parts:
        x += [gfsk(air, sps), np.zeros(gap, complex)]
    x = np.concatenate(x)
    rate = sps * 1e6
    x = x * np.exp(2j * np.pi * offset_hz / rate * np.arange(len(x)))
    sigma = 10 ** (-snr_db / 20) / np.sqrt(2)
    x = x + sigma * (rng.standard_normal(len(x)) + 1j * rng.standard_normal(len(x)))
    return 0.25 * x, rate


@pytest.mark.parametrize("sps", [2, 4])
@pytest.mark.parametrize("v", VECS, ids=[v["name"] for v in VECS])
def test_vectors(v, sps):
    iq, rate = capture([(v["air"], 30)], sps, offset_hz=40e3, seed=len(v["pdu"]) + sps)
    pkts = bp.decode(iq, rate, v["mhz"], v["aa"], v["crc_init"])
    assert len(pkts) == 1
    p = pkts[0]
    assert len(p["air"]) == 4 + len(v["pdu"]) + 3
    assert p["air"][:len(v["clear"])] == v["clear"] and p["pdu"] == v["pdu"]
    assert p["crc_ok"] and p["aa_errors"] == 0
    assert p["offset_hz"] == pytest.approx(40e3, abs=10e3)
    # The access address starts after the 40 samples of noise and the preamble.
    assert abs(p["sample"] - (40 + 8 * sps)) <= sps


def test_two_packets_and_a_bad_crc():
    """An advertisement and, 150 us later, a scan request with a broken CRC in one capture."""
    adv = next(v for v in VECS if v["name"] == "adv_ind_37")
    req = next(v for v in VECS if v["name"] == "scan_rsp_37")
    broken = bytearray(req["air"])
    broken[8] ^= 0x10  # a PDU bit
    iq, rate = capture([(adv["air"], 300), (bytes(broken), 30)], 2, offset_hz=-25e3)
    pkts = bp.decode(iq, rate, 2402)
    assert [p["crc_ok"] for p in pkts] == [True, False]
    assert pkts[0]["pdu"] == adv["pdu"]
    assert pkts[1]["sample"] > pkts[0]["sample"] + 8 * 2 * len(adv["air"])


def test_noise_only_and_a_cut_packet():
    rng = np.random.default_rng(7)
    noise = (rng.standard_normal(4000) + 1j * rng.standard_normal(4000)) / 8
    assert bp.decode(noise, 2e6, 2402) == []
    adv = VECS[0]
    iq, rate = capture([(adv["air"], 0)], 2)
    assert bp.decode(iq[:len(iq) // 2], rate, 2402) == []


def test_rate_must_be_whole_symbols():
    with pytest.raises(ValueError):
        bp.decode(np.zeros(100, complex), 3.2e6, 2402)


def test_rf_channel():
    assert [bp.rf_channel(m) for m in (2402, 2404, 2426, 2480)] == [0, 1, 12, 39]


def test_pcap_records(tmp_path):
    v = VECS[0]
    iq, rate = capture([(v["air"], 30)], 2)
    pkt = bp.decode(iq, rate, v["mhz"])[0]
    path = tmp_path / "x.pcap"
    with open(path, "wb") as f:
        w = bp.PcapWriter(f)
        w.write(1234.5678905, 2402, pkt)
        w.write(1235.0, 2402, dict(pkt, crc_ok=False, aa_errors=1))
    data = path.read_bytes()
    magic, major, minor, _, _, snap, link = struct.unpack("<IHHiIII", data[:24])
    assert (magic, major, minor, link) == (0xA1B2C3D4, 2, 4, 256)
    sec, usec, incl, orig = struct.unpack("<IIII", data[24:40])
    assert (sec, usec, incl, orig) == (1234, 567890, 10 + len(pkt["air"]), 10 + len(pkt["air"]))
    ch, sig, noise, offenses, ref_aa, flags = struct.unpack("<BbbBIH", data[40:50])
    assert (ch, offenses, ref_aa) == (0, 0, 0x8E89BED6)
    assert flags == 0x0001 | 0x0010 | 0x0020 | 0x0400 | 0x0800
    assert data[50:50 + incl - 10] == pkt["air"]
    second = data[50 + incl - 10:]
    assert struct.unpack("<BbbBIH", second[16:26])[3] == 1
    assert struct.unpack("<BbbBIH", second[16:26])[5] & 0x0800 == 0


def test_main_on_saved_captures(tmp_path, run_main, capsys):
    """chancap_bridge.py --save's files in, a pcap out; --bad keeps the broken one."""
    import json

    good, broken = VECS[0], bytearray(VECS[3]["air"])
    broken[9] ^= 1
    for k, air in enumerate([good["air"], bytes(broken)]):
        iq, rate = capture([(air, 30)], 2, seed=k)
        meta = {"channel": 0, "rx_time": [100 + k, 0.25], "rx_freq": 2402e6, "rx_rate": rate}
        np.savez(tmp_path / f"{k:06d}_ch0.npz", iq=iq.astype(np.complex64),
                 meta=json.dumps(meta))
    out = tmp_path / "out.pcap"
    assert run_main(bp, "--npz", tmp_path, "-w", out, "--print") == 0
    err = capsys.readouterr().err
    assert "ADV_IND" in err and "1 packets with a good CRC, 1 bad" in err
    assert len(out.read_bytes()) == 24 + 16 + 10 + len(good["clear"])
    assert run_main(bp, "--npz", tmp_path, "-w", out, "--bad") == 0
    data = out.read_bytes()
    rec2 = 24 + 16 + 10 + len(good["clear"])
    # The second capture is a second later on the board: so in the file.
    assert struct.unpack("<I", data[rec2:rec2 + 4])[0] == struct.unpack("<I", data[24:28])[0] + 1
