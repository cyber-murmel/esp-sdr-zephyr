# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/multiapp: what unloading needs, checkable without the board.

- every constructor an extension's prelude macro makes has a destructor in the same macro, or a
  marked reason why not (llext_teardown() runs the destructors);
- every module that defines multiapp_module_up() defines multiapp_module_down() and exports it, and
  the Wi-Fi module's down path calls the driver's stop and the hal's interrupt release;
- the loader's use-count check comes before llext_unload() (which counts down even when it fails).
"""

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
MULTIAPP = REPO / "apps/multiapp"
PRELUDES = [MULTIAPP / "include/multiapp/ext.h", MULTIAPP / "ext/wifi_prelude.h"]


def macros(path):
    """The #define blocks of a header (continuation lines joined) with a constructor in them."""
    text = path.read_text()
    out = []
    for m in re.finditer(r"^#define\s+(\w+)(?:[^\n]*\\\n)*[^\n]*", text, re.M):
        if "constructor" in m.group(0):
            out.append((m.group(1), m.group(0)))
    return out


def test_every_constructor_has_a_destructor_or_a_reason():
    found = 0
    for header in PRELUDES:
        for name, body in macros(header):
            found += 1
            assert "destructor" in body or "no destructor:" in body, f"{header.name}: {name}"
    # SHELL_CMD_ARG_REGISTER, SYS_INIT_NAMED, slabs, settings, K_THREAD_DEFINE, net_mgmt
    assert found >= 6


def test_destructors_undo_what_the_constructors_register():
    ext = (MULTIAPP / "include/multiapp/ext.h").read_text()
    assert "multiapp_shell_unregister" in ext
    assert "settings_deregister" in ext
    assert "k_thread_abort" in ext
    assert "net_mgmt_del_event_callback" in (MULTIAPP / "ext/wifi_prelude.h").read_text()


def module_sources():
    """The extension sources that define a module hook, by file."""
    return [p for p in (MULTIAPP / "ext").glob("*.c") if "multiapp_module_up" in p.read_text()
            or "multiapp_module_down" in p.read_text()]


def test_module_up_has_its_down():
    files = module_sources()
    assert files
    for path in files:
        text = path.read_text()
        up = re.search(r"^int multiapp_module_up\(void\)", text, re.M)
        down = re.search(r"^int multiapp_module_down\(void\)", text, re.M)
        assert (up is None) or down, f"{path.name} has no multiapp_module_down()"


def exported(ver):
    return set(re.findall(r"^\s*(\w+);", (MULTIAPP / "ext" / ver).read_text(), re.M))


def test_versioned_modules_export_their_down():
    for ver, src in (("wifi.ver", "wifi_glue.c"), ("espnow.ver", "espnow.c"),
                     ("phy.ver", "phy_down.c")):
        assert "multiapp_module_down" in exported(ver), ver
        assert re.search(r"^int multiapp_module_down\(void\)", (MULTIAPP / "ext" / src).read_text(),
                         re.M), src


def test_wifi_down_path():
    glue = (MULTIAPP / "ext/wifi_glue.c").read_text()
    down = glue[glue.index("int multiapp_module_down"):]
    for call in ("esp32_wifi_shutdown", "esp_wifi_adapter_release_isrs", "multiapp_wifi_unbind",
                 "esp_sdr_arb_wifi_detach"):
        assert call in down, call
    # The detach is exported by phy, the unbind is the host's.
    assert "esp_sdr_arb_wifi_detach" in exported("phy.ver")
    assert re.search(r"^int multiapp_wifi_unbind\(", (MULTIAPP / "src/wifi_netif.c").read_text(),
                     re.M)


def test_sdr_down_releases_the_radio():
    assert "esp_sdr_release_radio" in (MULTIAPP / "ext/sdr_up.c").read_text()
    lib = (REPO / "lib/esp_sdr/src/esp_sdr.c").read_text()
    body = lib[lib.index("int esp_sdr_release_radio"):]
    for call in ("esp_sdr_tx_dac_release", "esp_sdr_arb_release", "esp_sdr_arb_set_ops(NULL)"):
        assert call in body, call


def test_use_count_is_checked_before_llext_unload():
    apps = (MULTIAPP / "src/apps.c").read_text()
    body = apps[apps.index("static int ext_unload"):]
    body = body[:body.index("\n}\n")]
    assert body.index("use_count") < body.index("llext_unload")
    assert body.index("llext_teardown") < body.index("threads_sweep") < body.index("llext_unload")


def test_every_app_hook_has_a_prototype_in_the_abi():
    abi = (MULTIAPP / "include/multiapp/multiapp.h").read_text()
    for hook in ("multiapp_module_down", "multiapp_app_stop"):
        assert hook in abi


def test_host_calls_app_hooks_only_inside_the_extension():
    apps = (MULTIAPP / "src/apps.c").read_text()
    assert not re.search(r"llext_find_sym\(&loaded->exp_tab, \"(multiapp_app_stop|app_usb_dfu_prepare)\"",
                         apps.replace('llext_find_sym(&loaded->exp_tab, "multiapp_app_stop") != NULL', ""))
    body = apps[apps.index("static void *ext_hook"):]
    body = body[:body.index("\n}\n")]
    assert "ext_has" in body


def test_a_switch_asks_for_the_app_before_it_unloads():
    apps = (MULTIAPP / "src/apps.c").read_text()
    body = apps[apps.index("int multiapp_request"):]
    body = body[:body.index("\n}\n")]
    assert body.index("REQUEST_MAGIC") < body.index("multiapp_switch(app)")

