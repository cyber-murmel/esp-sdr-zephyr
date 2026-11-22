# SPDX-License-Identifier: GPL-3.0-or-later
"""The SSH server's key code of the multiapp host (apps/multiapp/src/authkeys.c: authorized_keys lines,
OpenSSH fingerprint, the DER the server imports) and its console registry (console_mux_core.c), built for the
PC and checked against OpenSSH's own output (cryptography, ssh-keygen)."""

import base64
import ctypes
import hashlib
import shutil
import subprocess

import pytest
from conftest import REPO

SRC = REPO / "apps/multiapp/src"
INC = REPO / "apps/multiapp/include"

# struct ak_key (authkeys.h)
AK_BLOB_MAX, AK_TYPE_MAX, AK_COMMENT_MAX, AK_FP_LEN = 600, 40, 48, 51
AK_SKIP, AK_E_SYNTAX, AK_E_OPTIONS, AK_E_BLOB, AK_E_SIZE = 1, -1, -2, -3, -4
AK_KIND_OTHER, AK_KIND_RSA, AK_KIND_ECDSA_P256, AK_KIND_ED25519 = 0, 1, 2, 3


class AkKey(ctypes.Structure):
    _fields_ = [("type", ctypes.c_char * AK_TYPE_MAX), ("kind", ctypes.c_int), ("blob", ctypes.c_uint8 * AK_BLOB_MAX),
                ("blob_len", ctypes.c_size_t), ("comment", ctypes.c_char * AK_COMMENT_MAX),
                ("bits", ctypes.c_uint), ("supported", ctypes.c_bool), ("why", ctypes.c_char_p)]


class Console(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char_p), ("open", ctypes.c_void_p), ("rx_ready", ctypes.c_void_p),
                ("tx_ready", ctypes.c_void_p), ("closed", ctypes.c_void_p),
                ("input_empty", ctypes.c_void_p)]


STUBS = """
#include "console_mux_core.h"
int locks, removed;
const struct multiapp_console *last_removed;
void cmux_lock(void) { locks++; }
void cmux_unlock(void) { locks--; }
void cmux_removed(const struct multiapp_console *c) { removed++; last_removed = c; }
"""


@pytest.fixture(scope="module")
def lib(tmp_path_factory):
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        pytest.skip("no C compiler")
    tmp = tmp_path_factory.mktemp("sshkeys")
    (tmp / "stubs.c").write_text(STUBS)
    so = tmp / "sshkeys.so"
    r = subprocess.run([cc, "-shared", "-fPIC", "-O1", "-Wall", "-Wextra", "-Werror", "-o", str(so),
                        str(SRC / "authkeys.c"), str(SRC / "console_mux_core.c"), str(tmp / "stubs.c"),
                        f"-I{SRC}", f"-I{INC}"], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    c = ctypes.CDLL(str(so))
    c.ak_parse_line.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(AkKey)]
    c.ak_strerror.restype = ctypes.c_char_p
    c.ak_fingerprint.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p]
    c.ak_from_ecdsa_point.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(AkKey)]
    c.ak_raw_pubkey.argtypes = [ctypes.POINTER(AkKey), ctypes.POINTER(ctypes.c_void_p)]
    c.ak_sha256.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p]
    c.multiapp_console_register.argtypes = [ctypes.POINTER(Console)]
    c.multiapp_console_unregister.argtypes = [ctypes.POINTER(Console)]
    c.cmux_pick.argtypes = [ctypes.c_int]
    c.cmux_pick.restype = ctypes.c_void_p
    c.cmux_current.restype = ctypes.c_void_p
    return c


def parse(lib, line):
    k = AkKey()
    data = line if isinstance(line, bytes) else line.encode()
    return lib.ak_parse_line(data, len(data), ctypes.byref(k)), k


def fingerprint(lib, blob):
    out = ctypes.create_string_buffer(AK_FP_LEN)
    lib.ak_fingerprint(blob, len(blob), out)
    return out.value.decode()


