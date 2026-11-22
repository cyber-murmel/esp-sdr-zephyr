# SPDX-License-Identifier: GPL-3.0-or-later
"""The host's check of extension files: MCUboot's ED25519 verifier (ext/fiat curve25519.c, tinycrypt SHA-512)
and apps/multiapp/src/extsig_core.c (strip the block, digest of name and file), built for the PC. RFC 8032
section 7.1 test 1 for the verifier; signatures made by apptrail.py (cryptography) for the rest."""

import ctypes
import shutil
import subprocess

import pytest
from conftest import REPO, load

MCUBOOT = REPO.parents[2] / "bootloader" / "mcuboot"
pubkey = load("apps/multiapp/pubkey.py")
apptrail = load("apps/multiapp/apptrail.py")

# RFC 8032, 7.1, TEST 1 (empty message)
PUB = bytes.fromhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a")
SIG = bytes.fromhex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
                    "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b")


class Lib:
    def __init__(self, so):
        c = ctypes.CDLL(str(so))
        c.ED25519_verify.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_char_p]
        c.extsig_strip.restype = ctypes.c_void_p
        c.extsig_strip.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_size_t)]
        c.extsig_digest.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p]
        c.extsig_needed.argtypes = [ctypes.c_char_p]
        self.c = c

    def verify(self, msg, sig, pub):
        return self.c.ED25519_verify(msg, len(msg), sig, pub) == 1

    def strip(self, data):
        buf = ctypes.create_string_buffer(data, len(data))
        n = ctypes.c_size_t(len(data))
        p = self.c.extsig_strip(buf, ctypes.byref(n))
        return (None if p is None else p - ctypes.addressof(buf)), n.value

    def digest(self, name, data):
        out = ctypes.create_string_buffer(32)
        self.c.extsig_digest(name.encode(), data, len(data), out)
        return out.raw

    def needed(self, source):
        return self.c.extsig_needed(source.encode()) != 0


@pytest.fixture(scope="module")
def lib(tmp_path_factory):
    cc = shutil.which("gcc") or shutil.which("cc")
    if cc is None or not (MCUBOOT / "ext/fiat/src/curve25519.c").exists():
        pytest.skip("no C compiler or no MCUboot tree")
    tmp = tmp_path_factory.mktemp("ed")
    out = tmp / "ed.so"
    # The flash map and config headers are the target's: empty stand-ins; what the verifier calls of
    # bootutil_public.h is a constant time compare
    (tmp / "flash_map_backend").mkdir()
    (tmp / "flash_map_backend/flash_map_backend.h").write_text("")
    (tmp / "mcuboot_config").mkdir()
    (tmp / "mcuboot_config/mcuboot_config.h").write_text("")
    (tmp / "stub.c").write_text("#include <stdint.h>\nint boot_secure_memequal(const void *a, const void *b, "
                                "unsigned long n){const uint8_t *x=a,*y=b;uint8_t d=0;"
                                "while(n--)d|=*x++^*y++;return d;}\n")
    src = [MCUBOOT / "ext/fiat/src/curve25519.c", MCUBOOT / "ext/tinycrypt-sha512/lib/source/sha512.c",
           MCUBOOT / "ext/tinycrypt/lib/source/utils.c", MCUBOOT / "ext/tinycrypt/lib/source/sha256.c",
           REPO / "apps/multiapp/src/extsig_core.c"]
    inc = [MCUBOOT / p for p in ("ext/fiat/src", "ext/tinycrypt/lib/include",
                                 "ext/tinycrypt-sha512/lib/include", "boot/bootutil/include")]
    inc.append(REPO / "apps/multiapp/src")
    r = subprocess.run([cc, "-shared", "-fPIC", "-O2", "-DNDEBUG", "-DMULTIAPP_EXT_VERIFY", "-o", out,
                        *map(str, src), str(tmp / "stub.c"), f"-I{tmp}", *[f"-I{i}" for i in inc]],
                       capture_output=True, text=True)
    if r.returncode != 0:
        pytest.skip("the verifier does not build for the PC: " + r.stderr[-300:])
    return Lib(out)


def test_rfc8032_vector(lib):
    assert lib.verify(b"", SIG, PUB)
    assert not lib.verify(b"x", SIG, PUB)
    bad = bytearray(SIG)
    bad[40] ^= 1
    assert not lib.verify(b"", bytes(bad), PUB)


def signed(tmp_path, run_main, name="hello"):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ed25519

    key = ed25519.Ed25519PrivateKey.generate()
    pem = tmp_path / "k.pem"
    pem.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                      serialization.NoEncryption()))
    elf, bid, out, hdr = (tmp_path / n for n in ("a.llext", "id", "o", "pub.h"))
    elf.write_bytes(b"\x7fELF" + bytes(range(256)) * 20)
    bid.write_bytes(bytes(16))
    run_main(apptrail, "--key", pem, "--name", name, "--stack", 2048, "--build-id", bid, elf, out)
    run_main(pubkey, pem, hdr)
    pub = bytes(int(x, 16) for x in hdr.read_text().split("{")[1].split("}")[0].split(","))
    assert len(pub) == 32
    return out.read_bytes(), pub


def test_multiapp_sig_check_logic(lib, tmp_path, run_main):
    """What multiapp_sig_check() does with a file, minus the logging and the worker thread."""
    data, pub = signed(tmp_path, run_main)
    block, n = lib.strip(data)
    assert block == len(data) - 68 and n == len(data) - 68          # the block comes off, the length shrinks
    sig = data[block:block + 64]
    ok = lambda name: lib.verify(lib.digest(name, data[:n]), sig, pub)
    assert ok("hello")
    assert not ok("capture") and not ok("sdr")                      # bound to the name
    assert not lib.verify(lib.digest("hello", data[:n - 1] + b"\x01"), sig, pub)   # bound to the content
    assert not lib.verify(lib.digest("hello", data[:n]), bytes(64), pub)
    # no SIG1: nothing is stripped, and the caller refuses it
    assert lib.strip(data[:-68]) == (None, len(data) - 68)
    assert lib.strip(b"SIG1") == (None, 4)
    # only the image's own copies go unchecked
    assert not lib.needed("built-in")
    assert lib.needed("file") and lib.needed("shell") and lib.needed("")
