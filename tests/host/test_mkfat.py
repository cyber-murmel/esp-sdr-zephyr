# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/multiapp/mkfat.py: images read back with a small FAT reader written from the spec."""

import struct

import pytest

from conftest import load

mkfat = load("apps/multiapp/mkfat.py")


def read_fat(img):
    """Root directory files of a FAT12/16 image: {long or 8.3 name: bytes}, and its geometry."""
    (bps, spc, rsvd, nfats, root_entries, total16, _media, fat_secs) = struct.unpack_from(
        "<HBHBHHBH", img, 11)
    total = total16 or struct.unpack_from("<I", img, 32)[0]
    kind = img[54:62].decode().strip()
    fat_off = rsvd * bps
    root_off = (rsvd + nfats * fat_secs) * bps
    data_off = root_off + root_entries * 32
    fat = img[fat_off:fat_off + fat_secs * bps]

    def nxt(c):
        if kind == "FAT12":
            v = struct.unpack_from("<H", fat, c + c // 2)[0]
            return v >> 4 if c & 1 else v & 0xFFF
        return struct.unpack_from("<H", fat, 2 * c)[0]

    eoc = 0xFF8 if kind == "FAT12" else 0xFFF8
    files, lfn = {}, {}
    for i in range(root_entries):
        e = img[root_off + 32 * i: root_off + 32 * (i + 1)]
        if e[0] == 0:
            break
        attr = e[11]
        if attr == 0x0F:
            seq = e[0] & 0x1F
            chars = e[1:11] + e[14:26] + e[28:32]
            lfn[seq] = (chars, e[13])
            continue
        if attr & 0x08:
            lfn = {}
            continue
        name11 = e[0:11]
        cluster, size = struct.unpack_from("<HI", e, 26)
        if lfn:
            raw = b"".join(lfn[k][0] for k in sorted(lfn))
            name = raw.decode("utf-16-le").split("\x00")[0]
            assert {c for _, c in lfn.values()} == {mkfat.sfn_checksum(name11)}
        else:
            base, ext = name11[:8].decode().rstrip(), name11[8:].decode().rstrip()
            name = base + ("." + ext if ext else "")
        data = bytearray()
        c = cluster
        while size and c < eoc:
            off = data_off + (c - 2) * spc * bps
            data += img[off:off + spc * bps]
            c = nxt(c)
        files[name] = bytes(data[:size])
        lfn = {}
    return files, {"kind": kind, "spc": spc, "total": total, "data_off": data_off}


def write(tmp_path, name, data):
    p = tmp_path / name
    p.write_bytes(data)
    return str(p)


def test_round_trip_long_names(tmp_path):
    payload = {
        "capture.llext": bytes(range(256)) * 40,       # 10 KiB, three clusters
        "README": b"plain 8.3 name\n",
        "a.very.long.file.name.llext": b"x" * 4096,  # exactly one cluster
        "empty.txt": b"",
    }
    paths = [write(tmp_path, n, d) for n, d in payload.items()]
    img, kind, clusters, data0, used = mkfat.build(1024 * 1024, paths, "MULTIAPP", 0x1234, 4096)
    files, geo = read_fat(bytes(img))
    assert files == payload
    assert kind == geo["kind"] == "FAT12"
    assert img[510:512] == b"\x55\xaa"
    # The data area starts on a cluster (flash erase page) boundary.
    assert data0 % 4096 == 0 and geo["data_off"] == data0
    # The image ends at the last used cluster: 3 + 1 + 1 clusters of data.
    assert used == data0 + 5 * 4096


def test_fat16_for_many_clusters(tmp_path):
    path = write(tmp_path, "big.bin", b"\xa5" * 70000)
    img, kind, clusters, _, _ = mkfat.build(16 * 1024 * 1024, [path], "VOL", 1, 2048)
    assert kind == "FAT16" and clusters >= 4085
    files, _ = read_fat(bytes(img))
    assert files == {"big.bin": b"\xa5" * 70000}


def test_deterministic(tmp_path):
    path = write(tmp_path, "app.llext", b"same")
    a = mkfat.build(256 * 1024, [path], "MULTIAPP", 7, 4096)[0]
    b = mkfat.build(256 * 1024, [path], "MULTIAPP", 7, 4096)[0]
    assert a == b


def test_short_names():
    taken = set()
    assert mkfat.short_name("README.TXT", taken) == (b"README  TXT", False)
    # Lower case, long or dotted names get a numbered tail and a long name entry.
    assert mkfat.short_name("capture.llext", taken) == (b"CAPTUR~1LLE", True)
    assert mkfat.short_name("capture.llextx", taken) == (b"CAPTUR~2LLE", True)
    assert mkfat.short_name("a+b.c", taken) == (b"A_B~1   C  ", True)
    assert mkfat.short_name("README.TXT", taken)[0] == b"README~1TXT"


def test_lfn_entries_layout():
    name = "abcdefghijklm"      # exactly 13 characters: one entry, no terminator
    (e,) = mkfat.lfn_entries(name, 0x5A)
    assert e[0] == 0x41 and e[11] == 0x0F and e[13] == 0x5A
    chars = (e[1:11] + e[14:26] + e[28:32]).decode("utf-16-le")
    assert chars == name
    entries = mkfat.lfn_entries("x" * 14, 0)
    assert [x[0] for x in entries] == [0x42, 0x01]


def test_too_big(tmp_path):
    path = write(tmp_path, "huge.bin", b"\0" * 300000)
    with pytest.raises(SystemExit):
        mkfat.build(128 * 1024, [path], "MULTIAPP", 1, 4096)


def test_main_writes_used_part(tmp_path, run_main, capsys):
    path = write(tmp_path, "hello.llext", b"hello")
    out = tmp_path / "vol.img"
    run_main(mkfat, "--size", "0x40000", "--out", out, path)
    img = out.read_bytes()
    files, _ = read_fat(img + bytes(0x40000 - len(img)))
    assert files == {"hello.llext": b"hello"}
    assert "FAT12" in capsys.readouterr().out
