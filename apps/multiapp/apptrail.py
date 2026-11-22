#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Append the multiapp trailer to an extension: the file the host loads.

struct multiapp_trailer (include/multiapp/multiapp.h), little endian: magic,
the ELF's size and CRC-32 (zlib's, crc32_ieee() on the target), the stack its
main() needs and the build id (buildid.py).

A hot-swap kernel's file (--kernel <placement>) has the same layout with the magic "MKT1" and the
placement (0 PSRAM, 1 internal RAM: enum multiapp_place in src/apps.h) where the stack is.

With --key (an ED25519 PEM, the MCUboot signing key) a signature block follows the trailer:
the 64 byte signature (over a digest of the name, ELF and trailer), then "SIG1". The host (src/extsig.c) strips it
before it reads the trailer, and verifies it for every file it did not build in.
"""

import argparse
import struct
import sys
import zlib

TRAILER = struct.Struct("<4I16s")
MAGIC = 0x3150414D  # "MAP1"
KERNEL_MAGIC = 0x31544B4D  # "MKT1"
PLACEMENTS = {"psram": 0, "internal": 1}
SIG_MAGIC = b"SIG1"


def sign_block(key_file, name, payload):
    """Signature block with an ED25519 PEM key (read here only): over the SHA-256 of "MAPSIG1", 0, the name the
    file is for, 0 and payload (ELF and trailer), so that it does not verify under another name."""
    import hashlib

    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ed25519

    with open(key_file, "rb") as f:
        key = serialization.load_pem_private_key(f.read(), password=None)
    if not isinstance(key, ed25519.Ed25519PrivateKey):
        sys.exit(f"apptrail.py: {key_file}: not an ED25519 key")
    digest = hashlib.sha256(b"MAPSIG1\0" + name.encode() + b"\0" + payload).digest()
    return key.sign(digest) + SIG_MAGIC


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--stack", type=int, help="stack of the app's main(), bytes")
    p.add_argument("--kernel", choices=sorted(PLACEMENTS),
                   help="a hot-swap kernel, placed in PSRAM or internal RAM (not with --stack)")
    p.add_argument("--build-id", required=True, help="the 16-byte build id (buildid.py)")
    p.add_argument("--key", help="ED25519 PEM key: append a signature block (the host verifies it)")
    p.add_argument("--name", help="the app, module or kernel name the file is signed for (with --key)")
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
    data = elf + trailer
    if a.key:
        if not a.name:
            sys.exit("apptrail.py: --key needs --name")
        data += sign_block(a.key, a.name, data)
    with open(a.out, "wb") as f:
        f.write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
