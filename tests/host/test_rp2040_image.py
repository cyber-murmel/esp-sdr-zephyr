# SPDX-License-Identifier: GPL-3.0-or-later
"""boards/badgeteam/mch2022_rp2040/support/rp2040_image.py: the MCH2022 RP2040's application
behind the stock bootloader. The values come from the bootloader's own checks and from a parser
of the UF2 format written here, not from the tool."""

import struct
import zlib

import pytest
from conftest import load

img = load("boards/badgeteam/mch2022_rp2040/support/rp2040_image.py")

APP = 0x10010000
HEADER = 0x10008000


def vectors(sp=0x20042000, reset=APP + 0xF7):
    return struct.pack("<II", sp, reset)


def parse_uf2(uf2):
    """{address: page} from a UF2, checking each block's fixed fields."""
    assert len(uf2) % 512 == 0
    pages = {}
    total = len(uf2) // 512
    for k in range(total):
        block = uf2[512 * k:512 * (k + 1)]
        m0, m1, flags, addr, size, no, count, family = struct.unpack_from("<8I", block)
        assert (m0, m1) == (0x0A324655, 0x9E5D5157)
        assert struct.unpack_from("<I", block, 508)[0] == 0x0AB16F30
        assert flags == 0x00002000
        assert family == 0xE48BFF56
        assert size == 256
        assert (no, count) == (k, total)
        assert block[32 + 256:508] == bytes(476 - 256)
        assert addr % 256 == 0
        assert addr not in pages
        pages[addr] = block[32:32 + 256]
    return pages


def test_header_holds_what_the_bootloader_checks():
    app = vectors() + bytes(range(256)) * 2
    uf2, head, padded = img.image(app)

    assert len(padded) % 256 == 0
    assert padded[:len(app)] == app
    assert set(padded[len(app):]) <= {0}
    vtor, size, crc = struct.unpack_from("<III", head)
    assert vtor == APP
    assert size == len(padded)
    assert crc == zlib.crc32(padded)
    assert len(head) == 256
    assert head[12:] == bytes(244)
    assert uf2


def test_uf2_runs_from_the_header_to_the_end_of_the_application():
    app = vectors() + b"\xa5" * 1000
    uf2, head, padded = img.image(app)
    pages = parse_uf2(uf2)

    assert sorted(pages) == list(range(HEADER, APP + len(padded), 256))
    assert pages[HEADER] == head
    for addr in range(HEADER + 256, APP, 256):
        assert pages[addr] == bytes(256)
    assert b"".join(pages[a] for a in range(APP, APP + len(padded), 256)) == padded


def test_nothing_below_the_header_is_written():
    pages = parse_uf2(img.image(vectors() + bytes(600))[0])

    assert min(pages) == HEADER


def test_a_whole_number_of_pages_stays_as_it_is():
    app = vectors() + bytes(256 - 8)

    assert len(img.image(app)[2]) == 256


@pytest.mark.parametrize("app, why", [
    (b"", "no vector table"),
    (b"\x00\x00", "stack pointer"),
    (vectors(sp=0x10000000) + bytes(248), "stack pointer"),
    (vectors(reset=APP + 0xF6) + bytes(248), "reset vector"),
    (vectors(reset=APP - 1) + bytes(248), "reset vector"),
    (vectors(reset=APP + 257) + bytes(248), "reset vector"),
])
def test_refuses_what_the_bootloader_would_refuse(app, why):
    with pytest.raises(img.ImageError, match=why):
        img.image(app)


def test_refuses_an_image_that_does_not_fit():
    app = vectors() + bytes(img.FLASH_END - APP)

    with pytest.raises(img.ImageError, match="do not fit"):
        img.image(app)


def test_command_line_writes_the_uf2(tmp_path, capsys):
    source = tmp_path / "app.bin"
    out = tmp_path / "app.uf2"
    source.write_bytes(vectors() + bytes(300))

    assert img.main([str(source), str(out)]) == 0
    assert out.read_bytes() == img.image(source.read_bytes())[0]
    assert "CRC-32" in capsys.readouterr().out


def test_command_line_reports_a_bad_image(tmp_path, capsys):
    source = tmp_path / "app.bin"
    out = tmp_path / "app.uf2"
    source.write_bytes(bytes(300))

    assert img.main([str(source), str(out)]) == 1
    assert not out.exists()
    assert "stack pointer" in capsys.readouterr().err
