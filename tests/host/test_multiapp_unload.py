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



# ---- the ESP32-S31 branch: the same unload, with the radio module's own down path ----

CMAKE = (MULTIAPP / "CMakeLists.txt").read_text()
ESP_SDR_C = (REPO / "lib/esp_sdr/src/esp_sdr.c").read_text()
OSMOSDR_C = (MULTIAPP / "src/osmosdr.c").read_text()
HS_MAIN = (REPO / "apps/osmosdr_hs/src/main.c").read_text()


def body(text, signature, contains=None):
    """The function whose definition starts with @p signature (a regex), to its closing brace; of
    several of that name (one per #if branch), the first that has @p contains."""
    for m in re.finditer(signature, text, re.M):
        found = text[m.start():text.index("\n}\n", m.start())]
        if contains is None or contains in found:
            return found
    raise AssertionError(signature)


def test_unload_is_on_by_default_on_both_socs():
    kconfig = (MULTIAPP / "Kconfig").read_text()
    section = kconfig[kconfig.index("config MULTIAPP_UNLOAD"):]
    default = re.search(r"^\s*default y if (.+)$", section, re.M).group(1)
    socs = set(re.findall(r"SOC_SERIES_\w+", default))
    assert {"SOC_SERIES_ESP32S3", "SOC_SERIES_ESP32S31"} <= socs


def test_s31_down_undoes_what_radio_up_did():
    # radio_up() of the PHY-only radio, then radio_down(): each power-up step has its inverse
    # (the modem clock is reference counted by the host: a reload would take a second one).
    up = body(ESP_SDR_C, r"^static int radio_up\(void\)", contains="esp_timer_init")
    down = body(ESP_SDR_C, r"^static void radio_down\(void\)")
    for step, inverse in (("esp_phy_enable(PHY_MODEM_WIFI)", "esp_phy_disable(PHY_MODEM_WIFI)"),
                          ("phy_wifi_enable_set(1)", "phy_wifi_enable_set(0)"),
                          ("modem_clock_module_enable(PERIPH_WIFI_MODULE)",
                           "modem_clock_module_disable(PERIPH_WIFI_MODULE)"),
                          ("esp_wifi_bt_power_domain_on()", "esp_wifi_bt_power_domain_off()")):
        assert step in up, step
        assert inverse in down, inverse
    # In reverse: the PHY's flag, then its power, then the clock, then the domain.
    order = [down.index(c) for c in ("phy_wifi_enable_set(0)", "esp_phy_disable",
                                     "modem_clock_module_disable", "esp_wifi_bt_power_domain_off")]
    assert order == sorted(order)


def test_s31_release_radio_stops_what_an_app_may_have_left():
    start = ESP_SDR_C.index("#if defined(CONFIG_SOC_SERIES_ESP32S31) && "
                            "defined(CONFIG_ESP_SDR_PHY_ONLY)\n/*\n * The S31's radio")
    s31 = ESP_SDR_C[start:ESP_SDR_C.index("#else\nint esp_sdr_release_radio", start)]
    for call in ("esp_sdr_diag16_stop", "esp_sdr_diag16_active", "esp_sdr_replay_stop",
                 "esp_sdr_tx_tone_stop", "radio_down"):
        assert call in s31, call
    # A lock a dead thread still holds must not hang the switch: timed, and -EBUSY.
    assert "k_mutex_lock(&esp_sdr_lock, K_SECONDS(1))" in s31
    assert "return -EBUSY" in s31
    # The library is "not ready" before the PHY goes, and the lock is let go at the end.
    assert (s31.index("esp_sdr_ready = false") < s31.index("radio_down();")
            < s31.index("k_mutex_unlock"))
    # The S3's version stays as it was: the arbiter, the DAC threads, the ring.
    s3 = ESP_SDR_C[ESP_SDR_C.index("#else\nint esp_sdr_release_radio", start):]
    for call in ("esp_sdr_ring_stop", "esp_sdr_tx_dac_release", "esp_sdr_arb_release"):
        assert call in s3, call


def test_s31_sdr_module_has_the_down_hook():
    # One radio module on the S31 (PHY and library together), up and down in ext/sdr_up.c.
    s31 = s31_block()
    assert re.search(r"multiapp_module\(sdr INTERNAL\b", s31)
    assert "ext/sdr_up.c" in s31
    sdr_up = (MULTIAPP / "ext/sdr_up.c").read_text()
    assert re.search(r"^int multiapp_module_down\(void\)", sdr_up, re.M)