def b64_nopad(raw):
    return base64.b64encode(raw).decode().rstrip("=")


# ---- keys (cryptography, as OpenSSH writes them) ----

try:
    from cryptography.hazmat.primitives import serialization as ser
    from cryptography.hazmat.primitives.asymmetric import ec, ed25519, rsa
except ImportError:  # pragma: no cover
    ser = None


def openssh_line(key, comment=""):
    raw = key.public_key().public_bytes(ser.Encoding.OpenSSH, ser.PublicFormat.OpenSSH).decode()
    return raw + (" " + comment if comment else "")


@pytest.fixture(scope="module")
def rsa_keys():
    if ser is None:
        pytest.skip("no cryptography")
    return {bits: rsa.generate_private_key(65537, bits) for bits in (1024, 2048, 3072, 4096)}


# ---- SHA-256 and fingerprints ----

@pytest.mark.parametrize("n", [0, 1, 3, 55, 56, 57, 63, 64, 65, 119, 120, 128, 1000])
def test_sha256(lib, n):
    data = bytes((i * 31 + 7) & 0xFF for i in range(n))
    out = ctypes.create_string_buffer(32)
    lib.ak_sha256(data, n, out)
    assert out.raw == hashlib.sha256(data).digest()


def test_fingerprint_format(lib):
    blob = bytes(range(40))
    fp = fingerprint(lib, blob)
    assert fp == "SHA256:" + b64_nopad(hashlib.sha256(blob).digest())
    assert len(fp) == AK_FP_LEN - 1 and "=" not in fp


@pytest.mark.skipif(shutil.which("ssh-keygen") is None, reason="no ssh-keygen")
def test_fingerprint_matches_ssh_keygen(lib, rsa_keys, tmp_path):
    line = openssh_line(rsa_keys[2048], "me@pc")
    f = tmp_path / "k.pub"
    f.write_text(line + "\n")
    shown = subprocess.run(["ssh-keygen", "-lf", str(f)], capture_output=True, text=True, check=True).stdout
    rc, k = parse(lib, line)
    assert rc == 0
    assert fingerprint(lib, bytes(k.blob[:k.blob_len])) == shown.split()[1]
    assert f"{k.bits}" == shown.split()[0]


# ---- authorized_keys lines ----

@pytest.mark.parametrize("bits", [1024, 2048, 3072, 4096])
def test_parse_rsa_is_listed_but_unsupported(lib, rsa_keys, bits):
    # The build has no RSA: the line is parsed (type, size, fingerprint) and not used
    rc, k = parse(lib, openssh_line(rsa_keys[bits], "user@host"))
    assert rc == 0 and not k.supported and k.kind == AK_KIND_RSA
    assert b"RSA keys are not supported" in k.why
    assert k.type == b"ssh-rsa" and k.bits == bits and k.comment == b"user@host"
    assert bytes(k.blob[:k.blob_len]) == base64.b64decode(openssh_line(rsa_keys[bits]).split()[1])
    assert lib.ak_raw_pubkey(ctypes.byref(k), ctypes.byref(ctypes.c_void_p())) < 0


def test_parse_line_ends_and_blanks(lib):
    if ser is None:
        pytest.skip("no cryptography")
    line = openssh_line(ed25519.Ed25519PrivateKey.generate(), "c")
    for ending in ("\n", "\r\n", "  \t", ""):
        rc, k = parse(lib, "  \t" + line + ending)
        assert rc == 0 and k.supported and k.comment == b"c", ending
    for text in ("", "\n", "   \r\n", "# a comment", "   # indented comment"):
        assert parse(lib, text)[0] == AK_SKIP, text


def test_parse_comment_with_spaces_and_cut(lib, rsa_keys):
    rc, k = parse(lib, openssh_line(rsa_keys[2048], "my laptop in the lab"))
    assert rc == 0 and k.comment == b"my laptop in the lab"
    rc, k = parse(lib, openssh_line(rsa_keys[2048], "x" * 200))
    assert rc == 0 and k.comment == b"x" * (AK_COMMENT_MAX - 1)


