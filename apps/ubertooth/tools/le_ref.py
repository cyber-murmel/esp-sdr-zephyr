#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Independent reference of the BLE 1M link layer bit handling, written from the
# Core specification (Vol 6 Part B) and not from the C code under test: data
# whitening, CRC-24, channel selection algorithms #1 and #2, and the on-air
# bit order. Generates the golden vectors of tests/unit/ubertooth_le
# (--emit-c) and checks its own models against the vendored upstream tables
# (--selftest).
#
# Bit order: bytes go on air LSB first. A packet is the preamble (0xAA when
# the access address bit 0 is 0, else 0x55), the 32 bit access address, the
# PDU and the 24 bit CRC; whitening covers PDU and CRC. The CRC register is
# transmitted starting with its most significant bit (position 23).

import argparse
import random
import re
import sys
from pathlib import Path

ADV_AA = 0x8E89BED6
UPSTREAM = Path(__file__).resolve().parent.parent / "src" / "upstream"


def channel_index(mhz):
    """Channel index (0..36 data, 37..39 advertising) of a frequency in MHz."""
    if mhz == 2402:
        return 37
    if mhz == 2426:
        return 38
    if mhz == 2480:
        return 39
    k = (mhz - 2402) // 2
    return k - 1 if k < 12 else k - 2


def channel_mhz(idx):
    if idx == 37:
        return 2402
    if idx == 38:
        return 2426
    if idx == 39:
        return 2480
    return 2404 + 2 * idx if idx < 11 else 2428 + 2 * (idx - 11)


def whitening_bits(idx, n):
    """First n bits of the whitening sequence of channel idx (7 bit LFSR x^7 + x^4 + 1).

    Position 0 starts as 1, positions 1..6 hold the channel index MSB first, the
    output is position 6 and feeds position 0 and, XORed, position 4.
    """
    s = [1] + [(idx >> (5 - i)) & 1 for i in range(6)]
    out = []
    for _ in range(n):
        o = s[6]
        out.append(o)
        s = [o, s[0], s[1], s[2], s[3] ^ o, s[4], s[5]]
    return out


def crc24(init, pdu):
    """CRC register after the PDU; init is CRCInit as an integer (advertising: 0x555555)."""
    r = init & 0xFFFFFF
    for b in pdu:
        for j in range(8):
            fb = ((r >> 23) & 1) ^ ((b >> j) & 1)
            r = (r << 1) & 0xFFFFFF
            if fb:
                r ^= 0x00065B
    return r


def crc_wire_bytes(r):
    """The three CRC bytes as they appear after dewhitening (position 23 sent first)."""
    bits = [(r >> i) & 1 for i in range(23, -1, -1)]
    return [sum(bits[8 * k + j] << j for j in range(8)) for k in range(3)]


def bytes_to_bits(data):
    return [(b >> j) & 1 for b in data for j in range(8)]


