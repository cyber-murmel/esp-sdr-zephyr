# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of the repository's Python tools (pytest, CPython): see doc/testing.md."""

import ctypes
import importlib.util
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]


def load(relpath, name=None):
    """Import a tool by its path (the scripts are not packages; some names have dashes)."""
    path = REPO / relpath
    name = name or path.stem.replace("-", "_")
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture
def run_main(monkeypatch):
    """Call a tool's main() with the given command line arguments."""
    def run(mod, *args):
        monkeypatch.setattr(sys, "argv", [mod.__name__, *map(str, args)])
        return mod.main()
    return run


@pytest.fixture(scope="session")
def hb_clib(tmp_path_factory):
    """A burst's half-band filter as the board runs it (apps/micropython/cmod/esdr_hb.c), built
    once with the host compiler; None without a compiler."""
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        return None
    out = tmp_path_factory.mktemp("hb") / "libesdr_hb.so"
    src = REPO / "apps/micropython/cmod/esdr_hb.c"
    subprocess.run([cc, "-O2", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC", "-o", str(out),
                    str(src)], check=True)
    lib = ctypes.CDLL(str(out))
    lib.esdr_hb_block.restype = ctypes.c_uint32
    lib.esdr_hb_block.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32,
                                  ctypes.c_bool, ctypes.c_void_p]
    return lib


@pytest.fixture(scope="session", params=["int32", "int16"])
def scan_clib(request, tmp_path_factory):
    """The Scan app's measurement as the boards run it (apps/micropython/cmod/esdr_scan.c), built
    once each way with the host compiler: the int32 FFT (the P4, the S31), and the S3's 16-bit
    one with esp-dsp's kernel in C; None without a compiler. lib.kind tells which."""
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        return None
    out = tmp_path_factory.mktemp("scan") / "libesdr_scan_{}.so".format(request.param)
    src = REPO / "apps/micropython/cmod/esdr_scan.c"
    defs = ["-DESDR_SCAN_FFT16", "-DESDR_SCAN_FFT16_MODEL"] if request.param == "int16" else []
    subprocess.run([cc, "-O2", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC", *defs, "-o",
                    str(out), str(src), "-lm"], check=True)
    lib = ctypes.CDLL(str(out))
    lib.kind = request.param
    lib.esdr_scan_init.restype = None
    lib.esdr_scan_init.argtypes = [ctypes.c_void_p]
    lib.esdr_scan_spectrum.restype = ctypes.c_uint32
    lib.esdr_scan_spectrum.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32,
                                       ctypes.c_uint32, ctypes.c_void_p, ctypes.c_void_p,
                                       ctypes.c_void_p]
    lib.esdr_scan_fold.restype = ctypes.c_int
    lib.esdr_scan_fold.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                                   ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p,
                                   ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                                   ctypes.c_uint32, ctypes.c_float, ctypes.c_float,
                                   ctypes.c_void_p, ctypes.c_void_p]
    lib.esdr_scan_q8.restype = None
    lib.esdr_scan_q8.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32]
    return lib


class FskFrame(ctypes.Structure):
    """struct esdr_fsk_frame of apps/micropython/cmod/esdr_fsk.h."""
    _fields_ = [("len", ctypes.c_uint32), ("errors", ctypes.c_uint32), ("end", ctypes.c_uint32),
                ("phase", ctypes.c_uint32), ("dc", ctypes.c_double), ("amp", ctypes.c_double),
                ("var", ctypes.c_double), ("power", ctypes.c_double), ("floor", ctypes.c_uint32),
                ("data", ctypes.c_uint8 * 255)]


FSK_EMIT = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.POINTER(FskFrame))


@pytest.fixture(scope="session")
def fsk_clib(tmp_path_factory):
    """The C modem the board runs (apps/micropython/cmod/esdr_fsk.c), built once with the host
    compiler, its prototypes set; None without a compiler."""
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        return None
    out = tmp_path_factory.mktemp("fsk") / "libesdr_fsk.so"
    src = REPO / "apps/micropython/cmod/esdr_fsk.c"
    subprocess.run([cc, "-O2", "-Wall", "-Werror", "-shared", "-fPIC", "-o", str(out), str(src),
                    "-lm"], check=True)
    lib = ctypes.CDLL(str(out))
    lib.esdr_fsk_rx_size.restype = ctypes.c_size_t
    lib.esdr_fsk_rx_size.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
    lib.esdr_fsk_rx_init.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32,
                                     ctypes.c_uint64, ctypes.c_uint32, ctypes.c_uint32,
                                     ctypes.c_uint32]
    lib.esdr_fsk_rx_reset.argtypes = [ctypes.c_void_p]
    lib.esdr_fsk_rx_process.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                        FSK_EMIT, ctypes.c_void_p]
    lib.esdr_fsk_rx_state.restype = ctypes.c_uint32
    lib.esdr_fsk_rx_state.argtypes = [ctypes.c_void_p]
    lib.esdr_fsk_angle.argtypes = [ctypes.c_int32, ctypes.c_int32]
    lib.esdr_fsk_angle.restype = ctypes.c_int32
    lib.esdr_fsk_mod.restype = ctypes.c_size_t
    lib.esdr_fsk_mod.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint32,
                                 ctypes.POINTER(ctypes.c_float), ctypes.c_size_t, ctypes.c_float,
                                 ctypes.c_float, ctypes.c_void_p, ctypes.c_size_t,
                                 ctypes.POINTER(ctypes.c_float)]
    return lib
