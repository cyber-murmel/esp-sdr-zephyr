#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""An application for the MCH2022 badge's RP2040, behind the badge's own bootloader, as a UF2.

The RP2040's flash holds the stock bootloader (from 0x10000000), the sector of an image header
(0x10008000) and the application (0x10010000). The bootloader runs the application if the header
{vector table, size, CRC-32 over the image} holds and the vector table looks right; this tool
checks the same. The UF2 runs without a gap from the header to the application's end, zeros
between them, as the stock image does; nothing below 0x10008000 is written, so the bootloader
stays. Works on this repository's firmware and on the stock application alike (the rollback).
"""

import argparse
import struct
import sys
import zlib
from pathlib import Path

HEADER_ADDR = 0x10008000
APP_ADDR = 0x10010000
FLASH_END = 0x10200000
SRAM_BASE = 0x20000000
PAGE = 256

UF2_MAGIC0 = 0x0A324655
UF2_MAGIC1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY = 0x00002000
UF2_FAMILY_RP2040 = 0xE48BFF56
UF2_DATA = 476


class ImageError(ValueError):
    """The bootloader would not run this image."""


def pad_to_page(data):
    """@p data with zeros up to the next page."""
    return data + bytes(-len(data) % PAGE)


def check(app, vtor=APP_ADDR):
    """Raises ImageError unless the bootloader would accept @p app (padded) at @p vtor."""
    if len(app) < 8:
        raise ImageError("no vector table")
    if vtor + len(app) > FLASH_END:
        raise ImageError(f"{len(app)} bytes at {vtor:#x} do not fit the flash")
    sp, reset = struct.unpack_from("<II", app)
    if sp < SRAM_BASE:
        raise ImageError(f"stack pointer {sp:#x} is not in RAM: not a vector table")
    if reset < vtor or reset > vtor + len(app) or not reset & 1:
        raise ImageError(f"reset vector {reset:#x} is not Thumb code in the image at {vtor:#x}")


def header_page(app, vtor=APP_ADDR):
    """The header's page for @p app (padded)."""
    head = struct.pack("<III", vtor, len(app), zlib.crc32(app))
    return head + bytes(PAGE - len(head))


def uf2_blocks(data, addr):
    """@p data, whole pages, as UF2 blocks from @p addr."""
    total = len(data) // PAGE
    out = bytearray()

    for k in range(total):
        out += struct.pack("<IIIIIIII", UF2_MAGIC0, UF2_MAGIC1, UF2_FLAG_FAMILY, addr + k * PAGE,
                           PAGE, k, total, UF2_FAMILY_RP2040)
        out += data[k * PAGE:(k + 1) * PAGE] + bytes(UF2_DATA - PAGE)
        out += struct.pack("<I", UF2_MAGIC_END)
    return bytes(out)


def image(app_bin, vtor=APP_ADDR):
    """(the UF2, the header's page, the padded application) of the binary @p app_bin."""
    app = pad_to_page(app_bin)

    check(app, vtor)
    head = header_page(app, vtor)
    flash = head + bytes(vtor - HEADER_ADDR - PAGE) + app
    return uf2_blocks(flash, HEADER_ADDR), head, app


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", maxsplit=1)[0])
    ap.add_argument("bin", type=Path, help="the application, linked at its vector table")
    ap.add_argument("uf2", type=Path, help="the UF2 to write")
    ap.add_argument("--vtor", type=lambda s: int(s, 0), default=APP_ADDR,
                    help="where the application is linked")
    a = ap.parse_args(argv)
    try:
        uf2, head, app = image(a.bin.read_bytes(), a.vtor)
    except ImageError as e:
        print(f"{a.bin}: {e}", file=sys.stderr)
        return 1
    a.uf2.write_bytes(uf2)
    vtor, size, crc = struct.unpack_from("<III", head)
    print(f"{a.uf2}: vector table {vtor:#x}, {size} bytes ({len(app) // PAGE} pages), "
          f"CRC-32 {crc:#010x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
