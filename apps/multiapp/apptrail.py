#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Append the multiapp trailer to an extension: the file the host loads.

struct multiapp_trailer (include/multiapp/multiapp.h), little endian: magic,
the ELF's size and CRC-32 (zlib's, crc32_ieee() on the target), the stack its
main() needs and the build id (buildid.py).

A hot-swap kernel's file (--kernel <placement>) has the same layout with the magic "MKT1" and the
placement (0 PSRAM, 1 internal RAM: enum multiapp_place in src/apps.h) where the stack is.
"""

import argparse
import struct
import sys
import zlib

TRAILER = struct.Struct("<4I16s")
MAGIC = 0x3150414D  # "MAP1"
KERNEL_MAGIC = 0x31544B4D  # "MKT1"
PLACEMENTS = {"psram": 0, "internal": 1}


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--stack", type=int, help="stack of the app's main(), bytes")
    p.add_argument("--kernel", choices=sorted(PLACEMENTS),
                   help="a hot-swap kernel, placed in PSRAM or internal RAM (not with --stack)")
    p.add_argument("--build-id", required=True, help="the 16-byte build id (buildid.py)")
    p.add_argument("elf", help="the extension (.llext)")
    p.add_argument("out", help="the extension with the trailer")
    a = p.parse_args()
    if (a.stack is None) == (a.kernel is None):
        p.error("exactly one of --stack (an app) and --kernel (a kernel) is needed")
    elf = open(a.elf, "rb").read()
    build_id = open(a.build_id, "rb").read()
    if len(build_id) != 16:
        sys.exit(f"apptrail.py: {a.build_id}: not a 16-byte build id")
    if a.kernel is not None:
        trailer = TRAILER.pack(KERNEL_MAGIC, len(elf), zlib.crc32(elf), PLACEMENTS[a.kernel],
                               build_id)
    else:
        trailer = TRAILER.pack(MAGIC, len(elf), zlib.crc32(elf), a.stack, build_id)
    with open(a.out, "wb") as f:
        f.write(elf + trailer)
    return 0


if __name__ == "__main__":
    sys.exit(main())
