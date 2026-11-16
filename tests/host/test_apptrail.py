# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/multiapp/apptrail.py: the trailer the multiapp host checks before it loads an app."""

import struct
import zlib

import pytest

from conftest import load

apptrail = load("apps/multiapp/apptrail.py")


def test_trailer(tmp_path, run_main):
    elf, bid, out = tmp_path / "app.llext", tmp_path / "id.bin", tmp_path / "app.trail"
    elf.write_bytes(b"\x7fELF" + bytes(range(200)))
    bid.write_bytes(bytes(range(16)))
    assert run_main(apptrail, "--stack", 3072, "--build-id", bid, elf, out) == 0
    data = out.read_bytes()
    body, trailer = data[:-32], data[-32:]
    assert body == elf.read_bytes()
    magic, size, crc, stack, build_id = struct.unpack("<4I16s", trailer)
    # include/multiapp/multiapp.h: "MAP1", the ELF's size and CRC-32 (crc32_ieee()), the stack.
    assert magic.to_bytes(4, "little") == b"MAP1"
    assert (size, crc, stack, build_id) == (len(body), zlib.crc32(body), 3072, bytes(range(16)))


def test_build_id_length(tmp_path, run_main):
    elf, bid = tmp_path / "app.llext", tmp_path / "id.bin"
    elf.write_bytes(b"x")
    bid.write_bytes(b"short")
    with pytest.raises(SystemExit):
        run_main(apptrail, "--stack", 1, "--build-id", bid, elf, tmp_path / "out")