def raw_of(lib, k):
    p = ctypes.c_void_p()
    n = lib.ak_raw_pubkey(ctypes.byref(k), ctypes.byref(p))
    return n, (ctypes.string_at(p.value, n) if n > 0 else None)


def test_parse_ed25519(lib):
    if ser is None:
        pytest.skip("no cryptography")
    key = ed25519.Ed25519PrivateKey.generate()
    rc, k = parse(lib, openssh_line(key, "ed"))
    assert rc == 0 and k.supported and k.why is None and k.kind == AK_KIND_ED25519
    assert k.type == b"ssh-ed25519" and k.bits == 256 and k.comment == b"ed"
    n, raw = raw_of(lib, k)
    assert n == 32 and raw == key.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)
    assert fingerprint(lib, bytes(k.blob[:k.blob_len])).startswith("SHA256:")


def test_parse_ecdsa_p256(lib):
    if ser is None:
        pytest.skip("no cryptography")
    key = ec.generate_private_key(ec.SECP256R1())
    rc, k = parse(lib, openssh_line(key, "ec"))
    assert rc == 0 and k.supported and k.kind == AK_KIND_ECDSA_P256 and k.type == b"ecdsa-sha2-nistp256"
    n, raw = raw_of(lib, k)
    assert n == 65 and raw == key.public_key().public_bytes(ser.Encoding.X962, ser.PublicFormat.UncompressedPoint)


def test_parse_other_curves_and_types_are_listed_but_unsupported(lib):
    if ser is None:
        pytest.skip("no cryptography")
    for key, name in ((ec.generate_private_key(ec.SECP384R1()), b"ecdsa-sha2-nistp384"),
                      (ec.generate_private_key(ec.SECP521R1()), b"ecdsa-sha2-nistp521")):
        rc, k = parse(lib, openssh_line(key))
        assert rc == 0 and not k.supported and k.type == name and b"ecdsa-sha2-nistp256" in k.why
        assert fingerprint(lib, bytes(k.blob[:k.blob_len])).startswith("SHA256:")
        assert lib.ak_raw_pubkey(ctypes.byref(k), ctypes.byref(ctypes.c_void_p())) < 0


def _blob(parts):
    import struct
    return b"".join(struct.pack(">I", len(p)) + p for p in parts)


def _line(type_, parts, tail=b""):
    return type_ + " " + base64.b64encode(_blob(parts) + tail).decode() + " c"


def test_parse_ed25519_key_length_and_trailing_bytes(lib):
    for n in (31, 33, 0, 64):
        rc, k = parse(lib, _line("ssh-ed25519", [b"ssh-ed25519", bytes(n)]))
        assert rc == 0 and k.supported is False or rc < 0, n   # a bad key is not usable
    rc, k = parse(lib, _line("ssh-ed25519", [b"ssh-ed25519", b"\x01" * 32]))
    assert rc == 0 and k.supported
    rc, k = parse(lib, _line("ssh-ed25519", [b"ssh-ed25519", b"\x01" * 32], tail=b"\0\0\0"))
    assert rc < 0 or not k.supported, "trailing bytes in the blob"


def test_parse_ecdsa_wrong_curve_and_point(lib):
    point = b"\x04" + b"\x11" * 64
    rc, k = parse(lib, _line("ecdsa-sha2-nistp256", [b"ecdsa-sha2-nistp256", b"nistp256", point]))
    assert rc == 0 and k.supported
    for parts in ([b"ecdsa-sha2-nistp256", b"nistp384", point],
                  [b"ecdsa-sha2-nistp256", b"nistp256", b"\x03" + b"\x11" * 64],
                  [b"ecdsa-sha2-nistp256", b"nistp256", point[:-1]],
                  [b"ecdsa-sha2-nistp256", b"nistp256", point + b"\0"]):
        rc, k = parse(lib, _line("ecdsa-sha2-nistp256", parts))
        assert rc < 0 or not k.supported, parts