def s31_block():
    """The S31's part of CMakeLists.txt: what the radio module and the apps are built from."""
    start = CMAKE.index("elseif(CONFIG_SOC_SERIES_ESP32S31)")
    return CMAKE[start:CMAKE.index("# The stock Wi-Fi driver", start)]


def extensions(block):
    """multiapp_extension(name ...) calls of a CMake block, name -> argument text."""
    out = {}
    for m in re.finditer(r"multiapp_extension\((\w+)([^)]*)\)", block):
        out[m.group(1)] = m.group(2)
    return out


def test_s31_apps_that_switch_in_place_share_a_usb_profile():
    # hello, capture and rfprobe have no USB class of their own (profile 0): they switch among
    # themselves in place. osmosdr_hs has the vendor class (USB 2): a change of profile reboots.
    apps = extensions(s31_block())
    for name in ("hello", "capture", "rfprobe"):
        assert not re.search(r"\bUSB\b", apps[name]), name
        assert not re.search(r"\b(CONSOLE|AREA)\b", apps[name]), name
    assert re.search(r"\bUSB 2\b", apps["osmosdr_hs"])
    assert "CONSOLE" not in apps["osmosdr_hs"]
    # The rule itself: same profile, no console app, no drive held by the PC.
    switch = body((MULTIAPP / "src/apps.c").read_text(), r"^static bool switch_ok")
    assert "app->usb != usb_profile" in switch
    assert "console" in switch and "MULTIAPP_USB_MSC" in switch


def test_s31_takes_the_in_place_branch_of_a_request():
    # Without CONFIG_MULTIAPP_UNLOAD an app started in place only when none was loaded: with it,
    # the S31 goes through multiapp_switch() like the S3 (the SoC is not named in the branch).
    request = body((MULTIAPP / "src/apps.c").read_text(), r"^int multiapp_request")
    unload_branch = request[request.index("#if defined(CONFIG_MULTIAPP_UNLOAD)"):
                            request.index("#else")]
    assert "multiapp_switch(app)" in unload_branch
    assert "SOC_SERIES" not in unload_branch


def test_every_start_may_mount_the_volume_again():
    # An in-place switch starts the next app in the same boot: the read-write mount stays.
    mount = body((MULTIAPP / "src/drive.c").read_text(), r"^int multiapp_drive_mount_rw")
    assert mount.index("if (mounted_rw)") < mount.index("fs_mount")


def test_osmosdr_forwarders_drop_the_handlers_of_an_unloaded_app():
    header = (MULTIAPP.parent / "osmosdr/src/usb_sdr.h").read_text()
    provided = header[header.index("Provided by the app"):]
    handlers = set(re.findall(r"^(?:void|int) (usb_sdr_\w+)\(", provided, re.M))
    assert handlers == {"usb_sdr_in_done", "usb_sdr_out_done", "usb_sdr_link",
                        "usb_sdr_control_out", "usb_sdr_control_in"}
    # No cache in a function-local static (nothing could reset it), one counted entry per handler.
    assert not re.search(r"^\s+static (void|int) \(\*", OSMOSDR_C, re.M)
    assert (OSMOSDR_C.count("atomic_inc(&calls)") == OSMOSDR_C.count("atomic_dec(&calls)")
            == len(handlers))
    for name in handlers:
        fn = body(OSMOSDR_C, rf"^\w[^\n]* {name}\(")
        # Counted in first, then the cache is read (atomically), then the call.
        assert re.search(rf'HANDLER\(\w+, "{name}"\)', fn), name
        assert fn.index("atomic_inc(&calls)") < fn.index("HANDLER(") < fn.index("atomic_dec(&calls)")
    cache = body(OSMOSDR_C, r"^#define HANDLER", contains="__atomic_load_n")
    assert "__atomic_load_n(&app." in cache and "__atomic_store_n(&app." in cache
    drop = body(OSMOSDR_C, r"^int multiapp_osmosdr_drop")
    # Forget first (a caller that counts itself in from then on finds nothing), wait for the ones
    # inside, forget again (one of them may have cached what it looked up). The behaviour is
    # test_multiapp_forwarders.py's.
    assert drop.index("forget()") < drop.index("atomic_get(&calls)") < drop.rindex("forget()")
    assert "-EBUSY" in drop