def bits_to_bytes(bits):
    assert len(bits) % 8 == 0
    return [sum(bits[8 * k + j] << j for j in range(8)) for k in range(len(bits) // 8)]


def air_bits(aa, idx, pdu, crc_init, preamble=True):
    """On-air bit stream of one packet."""
    aa_bytes = [(aa >> (8 * k)) & 0xFF for k in range(4)]
    pre = [0xAA] if (aa & 1) == 0 else [0x55]
    body = list(pdu) + crc_wire_bytes(crc24(crc_init, pdu))
    wh = whitening_bits(idx, 8 * len(body))
    wbits = [b ^ w for b, w in zip(bytes_to_bits(body), wh)]
    head = bytes_to_bits((pre if preamble else []) + aa_bytes)
    return head + wbits


def air_bytes(aa, idx, pdu, crc_init, preamble=True):
    return bits_to_bytes(air_bits(aa, idx, pdu, crc_init, preamble))


def usb_data(aa, pdu, crc_init):
    """What libbtbb expects in usb_pkt_rx.data: AA (LE), PDU, CRC, first 50 bytes."""
    d = [(aa >> (8 * k)) & 0xFF for k in range(4)] + list(pdu) + crc_wire_bytes(crc24(crc_init, pdu))
    return (d + [0] * 50)[:50]


def unmapped_csa1(event, inc):
    """Unmapped channel of connection event number event (0 = first event)."""
    return ((event + 1) * inc) % 37


def used_list(chm):
    return [i for i in range(37) if (chm[i // 8] >> (i % 8)) & 1]


def csa1(event, inc, chm):
    u = unmapped_csa1(event, inc)
    used = used_list(chm)
    return u if u in used else used[u % len(used)]


def _perm(x):
    x = ((x & 0xF0F0) >> 4) | ((x & 0x0F0F) << 4)
    x = ((x & 0xCCCC) >> 2) | ((x & 0x3333) << 2)
    x = ((x & 0xAAAA) >> 1) | ((x & 0x5555) << 1)
    return x


def csa2_prn(counter, aa):
    cid = ((aa >> 16) ^ aa) & 0xFFFF
    p = (counter & 0xFFFF) ^ cid
    for _ in range(3):
        p = _perm(p)
        p = (17 * p + cid) & 0xFFFF
    return p ^ cid


def csa2(counter, aa, chm):
    p = csa2_prn(counter, aa)
    u = p % 37
    used = used_list(chm)
    return u if u in used else used[(len(used) * p) >> 16]


# ---- self test against the vendored upstream tables ----

def _arr(src, name):
    m = re.search(name + r"\[[^\]]*\]\s*=\s*\{(.*?)\};", src, re.S)
    return [int(x, 0) for x in re.findall(r"0x[0-9a-fA-F]+|\d+", m.group(1))]


def selftest():
    h = (UPSTREAM / "bluetooth_le.h").read_text()
    c = (UPSTREAM / "bluetooth_le.c").read_text()
    whit = _arr(h, r"whitening")
    widx = _arr(h, r"whitening_index")
    lut = _arr(c, r"btle_crc_lut")
    assert len(whit) == 127 and len(widx) == 40 and len(lut) == 256
    for ch in range(40):
        ref = [whit[(widx[ch] + i) % 127] for i in range(127)]
        assert whitening_bits(ch, 127) == ref, "whitening channel %d" % ch
    rnd = random.Random(1)
    for _ in range(500):
        init = rnd.getrandbits(24)
        pdu = [rnd.getrandbits(8) for _ in range(rnd.randint(2, 60))]
        st = int(format(init, "024b")[::-1], 2)
        for b in pdu:
            st = (st >> 8) ^ lut[(b ^ st) & 0xFF]
        assert crc_wire_bytes(crc24(init, pdu)) == [st & 0xFF, (st >> 8) & 0xFF, (st >> 16) & 0xFF]
    # Core spec Vol 6 Part C sample data, channel selection algorithm #2
    assert [csa2_prn(i, ADV_AA) for i in range(4)] == [56857, 1685, 38301, 27475]
    # the inverse table the promiscuous hop recovery uses
    lut_inv = _arr(h, r"hop_interval_lut")
    assert all((k * lut_inv[k]) % 37 == 1 for k in range(1, 37))
    print("selftest ok: whitening (40 channels), CRC-24, CSA#2 sample data, hop inverse table")


# ---- golden vectors ----

def le_addr(n):
    return [(0x1234567 * (n + 3) >> (8 * k)) & 0xFF for k in range(6)]


def vectors():
    v = []

    def add(name, aa, idx, crc_init, pdu):
        v.append(dict(name=name, aa=aa, idx=idx, mhz=channel_mhz(idx), crc_init=crc_init, pdu=list(pdu)))

    adv_a = [0x11, 0x22, 0x33, 0x44, 0x55, 0xC6]
    init_a = [0x66, 0x77, 0x88, 0x99, 0xAA, 0xD7]
    # ADV_IND: TxAdd = 1 (random address), flags + complete local name
    ad = [0x02, 0x01, 0x06, 0x07, 0x09] + list(b"Ubert")
    add("adv_ind_37", ADV_AA, 37, 0x555555, [0x40, 6 + len(ad)] + adv_a + ad)
    add("adv_nonconn_38", ADV_AA, 38, 0x555555, [0x42, 6 + 3] + adv_a + [0x02, 0x0A, 0x04])
    add("scan_req_39", ADV_AA, 39, 0x555555, [0xC3, 12] + init_a + adv_a)
    add("scan_rsp_37", ADV_AA, 37, 0x555555, [0x44, 6 + 8] + adv_a + [0x07, 0xFF, 1, 2, 3, 4, 5, 6])
    connect = [0xD6, 0x11, 0x22, 0x33,            # access address 0x332211D6
               0x45, 0x23, 0x01,                  # CRCInit 0x012345
               0x03, 0x02, 0x00,                  # WinSize 3, WinOffset 2
               0x18, 0x00, 0x00, 0x00, 0x48, 0x00,  # Interval 24, Latency 0, Timeout 72
               0xFF, 0xFF, 0xFF, 0xFF, 0x1F,      # channel map, all 37 used
               0xAC]                              # Hop 12, SCA 5
    add("connect_req_37", ADV_AA, 37, 0x555555, [0xC5, 34] + init_a + adv_a + connect)
    add("adv_direct_38", ADV_AA, 38, 0x555555, [0x41, 12] + adv_a + init_a)
    dd_aa, dd_crc = 0x332211D6, 0x012345
    add("data_empty_9", dd_aa, 9, dd_crc, [0x01, 0x00])
    add("data_l2cap_20", dd_aa, 20, dd_crc, [0x02, 27] + [(7 * i + 3) & 0xFF for i in range(27)])
    add("ll_conn_update_0", dd_aa, 0, dd_crc, [0x03, 12, 0x00, 0x02, 0x03, 0x00, 0x30, 0x00, 0x00, 0x00, 0x90, 0x00, 0x40, 0x00])
    add("ll_chm_36", dd_aa, 36, dd_crc, [0x03, 8, 0x01, 0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0x20, 0x00])
    # data length extension: 60 byte payload, longer than the 46 bytes the USB packet carries
    add("data_long_15", 0x8F1E3A71, 15, 0xABCDEF, [0x02, 60] + [(11 * i + 5) & 0xFF for i in range(60)])
    # AA with bit 0 = 1: the preamble is 0x55
    add("data_aa_odd_30", 0x6A5C3B21, 30, 0x7F00A1, [0x0A, 4, 1, 2, 3, 4])
    return v


def emit_c(out):
    def carr(name, data, per=12):
        lines = ["static const uint8_t %s[%d] = {" % (name, len(data))]
        for i in range(0, len(data), per):
            lines.append("\t" + ", ".join("0x%02x" % x for x in data[i:i + per]) + ",")
        lines.append("};")
        return "\n".join(lines)

    vs = vectors()
    out.write("/* SPDX-License-Identifier: GPL-3.0-or-later */\n")
    out.write("/* Generated by apps/ubertooth/tools/le_ref.py --emit-c: do not edit. */\n\n")
    for i, x in enumerate(vs):
        out.write(carr("v%d_pdu" % i, x["pdu"]) + "\n")
        out.write(carr("v%d_air" % i, air_bytes(x["aa"], x["idx"], x["pdu"], x["crc_init"])) + "\n")
        out.write(carr("v%d_usb" % i, usb_data(x["aa"], x["pdu"], x["crc_init"])) + "\n\n")
    out.write("struct le_vec {\n\tconst char *name;\n\tuint16_t mhz;\n\tuint32_t aa;\n\tuint32_t crc_init;\n")
    out.write("\tuint8_t pdu_len;\n\tuint8_t air_len;\n\tconst uint8_t *pdu;\n\tconst uint8_t *air;\n\tconst uint8_t *usb;\n};\n\n")
    out.write("static const struct le_vec le_vecs[] = {\n")
    for i, x in enumerate(vs):
        air = air_bytes(x["aa"], x["idx"], x["pdu"], x["crc_init"])
        out.write('\t{ "%s", %d, 0x%08x, 0x%06x, %d, %d, v%d_pdu, v%d_air, v%d_usb },\n'
                  % (x["name"], x["mhz"], x["aa"], x["crc_init"], len(x["pdu"]), len(air), i, i, i))
    out.write("};\n\n")
    # channel selection samples for le_conn
    chm9 = [0x00, 0x06, 0x00, 0x18, 0x1E]  # a map with a few channels cleared
    inc = 7
    out.write("/* CSA#1 (hop %d, all 37 channels, then map %s) and CSA#2 (AA 0x%08x) for events 0..39 */\n"
              % (inc, "".join("%02x" % b for b in chm9), ADV_AA))
    full = [0xFF, 0xFF, 0xFF, 0xFF, 0x1F]
    out.write(carr("csa1_full", [csa1(e, inc, full) for e in range(40)]) + "\n")
    out.write(carr("csa1_part", [csa1(e, inc, chm9) for e in range(40)]) + "\n")
    out.write(carr("csa2_full", [csa2(e, ADV_AA, full) for e in range(40)]) + "\n")
    out.write(carr("csa2_part", [csa2(e, ADV_AA, chm9) for e in range(40)]) + "\n")
    out.write("static const uint8_t csa_chm_part[5] = { %s };\n" % ", ".join("0x%02x" % b for b in chm9))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--selftest", action="store_true", help="check the models against the upstream tables")
    ap.add_argument("--emit-c", action="store_true", help="write the golden vectors as C to stdout")
    a = ap.parse_args()
    if a.selftest:
        selftest()
    if a.emit_c:
        emit_c(sys.stdout)
    if not (a.selftest or a.emit_c):
        ap.print_help()


if __name__ == "__main__":
    main()