def test_parse_base64_padding_in_the_middle_and_control_bytes(lib):
    good = _line("ssh-ed25519", [b"ssh-ed25519", b"\x01" * 32]).split()
    assert parse(lib, "ssh-ed25519 " + good[1][:20] + "=" + good[1][20:])[0] == AK_E_BLOB
    for bad in ("ssh-ed25519 " + good[1] + " com\nment", "ssh-ed25519 " + good[1] + " c\x01", "ssh-ed25519\x00 " + good[1],
                "ssh-ed25519 " + good[1] + " c\x7f"):
        assert parse(lib, bad.encode())[0] == AK_E_SYNTAX, repr(bad)
    # a tab in the comment is fine, a CR before the line end is a line end
    assert parse(lib, "ssh-ed25519 " + good[1] + " a\tb\r\n")[0] == 0


def test_raw_pubkey_checks_the_blob(lib):
    if ser is None:
        pytest.skip("no cryptography")
    rc, k = parse(lib, openssh_line(ec.generate_private_key(ec.SECP256R1())))
    k2 = AkKey.from_buffer_copy(k)
    k2.blob[k2.blob_len - 65] = 0x05   # the point's first byte: not uncompressed
    assert raw_of(lib, k2)[0] < 0
    k3 = AkKey.from_buffer_copy(k)
    k3.blob_len -= 1
    assert raw_of(lib, k3)[0] < 0
    rc, r = parse(lib, openssh_line(rsa.generate_private_key(65537, 2048)))
    assert raw_of(lib, r)[0] < 0, "RSA has no raw key"


def test_from_ecdsa_point_is_the_wire_blob(lib):
    if ser is None:
        pytest.skip("no cryptography")
    key = ec.generate_private_key(ec.SECP256R1())
    point = key.public_key().public_bytes(ser.Encoding.X962, ser.PublicFormat.UncompressedPoint)
    k = AkKey()
    assert lib.ak_from_ecdsa_point(point, len(point), ctypes.byref(k)) == 0
    assert bytes(k.blob[:k.blob_len]) == base64.b64decode(openssh_line(key).split()[1])
    assert k.type == b"ecdsa-sha2-nistp256" and k.supported and k.kind == AK_KIND_ECDSA_P256
    assert lib.ak_from_ecdsa_point(point[:64], 64, ctypes.byref(k)) < 0
    assert lib.ak_from_ecdsa_point(b"\x05" + point[1:], 65, ctypes.byref(k)) < 0


@pytest.mark.skipif(shutil.which("ssh-keygen") is None, reason="no ssh-keygen")
def test_fingerprint_of_other_key_types_matches_ssh_keygen(lib, tmp_path):
    for key in (ed25519.Ed25519PrivateKey.generate(), ec.generate_private_key(ec.SECP256R1())):
        f = tmp_path / "k.pub"
        f.write_text(openssh_line(key) + "\n")
        shown = subprocess.run(["ssh-keygen", "-lf", str(f)], capture_output=True, text=True, check=True).stdout
        rc, k = parse(lib, openssh_line(key))
        assert fingerprint(lib, bytes(k.blob[:k.blob_len])) == shown.split()[1]


@pytest.mark.parametrize("line", [
    'command="ls" ssh-rsa AAAA x',
    'from="10.0.0.1" ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIA== x',
    "restrict ssh-rsa AAAA",
    "no-pty,command=\"x\" ssh-rsa AAAA",
    "garbage",
])
def test_parse_options_are_refused(lib, line):
    assert parse(lib, line)[0] == AK_E_OPTIONS