def test_an_unload_cuts_the_app_off_before_it_drops_the_handlers():
    apps = (MULTIAPP / "src/apps.c").read_text()
    fn = body(apps, r"^void \*multiapp_app_fn")
    assert "loaded_info" in fn
    cut = body(apps, r"^static int app_cutoff")
    # The app is made invisible, the USB message hook cleared and waited for, then the vendor
    # class's handlers dropped; a failure puts the app back as it was (it stays loaded).
    assert (cut.index("__atomic_store_n(&loaded_info, NULL") < cut.index("app_msg_cb")
            < cut.index("app_msg_users") < cut.index("multiapp_osmosdr_drop()")
            < cut.index("__atomic_store_n(&loaded_info, info"))
    assert cut.count("-EBUSY") >= 1
    # Both ways an app goes (an unload, a start that failed in its constructors) pass through it
    # before the memory is freed.
    unload = body(apps, r"^static int app_unload")
    assert unload.index("app_cutoff(def)") < unload.index("loaded_def = NULL") < unload.index(
        "ext_unload(")
    start = body(apps, r"^int multiapp_start")
    tail = start[start.index("\nunload:"):]
    assert tail.index("app_cutoff(app)") < tail.index("ext_unload(")
    # An app that could not be cut off stays: neither a switch nor "multiapp info" may trip on it.
    assert "loaded_def == NULL" in body(apps, r"^static bool switch_ok")
    info = body(apps, r"^static int cmd_info")
    assert info.index("loaded_info == NULL") < info.index("loaded_info->name")
    # The message hook's pointer is read and written with the atomics.
    assert "__atomic_load_n(&app_msg_cb" in body(apps, r"^void app_usb_msg")


def guarded(text):
    """For each line of @p text, the conditions of the #if blocks it is in, as (condition,
    positive) pairs: a line under #else or after #elif is the negative of the first."""
    stack = []
    out = []
    for line in text.splitlines():
        s = line.strip()
        if re.match(r"#\s*(if|ifdef|ifndef)\b", s):
            cond = re.sub(r"^#\s*if(def|ndef)?\s*", "", s)
            if s.startswith(("#ifndef", "# ifndef")):
                cond = f"!{cond}"
            stack.append([cond, True])
        elif re.match(r"#\s*elif\b", s):
            stack[-1][1] = False
        elif re.match(r"#\s*else\b", s):
            stack[-1][1] = False
        elif re.match(r"#\s*endif\b", s):
            stack.pop()
        out.append(tuple(tuple(c) for c in stack))
    return out


def test_osmosdr_hs_stop_hook_exists_only_in_the_multiapp_build():
    lines = HS_MAIN.splitlines()
    marks = guarded(HS_MAIN)
    hits = [i for i, line in enumerate(lines)
            if re.search(r"\bmultiapp_app_stop\b|\bhs_release\b|\bspin_started\b|"
                         r"^static atomic_t quit", line)]
    assert hits
    for i in hits:
        assert ("defined(MULTIAPP_NAME)", True) in marks[i], f"line {i + 1}: {lines[i]}"
    # Standalone, HS_QUIT() is the constant false: the code it guards compiles away.
    assert re.search(r"#else\n#define HS_QUIT\(\) false\n#endif", HS_MAIN)
    # The hook sets the flag, then moves the generation and ends the ring (stream_replan()).
    hook = body(HS_MAIN, r"^int multiapp_app_stop")
    assert hook.index("atomic_set(&quit, 1)") < hook.index("stream_replan()")
    # main() looks at the flag after it has read the generation: a stop before that read is seen
    # there, one after it moves the generation. Before the flag was read first, and a stop in
    # between ended up in a ring that nothing stopped.
    main = body(HS_MAIN, r"^int main\(void\)")
    assert (main.index("k_sem_reset(&kick)") < main.index("gen = atomic_get(&want_gen)")
            < main.index("HS_QUIT()") < main.index("k_spin_lock(&want_lock)"))
    assert main.count("HS_QUIT()") == 1
    # The ring's sink ends the run for the flag too (esp_sdr_diag16_run() clears a stop that
    # came just before it started).
    sink = body(HS_MAIN, r"^static void ring_sink")
    assert re.search(r"atomic_get\(&want_gen\) != run_gen \|\| HS_QUIT\(\)", sink)
    # The batches still queued are cancelled and waited for before main() returns, while this app
    # is the one the completions reach.
    tail = main[main.rindex("HS_QUIT()"):]
    assert "#if defined(MULTIAPP_NAME)" in tail
    assert (tail.index("usb_sdr_cancel_in()") < tail.index("atomic_get(&in_flight) != 0")
            < tail.index("return 0"))
    # The destructor leaves nothing queued: the work items, then the transfers the class holds.
    release = body(HS_MAIN, r"^static void __attribute__\(\(destructor\)\) hs_release")
    assert release.index("k_work_cancel_sync(&link_work") < release.index("usb_sdr_cancel()")
    assert release.index("k_work_cancel_sync(&suspend_work") < release.index("usb_sdr_cancel()")


