# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/multiapp/apptrail.py: the trailer the multiapp host checks before it loads an app."""

import hashlib
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


def test_kernel_trailer(tmp_path, run_main):
    # A kernel: the same layout, "MKT1", and the placement where an app has its stack.
    elf, bid = tmp_path / "k.llext", tmp_path / "id.bin"
    elf.write_bytes(b"\x7fELF" + bytes(64))
    bid.write_bytes(bytes(range(16)))
    for placement, value in (("psram", 0), ("internal", 1)):
        out = tmp_path / f"k.{placement}"
        assert run_main(apptrail, "--kernel", placement, "--build-id", bid, elf, out) == 0
        data = out.read_bytes()
        magic, size, crc, place, build_id = struct.unpack("<4I16s", data[-32:])
        assert magic.to_bytes(4, "little") == b"MKT1"
        assert (size, crc, place, build_id) == (len(data) - 32, zlib.crc32(data[:-32]), value,
                                                 bytes(range(16)))


def test_app_or_kernel_not_both(tmp_path, run_main):
    elf, bid = tmp_path / "x.llext", tmp_path / "id.bin"
    elf.write_bytes(b"x")
    bid.write_bytes(bytes(16))
    for extra in ((), ("--stack", 1, "--kernel", "psram")):
        with pytest.raises(SystemExit):
            run_main(apptrail, *extra, "--build-id", bid, elf, tmp_path / "out")


def test_signature_block(tmp_path, run_main):
    """--key appends the ED25519 signature over ELF and trailer, then "SIG1"; the trailer stays where it was."""
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ed25519

    key = ed25519.Ed25519PrivateKey.generate()
    pem = tmp_path / "k.pem"
    pem.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                      serialization.NoEncryption()))
    elf, bid, out = tmp_path / "app.llext", tmp_path / "id.bin", tmp_path / "app.sig"
    elf.write_bytes(b"\x7fELF" + bytes(range(200)))
    bid.write_bytes(bytes(16))
    assert run_main(apptrail, "--key", pem, "--name", "hello", "--stack", 1024, "--build-id", bid, elf, out) == 0
    data = out.read_bytes()
    assert data[-4:] == b"SIG1" and len(data) == len(elf.read_bytes()) + 32 + 64 + 4
    signed, sig = data[:-68], data[-68:-4]
    assert signed[-32:-28] == b"MAP1"

    def digest(name, payload):
        return hashlib.sha256(b"MAPSIG1\0" + name + b"\0" + payload).digest()

    key.public_key().verify(sig, digest(b"hello", signed))  # raises when wrong
    # a flipped bit in the ELF or in the trailer's stack field, or another name, breaks it
    for pos in (10, len(signed) - 16):
        bad = bytearray(signed)
        bad[pos] ^= 1
        with pytest.raises(Exception):
            key.public_key().verify(sig, digest(b"hello", bytes(bad)))
    with pytest.raises(Exception):
        key.public_key().verify(sig, digest(b"capture", signed))
    with pytest.raises(SystemExit):
        run_main(apptrail, "--key", pem, "--stack", 1, "--build-id", bid, elf, out)


def test_unsigned_file_has_no_block(tmp_path, run_main):
    elf, bid, out = tmp_path / "a", tmp_path / "id", tmp_path / "o"
    elf.write_bytes(b"x" * 10)
    bid.write_bytes(bytes(16))
    run_main(apptrail, "--stack", 1, "--build-id", bid, elf, out)
    assert len(out.read_bytes()) == 10 + 32


def test_signed_kernel(tmp_path, run_main):
    """--kernel takes --key and --name too: the signature covers the MKT1 trailer under the kernel's name."""
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ed25519

    key = ed25519.Ed25519PrivateKey.generate()
    pem = tmp_path / "k.pem"
    pem.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                      serialization.NoEncryption()))
    elf, bid, out = tmp_path / "k.llext", tmp_path / "id.bin", tmp_path / "k.sig"
    elf.write_bytes(b"\x7fELF" + bytes(range(100)))
    bid.write_bytes(bytes(16))
    assert run_main(apptrail, "--key", pem, "--name", "hello_op", "--kernel", "internal", "--build-id", bid,
                    elf, out) == 0
    data = out.read_bytes()
    assert data[-4:] == b"SIG1"
    signed, sig = data[:-68], data[-68:-4]
    assert signed[-32:-28] == b"MKT1"
    digest = hashlib.sha256(b"MAPSIG1\0hello_op\0" + signed).digest()
    key.public_key().verify(sig, digest)