def test_parse_syntax_and_blob_errors(lib, rsa_keys):
    good = openssh_line(rsa_keys[2048]).split()
    assert parse(lib, "ssh-rsa")[0] == AK_E_SYNTAX
    assert parse(lib, "ssh-rsa    ")[0] == AK_E_SYNTAX
    assert parse(lib, "ssh-rsa not*base64!")[0] == AK_E_BLOB
    assert parse(lib, "ssh-rsa A")[0] == AK_E_BLOB
    assert parse(lib, "ssh-rsa " + good[1][:60])[0] == AK_E_BLOB          # cut off
    assert parse(lib, "ssh-rsa " + good[1][:-8] + "AAAA")[0] == AK_E_BLOB  # tail replaced: e, n lengths broken
    assert parse(lib, "ssh-dss " + good[1])[0] == AK_E_BLOB                # the blob says ssh-rsa
    assert parse(lib, "ssh-rsa " + b64_nopad(b"\x00" * 700))[0] == AK_E_SIZE
    assert parse(lib, "x" * 200 + " AAAA")[0] == AK_E_OPTIONS
    assert parse(lib, "ssh-" + "x" * 60 + " AAAA")[0] == AK_E_SYNTAX
    for e in (AK_E_SYNTAX, AK_E_OPTIONS, AK_E_BLOB, AK_E_SIZE):
        assert lib.ak_strerror(e)


# ---- the console registry (console_mux_core.c) ----

CMUX_SHELL, CMUX_EXEC = 0, 1


def make_console(name=b"repl", complete=True):
    cb = ctypes.CFUNCTYPE(None)
    fn = cb(lambda: None)
    c = Console(name, ctypes.cast(fn, ctypes.c_void_p).value, ctypes.cast(fn, ctypes.c_void_p).value,
                None, ctypes.cast(fn, ctypes.c_void_p).value)
    if not complete:
        c.open = None
    return c, fn  # keep fn alive


def test_console_default_is_the_zephyr_shell(lib):
    assert lib.cmux_current() is None
    assert lib.cmux_pick(CMUX_SHELL) is None and lib.cmux_pick(CMUX_EXEC) is None


def test_console_register_pick_unregister(lib):
    c, keep = make_console()
    assert lib.multiapp_console_register(ctypes.byref(c)) == 0
    assert lib.cmux_current() == ctypes.addressof(c)
    # a shell request goes to the registered console, an exec request (ssh host shell) never
    assert lib.cmux_pick(CMUX_SHELL) == ctypes.addressof(c)
    assert lib.cmux_pick(CMUX_EXEC) is None
    other, keep2 = make_console(b"other")
    import errno
    assert lib.multiapp_console_register(ctypes.byref(other)) == -errno.EBUSY
    assert lib.multiapp_console_unregister(ctypes.byref(other)) == -errno.ENOENT
    assert lib.cmux_pick(CMUX_SHELL) == ctypes.addressof(c)
    assert lib.multiapp_console_unregister(ctypes.byref(c)) == 0
    assert lib.cmux_pick(CMUX_SHELL) is None
    assert lib.multiapp_console_unregister(ctypes.byref(c)) == -errno.ENOENT
    # the next one can have it
    assert lib.multiapp_console_register(ctypes.byref(other)) == 0
    assert lib.multiapp_console_unregister(ctypes.byref(other)) == 0
    del keep, keep2


def test_console_register_checks_the_functions(lib):
    import errno
    c, keep = make_console(complete=False)
    assert lib.multiapp_console_register(ctypes.byref(c)) == -errno.EINVAL
    assert lib.multiapp_console_register(None) == -errno.EINVAL
    nameless, keep2 = make_console(name=None)
    assert lib.multiapp_console_register(ctypes.byref(nameless)) == -errno.EINVAL
    assert lib.cmux_current() is None
    del keep, keep2


def test_console_unregister_tells_the_glue_under_the_lock(lib):
    removed = ctypes.c_int.in_dll(lib, "removed")
    locks = ctypes.c_int.in_dll(lib, "locks")
    before = removed.value
    c, keep = make_console()
    assert lib.multiapp_console_register(ctypes.byref(c)) == 0
    assert lib.multiapp_console_unregister(ctypes.byref(c)) == 0
    assert removed.value == before + 1
    assert locks.value == 0, "every lock taken was given back"
    lib.multiapp_console_unregister(ctypes.byref(c))
    assert removed.value == before + 1, "an unregister of nothing closes nothing"
    del keep