def test_capture_ends_on_request_in_the_multiapp_build_only():
    # Its survey loop never ends by itself: without a hook the 5 s join fails and a switch reboots.
    src = (REPO / "apps/capture/src/main.c").read_text()
    lines = src.splitlines()
    marks = guarded(src)
    hits = [i for i, line in enumerate(lines)
            if re.search(r"\bmultiapp_app_stop\b|^static atomic_t quit|\bquit_sem\b", line)]
    assert hits
    for i in hits:
        assert ("defined(MULTIAPP_NAME)", True) in marks[i], f"line {i + 1}: {lines[i]}"
    assert re.search(r"#else\n#define CAPTURE_QUIT\(\) false\n#endif", src)
    hook = body(src, r"^int multiapp_app_stop")
    assert hook.index("atomic_set(&quit, 1)") < hook.index("k_sem_give(&quit_sem)")
    # The flag is looked at between two steps of the survey, and the pause between surveys waits on
    # the semaphore, so that the stop is not a ten second wait.
    main = body(src, r"^int main\(void\)")
    assert main.index("CAPTURE_QUIT()") < main.index("capture_step(")
    assert main.index("k_sem_take(&quit_sem, K_SECONDS(10))") < main.index("k_sleep(K_SECONDS(10))")
    assert "#if defined(MULTIAPP_NAME)" in main


# ---- the one lock for the device's writes to the volume ----

def test_drive_write_lock_api():
    drive = (MULTIAPP / "src/drive.c").read_text()
    header = (MULTIAPP / "include/multiapp/multiapp.h").read_text()
    # Declared where host and extensions both reach it, defined by the host, so that exports.py
    # exports it to an extension that imports it.
    assert re.search(r"^int multiapp_drive_write_begin\(k_timeout_t timeout\);", header, re.M)
    assert re.search(r"^void multiapp_drive_write_end\(void\);", header, re.M)
    begin = body(drive, r"^int multiapp_drive_write_begin")
    assert "k_mutex_lock(&write_lock, timeout)" in begin
    assert "-EAGAIN" in begin and "-EBUSY" in begin
    # Refused while the PC holds the volume, before the lock and again after waiting for it.
    assert begin.count("msc_exported") == 2
    assert "k_mutex_unlock(&write_lock)" in begin
    assert "k_mutex_unlock(&write_lock)" in body(drive, r"^void multiapp_drive_write_end")
    # The MSC boot path sets the hook.
    export = body(drive, r"^int multiapp_drive_export")
    assert "multiapp_drive_msc_set(true)" in export
    assert re.search(r"^void multiapp_drive_msc_set\(bool", (MULTIAPP / "src/apps.h").read_text()
                     + drive, re.M)


def test_the_calibration_store_writes_under_the_lock_in_the_multiapp_build_only():
    src = (REPO / "lib/esp_sdr/src/esp_sdr_calstore.c").read_text()
    lines = src.splitlines()
    marks = guarded(src)
    # The multiapp symbols appear only where CONFIG_ESP_SDR_AS_EXTENSION is the condition.
    for i, line in enumerate(lines):
        if re.search(r"multiapp_drive_write_(begin|end)|<multiapp/multiapp\.h>", line):
            assert ("defined(CONFIG_ESP_SDR_AS_EXTENSION)", True) in marks[i], f"line {i + 1}"
    assert re.search(r"#else\n#define STORE_BEGIN\(\) 0\n#define STORE_END\(\)\s+\(\(void\)0\)\n#endif",
                     src)
    # Both writers (save, clear) bracket the settings call; the readers do not take it.
    for name, call in (("esp_sdr_cal_save", "settings_save_one"),
                       ("esp_sdr_cal_clear", "settings_delete")):
        fn = body(src, rf"^int {name}\(")
        assert fn.index("STORE_BEGIN()") < fn.index(call) < fn.rindex("STORE_END()"), name
        # A refusal returns before anything is written or any other lock is taken.
        assert fn.index("STORE_BEGIN()") < fn.index("k_mutex_lock(&cal_lock")
    assert "STORE_BEGIN" not in body(src, r"^int esp_sdr_cal_load\(")
    # Every settings write of the library is one of those two.
    writers = set(re.findall(r"\b(settings_(?:save_one|delete|save))\(", src))
    assert writers == {"settings_save_one", "settings_delete"}
