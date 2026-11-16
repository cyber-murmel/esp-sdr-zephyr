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


def generate(tmp_path, run_main, *libs, area=4096, modules=()):
    files = {k: tmp_path / f"{k}.c" for k in ("exports", "logs", "objects")}
    module_args = [arg for name in modules for arg in ("--module", name)]
    run_main(exports, "--exports", files["exports"], "--logs", files["logs"],
             "--objects", files["objects"], "--area-size", area, *module_args, *libs)
    return {k: p.read_text() for k, p in files.items()}


@pytest.fixture
def two_libs(tmp_path):
    """An app importing mod_call and k_yield, and a module defining mod_call."""
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        pytest.skip("no host C compiler")
    built = []
    for name, code in (("app", "extern int mod_call(void); extern void k_yield(void);"
                               "int app_main(void) { k_yield(); return mod_call(); }"),
                       ("mod", "extern void k_sleep(void); int mod_call(void) { k_sleep(); return 1; }")):
        src = tmp_path / f"{name}.c"
        src.write_text(code)
        out = tmp_path / f"lib{name}_ext_llext_lib.so"
        subprocess.run([cc, "-shared", "-fPIC", "-o", out, src], check=True)
        built.append(out)
    return built


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


def test_defines(two_libs):
    assert "mod_call" in exports.defines(two_libs[1])
    assert "mod_call" not in exports.defines(two_libs[0])


def test_module_symbols_are_not_the_hosts(tmp_path, run_main, two_libs):
    # Named as a module, what it defines comes from it; what it imports still from the host.
    out = generate(tmp_path, run_main, *two_libs, modules=("mod",))["exports"]
    assert '__asm__("mod_call")' not in out
    assert '__asm__("k_yield")' in out and '__asm__("k_sleep")' in out


def test_app_symbols_stay_private(tmp_path, run_main, two_libs):
    # Not a module: another extension's import of the name is still the host's to provide.
    out = generate(tmp_path, run_main, *two_libs)["exports"]
    assert '__asm__("mod_call")' in out


def test_psram_object(tmp_path, run_main, lib, monkeypatch):
    monkeypatch.setattr(exports, "host_objects",
                        lambda path: iter([("demo_ring", 3, [4096, 4, 0])]))
    out = generate(tmp_path, run_main, lib)
    assert "EXT_RAM_BSS_ATTR uint8_t demo_ring[4096] __aligned(4);" in out["objects"]
    assert "EXPORT_GROUP_SYMBOL(MULTIAPP, demo_ring);" in out["objects"]


def test_unknown_kind(tmp_path, run_main, lib, monkeypatch):
    monkeypatch.setattr(exports, "host_objects", lambda path: iter([("odd", 7, [0, 0, 0])]))
    with pytest.raises(SystemExit):
        generate(tmp_path, run_main, lib)
