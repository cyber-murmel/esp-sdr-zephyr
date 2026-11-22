# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/multiapp/exports.py: what the host exports for its extensions, from a shared library
the host compiler builds here (skipped without one)."""

import re
import shutil
import subprocess
from pathlib import Path

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


def generate(tmp_path, run_main, *libs, area=4096, modules=(), owners=("demo",)):
    files = {k: tmp_path / f"{k}.c" for k in ("exports", "logs", "objects")}
    module_args = [arg for name in modules for arg in ("--module", name)]
    module_args += [arg for name in owners for arg in ("--area-owner", name)]
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


@pytest.fixture
def two_objs(tmp_path):
    """two_libs as relocatable objects (cc -r), as RISC-V extensions are built."""
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        pytest.skip("no host C compiler")
    built = []
    for name, code in (("app", "extern int mod_call(void); extern void k_yield(void);"
                               "static int twice(int v) { return 2 * v; }"
                               "int app_main(void) { k_yield(); return twice(mod_call()); }"),
                       ("mod", "extern void k_sleep(void); int mod_call(void) { k_sleep(); return 1; }")):
        src = tmp_path / f"{name}.c"
        src.write_text(code)
        obj = tmp_path / f"{name}.o"
        subprocess.run([cc, "-c", "-o", obj, src], check=True)
        out = tmp_path / f"{name}_ext_llext_lib.obj"
        subprocess.run([cc, "-r", "-nostdlib", "-o", out, obj], check=True)
        built.append(out)
    return built


def test_relocatable_symbols(two_objs):
    # From the symbol table: what it leaves undefined, what it defines; statics are neither.
    assert {"mod_call", "k_yield"} <= exports.imports(two_objs[0])
    assert "app_main" in exports.defines(two_objs[0])
    assert "twice" not in exports.defines(two_objs[0]) | exports.imports(two_objs[0])
    assert exports.defines(two_objs[1]) >= {"mod_call"}


def test_relocatable_module(tmp_path, run_main, two_objs):
    # The module is known by its object's name, <name>_ext_llext_lib.obj.
    out = generate(tmp_path, run_main, *two_objs, modules=("mod",))["exports"]
    assert '__asm__("mod_call")' not in out
    assert '__asm__("k_yield")' in out and '__asm__("k_sleep")' in out


def test_psram_section(tmp_path, run_main, lib, monkeypatch):
    # Where EXT_RAM_BSS_ATTR places nothing in PSRAM (ESP32-S31), the section by name.
    monkeypatch.setattr(exports, "host_objects",
                        lambda path: iter([("demo_ring", 3, [4096, 4, 0])]))
    files = {k: tmp_path / f"{k}.c" for k in ("exports", "logs", "objects")}
    run_main(exports, "--exports", files["exports"], "--logs", files["logs"],
             "--objects", files["objects"], "--area-size", 4096,
             "--psram-section", ".ext_ram.bss", lib)
    out = files["objects"].read_text()
    assert ('__attribute__((section(".ext_ram.bss"))) uint8_t demo_ring[4096] __aligned(4);'
            in out)
    assert "EXT_RAM_BSS_ATTR" not in out


# The phy module's version script (apps/multiapp/ext/phy.ver): what its importers need.
PHY_VER = Path(exports.__file__).parent / "ext/phy.ver"
ARB_H = PHY_VER.parents[3] / "include/esp_sdr/esp_sdr_arb.h"


def version_globals(path):
    """The names a version script exports."""
    text = re.sub(r"/\*.*?\*/", "", path.read_text(), flags=re.S)
    return set(re.findall(r"^\s*(\w+);", text.split("local:")[0], re.M))


def test_phy_exports_the_arbiter_api():
    # Every function of esp_sdr_arb.h but the weak trace hook is imported by the sdr module,
    # the wifi module or an app: the phy module must export it. The gate is private to the
    # library (esp_sdr_priv.h), the wraps are the wifi module's.
    names = version_globals(PHY_VER)
    api = set(re.findall(r"^[\w ]+?\**\s*(esp_sdr_arb_\w+)\(", ARB_H.read_text(), re.M))
    assert {"esp_sdr_arb_claim", "esp_sdr_arb_release", "esp_sdr_arb_wifi_attach"} <= api
    assert (api - {"esp_sdr_arb_trace"}) <= names
    assert {"esp_sdr_arb_gate", "__wrap_esp_phy_enable", "__wrap_esp_phy_disable"} <= names


def test_phy_exports_what_wifi_calls():
    # The wifi module has no libphy of its own: the PHY calls of its blobs and glue (nm of
    # libnet80211, libpp, esp_adapter.c, wifi_init.c, 2026-11) resolve from phy.
    need = {"esp_phy_disable", "esp_phy_modem_init", "esp_phy_modem_deinit",
            "esp_phy_update_country_info", "esp_wifi_power_domain_on",
            "esp_wifi_power_domain_off", "phy_wifi_enable_set", "ant_dft_cfg", "ant_wifirx_cfg",
            "ant_wifitx_cfg", "chip_v7_set_chan_misc", "noise_check_loop", "phy_change_channel",
            "phy_disable_low_rate", "phy_enable_low_rate", "phy_set_most_tpw",
            "register_chipv7_phy_init_param", "set_rx_sense"}
    assert need <= version_globals(PHY_VER)


# The wifi module's version script and the espnow module's.
WIFI_VER = PHY_VER.parent / "wifi.ver"
ESPNOW_VER = PHY_VER.parent / "espnow.ver"


def test_wifi_exports_what_espnow_imports():
    # libespnow.a is split: manatick.o (the mt_ layer, which net80211 reads) stays in wifi and
    # espnow.o goes to the espnow module, which imports these from wifi (nm of espnow.o,
    # 2026-11), plus the driver calls of ext/espnow.c. g_osi_funcs_p and memcpy are the host's.
    mt = {"mt_" + n for n in (
        "add_peer deinit del_peer fetch_peer get_peer get_peer_num get_user_oui get_version init "
        "is_peer_exist mod_peer register_recv_cb register_send_cb send set_peer_rate_config "
        "set_pmk set_user_oui set_wake_window unregister_recv_cb unregister_send_cb").split()}
    need = mt | {"esp_wifi_ipc_internal", "ieee80211_espnow_get_init_flag",
                 "ieee80211_espnow_set_init_flag", "ieee80211_ioctl", "wifi_action_tx_process",
                 "wifi_init_completed", "wifi_log", "wifi_roc_process",
                 "wifi_sta_restore_default_mac", "g_wifi_menuconfig", "g_wifi_nvs",
                 "esp_wifi_get_mode", "esp_wifi_set_mode", "esp_wifi_get_mac",
                 "esp_wifi_connectionless_module_set_wake_interval"}
    assert need <= version_globals(WIFI_VER)


def test_espnow_exports_its_api():
    text = ESPNOW_VER.read_text()
    assert "esp_now_*;" in text and "esp_sdr_espnow_*;" in text
    header = (PHY_VER.parents[3] / "include/esp_sdr/espnow.h").read_text()
    # What the header declares is defined in ext/espnow.c, which the glob exports.
    src = (PHY_VER.parent / "espnow.c").read_text()
    for name in re.findall(r"^[\w ]+?\**\s*(esp_sdr_espnow_\w+)\(", header, re.M):
        assert re.search(rf"^\w[\w ]*\**\s*{name}\(", src, re.M), name


def test_area_has_one_owner(tmp_path, run_main, lib):
    # coex's stack went to the area (ESP_SDR_PLACE_STACK is the high region) while wifi's heap was
    # the whole area: an extension that does not own the area may not have buffers there.
    with pytest.raises(SystemExit) as err:
        generate(tmp_path, run_main, lib, owners=("wifi",))
    assert "demo has buffers in the area" in str(err.value)
    generate(tmp_path, run_main, lib, owners=("wifi", "demo"))


def test_weak_symbol_relocations_are_refused(tmp_path, run_main):
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        pytest.skip("no host C compiler")
    src = tmp_path / "w.c"
    src.write_text("__attribute__((weak)) void hook(void) {}\nvoid call(void) { hook(); }\n")
    out = tmp_path / "libw_ext_llext_lib.so"
    subprocess.run([cc, "-shared", "-fPIC", "-o", out, src], check=True)
    assert exports.weak_relocations(out) == {"hook"}
    with pytest.raises(SystemExit, match="weak"):
        generate(tmp_path, run_main, out, owners=("w",))


def test_no_weak_functions_in_extension_sources():
    ext = Path(__file__).resolve().parents[2] / "apps/multiapp/ext"
    for path in ext.rglob("*.[ch]"):
        assert not re.search(r"__weak\b|attribute__\s*\(\(\s*weak", path.read_text()), path.name
