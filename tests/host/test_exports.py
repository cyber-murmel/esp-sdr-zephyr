# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/multiapp/exports.py: what the host exports for its extensions, from a shared library
the host compiler builds here (skipped without one)."""

import shutil
import subprocess

import pytest

from conftest import load

pytest.importorskip("elftools")
exports = load("apps/multiapp/exports.py")

LIB = r"""
struct multiapp_host_object { char name[56]; unsigned int kind, arg[3]; };
__attribute__((section(".multiapp_host"), used))
static const struct multiapp_host_object objs[] = {
	{"demo_pool", 1, {4, 128, 8}},
	{"demo_big", 2, {1000, 4, 0}},
	{"demo_small", 2, {10, 16, 0}},
};
extern int k_sem_take(void *sem, long timeout);
extern const char log_const_demo;
extern char demo_pool;
int demo(void) { return k_sem_take(0, 0) + log_const_demo + demo_pool; }
"""


@pytest.fixture
def lib(tmp_path):
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        pytest.skip("no host C compiler")
    src = tmp_path / "demo.c"
    src.write_text(LIB)
    out = tmp_path / "libdemo_ext_llext_lib.so"
    subprocess.run([cc, "-shared", "-fPIC", "-o", out, src], check=True)
    return out


def generate(tmp_path, run_main, lib, area=4096):
    files = {k: tmp_path / f"{k}.c" for k in ("exports", "logs", "objects")}
    run_main(exports, "--exports", files["exports"], "--logs", files["logs"],
             "--objects", files["objects"], "--area-size", area, lib)
    return {k: p.read_text() for k, p in files.items()}


def test_imports_and_objects(lib):
    names = exports.imports(lib)
    assert {"k_sem_take", "log_const_demo", "demo_pool"} <= names
    assert "demo" not in names
    objs = list(exports.host_objects(lib))
    assert objs == [("demo_pool", 1, [4, 128, 8]), ("demo_big", 2, [1000, 4, 0]),
                    ("demo_small", 2, [10, 16, 0])]


def test_generated_files(tmp_path, run_main, lib):
    out = generate(tmp_path, run_main, lib)
    # Imports are exported by name, except the objects the host defines itself.
    assert 'EXPORT_GROUP_SYMBOL_NAMED(MULTIAPP, multiapp_import_' in out["exports"]
    assert '__asm__("k_sem_take")' in out["exports"]
    assert '"demo_pool"' not in out["exports"]
    assert "_LOG_MODULE_DATA_CREATE(demo, CONFIG_LOG_DEFAULT_LEVEL);" in out["logs"]
    assert "NET_BUF_POOL_FIXED_DEFINE(demo_pool, 4, 128, 8, NULL);" in out["objects"]
    # Area objects packed from the start, each at its alignment: 0, then 1000 up to 16.
    assert "multiapp_area[0], demo_big);" in out["objects"]
    assert "multiapp_area[1008], demo_small);" in out["objects"]
    assert "/* 1018 of 4096 bytes */" in out["objects"]


def test_area_too_small(tmp_path, run_main, lib):
    with pytest.raises(SystemExit):
        generate(tmp_path, run_main, lib, area=1000)


def test_unknown_kind(tmp_path, run_main, lib, monkeypatch):
    monkeypatch.setattr(exports, "host_objects", lambda path: iter([("odd", 7, [0, 0, 0])]))
    with pytest.raises(SystemExit):
        generate(tmp_path, run_main, lib)
