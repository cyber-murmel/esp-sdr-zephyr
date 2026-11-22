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
PRELUDES = [MULTIAPP / "include/multiapp/ext.h", MULTIAPP / "ext/wifi_prelude.h",
            MULTIAPP / "include/multiapp/stage.h"]


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
    # SHELL_CMD_ARG_REGISTER, SYS_INIT_NAMED, slabs, settings, K_THREAD_DEFINE, net_mgmt, the
    # stage of include/multiapp/stage.h
    assert found >= 7


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
    # hello, capture and rfprobe declare no USB class of their own (the host adds the drive to
    # all of them, profile 4): they switch among themselves in place. osmosdr_hs has the vendor
    # class (USB 2, with the drive 6): a change of profile reboots.
    apps = extensions(s31_block())
    for name in ("hello", "capture", "rfprobe"):
        assert not re.search(r"\bUSB\b", apps[name]), name
        assert not re.search(r"\b(CONSOLE|AREA)\b", apps[name]), name
    assert re.search(r"\bUSB 2\b", apps["osmosdr_hs"])
    assert "CONSOLE" not in apps["osmosdr_hs"]
    # The rule itself: same profile (the one the host makes of the app's mask), no console app,
    # and no app that brought disks of its own for the drive (the PC may hold the drive, which the
    # host's own proxy keeps up through a switch).
    switch = body((MULTIAPP / "src/apps.c").read_text(), r"^static bool switch_ok")
    assert "profile_of(app) != usb_profile" in switch
    assert "console" in switch and "MULTIAPP_USB_OWN_DISKS" in switch


def test_s31_takes_the_in_place_branch_of_a_request():
    # Without CONFIG_MULTIAPP_UNLOAD an app started in place only when none was loaded: with it,
    # the S31 goes through multiapp_switch() like the S3 (the SoC is not named in the branch).
    request = body((MULTIAPP / "src/apps.c").read_text(), r"^int multiapp_request")
    unload_branch = request[request.index("#if defined(CONFIG_MULTIAPP_UNLOAD)"):
                            request.index("#else")]
    assert "multiapp_switch(app)" in unload_branch
    assert "SOC_SERIES" not in unload_branch


def test_an_app_start_does_not_mount_the_volume():
    # The calibration has a partition of its own: nothing keeps the FAT volume mounted between
    # reads, so the PC's writes are never behind a stale mount.
    drive = (MULTIAPP / "src/drive.c").read_text()
    assert "mount_rw" not in drive and "mounted_rw" not in drive
    start = (MULTIAPP / "src/apps.c").read_text()
    assert "mount_rw" not in start
    read = body(drive, r"^static ssize_t read_path")
    assert read.index("claim(false") < read.index("fs_mount(") < read.index("fs_unmount(")


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


# ---- the device's claim on the volume against the PC's drive ----

def test_drive_claim_api():
    drive = (MULTIAPP / "src/drive.c").read_text()
    header = (MULTIAPP / "include/multiapp/multiapp.h").read_text()
    # Declared where host and extensions both reach it, defined by the host, so that exports.py
    # exports it to an extension that imports it.
    for sig in (r"int multiapp_drive_read_begin\(k_timeout_t timeout\)", r"void multiapp_drive_read_end\(void\)",
                r"int multiapp_drive_write_begin\(k_timeout_t timeout\)", r"void multiapp_drive_write_end\(void\)",
                r"void multiapp_drive_busy_hook\(bool \(\*busy\)\(void\)\)"):
        assert re.search(rf"^{sig};", header, re.M), sig
        assert re.search(rf"^{sig}\n\{{", drive, re.M), sig
    # A writer's claim is a write (the medium goes away and stays away), a reader's is not.
    assert "claim(true, timeout)" in body(drive, r"^int multiapp_drive_write_begin")
    assert "claim(false, timeout)" in body(drive, r"^int multiapp_drive_read_begin")
    assert "drive_give(&px)" in body(drive, r"^void multiapp_drive_write_end")
    assert "drive_give(&px)" in body(drive, r"^void multiapp_drive_read_end")
    # Every device access to the volume after boot is under a claim: the app's files and the kernel
    # files (both read_path()), refused on the USB thread, and a read never hides the medium.
    text = body(drive, r"^static ssize_t read_path")
    assert text.count("claim(false, K_MSEC(CLAIM_MS))") == 1 and "-EBUSY" in text
    assert re.search(r"in_usb_thread = in_usb_thread", drive)
    # The drive's class sets the hook once it is on the bus; boots without the drive never do.
    assert re.search(r"^void multiapp_drive_msc_set\(bool", (MULTIAPP / "src/apps.h").read_text(), re.M)
    assert "msc_exported" not in drive
    # The radio module tells the host whether a ring runs, and takes it back before it unloads.
    up = (MULTIAPP / "ext/sdr_up.c").read_text()
    assert up.index("multiapp_drive_busy_hook(NULL)") < up.index("ret = esp_sdr_release_radio()")
    assert "multiapp_drive_busy_hook(ring_runs)" in up


def test_the_calibration_store_names_no_multiapp_symbol():
    # The store has its own partition in the multiapp host: no claim of the volume, no lock but its
    # own, and the same code in every build (standalone apps keep their backends).
    src = (REPO / "lib/esp_sdr/src/esp_sdr_calstore.c").read_text()
    assert "multiapp_drive" not in src and "multiapp/multiapp.h" not in src
    assert "STORE_BEGIN" not in src and "STORE_END" not in src
    # Every settings write of the library is one of two, and each takes the store's own lock.
    writers = set(re.findall(r"\b(settings_(?:save_one|delete|save))\(", src))
    assert writers == {"settings_save_one", "settings_delete"}
    for name in ("esp_sdr_cal_save", "esp_sdr_cal_clear"):
        assert "k_mutex_lock(&cal_lock" in body(src, rf"^int {name}\(")


# ---- hot-swapping a stage (doc/apps/multiapp-internals.md): what the unload has to do for it ----

def test_an_app_unload_drops_its_kernels_before_it_cuts_the_app_off():
    # The kernels import from the app, so its use count blocks ext_unload() until they are gone.
    apps = (MULTIAPP / "src/apps.c").read_text()
    body = apps[apps.index("static int app_unload"):]
    body = body[:body.index("\n}\n")]
    # A thread of the app that may be inside a kernel is aborted before the kernel goes.
    assert (body.index("app_stop()") < body.index("multiapp_stage_has_kernels()")
            < body.index("threads_sweep(def->name, loaded)")
            < body.index("multiapp_stage_drop_kernels()")
            < body.index("app_cutoff(def)") < body.index("ext_unload("))


def test_hellos_stop_hook_fails_when_its_thread_does_not_end():
    # Otherwise a thread stuck in a kernel's op would run freed text after the unload.
    hello = (MULTIAPP / "ext/hello.c").read_text()
    body = hello[hello.index("int multiapp_app_stop(void)"):]
    body = body[:body.index("\n}\n")]
    assert "k_thread_join" in body and "return -EBUSY" in body


def test_the_stage_abi_is_one_hash_for_the_app_and_its_kernels():
    # multiapp_stage_abi() hashes the shared headers once; the app's extension and every kernel of
    # the stage are built with it, and the host refuses a kernel whose hash is not the app's.
    assert "function(multiapp_stage_abi" in CMAKE and "file(SHA256" in CMAKE
    assert "MULTIAPP_STAGE_ABI_${KERN_STAGE}=${stage_abi_${KERN_STAGE}}" in CMAKE
    assert "DEFINES MULTIAPP_STAGE_ABI_ring_sink=${stage_abi_ring_sink}" in CMAKE
    assert "DEFINES MULTIAPP_STAGE_ABI_hello_op=${stage_abi_hello_op}" in CMAKE
    assert "ring_stage.h" in CMAKE[CMAKE.index("multiapp_stage_abi(ring_sink"):][:200]
    stage = (MULTIAPP / "src/stage.c").read_text()
    assert "k->desc->abi != s->st->abi" in stage
    header = (MULTIAPP / "include/multiapp/stage.h").read_text()
    assert header.count("MULTIAPP_STAGE_ABI_##") == 2
    # No accessor that reads the stage's function without a sync.
    assert "_stage_fn" not in header


def test_kernel_files_are_read_under_the_claim_on_both_chips():
    drive = (MULTIAPP / "src/drive.c").read_text()
    read = drive[drive.index("ssize_t multiapp_drive_read_file"):]
    read = read[:read.index("\n}\n")]
    # No chip is left out (the S3's FATFS object is out of ring bank 0) and no refusal on the
    # PC's holding the drive: the claim is made in read_path(), which gives -EBUSY on a timeout.
    assert "ENOTSUP" not in read and "SOC_SERIES" not in read and "msc_exported" not in read
    assert "return read_path(path, buf, max)" in read
    assert "-EBUSY" in body(drive, r"^static ssize_t read_path")
    # A file the PC is still copying is tried once more after a pause, here and for an app's file.
    shell = (MULTIAPP / "src/stage_shell.c").read_text()
    load = body(shell, r"^static int cmd_load")
    assert "DRIVE_RETRY_MS" in load and "multiapp_drive_on_bus()" in load
    assert "DRIVE_RETRY_MS" in body((MULTIAPP / "src/apps.c").read_text(), r"^static int load_extension")
    # The upload is dropped under the command lock like the other commands.
    shell = (MULTIAPP / "src/stage_shell.c").read_text()
    abort = shell[shell.index("static int cmd_abort"):]
    assert "lock(sh)" in abort[:abort.index("\n}\n")]


def test_every_unload_forgets_the_stages_in_the_extension():
    apps = (MULTIAPP / "src/apps.c").read_text()
    body = apps[apps.index("static int ext_unload"):]
    body = body[:body.index("\n}\n")]
    assert body.index("roots_drop") < body.index("stages_drop") < body.index("llext_unload")


def test_a_kernel_loads_only_with_the_kernel_trailer_and_only_from_the_app():
    apps = (MULTIAPP / "src/apps.c").read_text()
    body = apps[apps.index("int multiapp_kernel_load"):]
    body = body[:body.index("\n}\n")]
    assert "MULTIAPP_KTRAILER_MAGIC" in body and "kernel_check_deps" in body
    # The signature (extsig.c) is checked inside check_trailer(), under the kernel's own name
    # (the file's, not the ksw<n> it loads as), and a refusal returns before llext_load().
    assert "check_trailer(name != NULL ? name : \"\"" in body
    assert body.index("check_trailer(") < body.index("llext_load(")
    assert "multiapp_sig_check(name, source, data, &len)" in function_body(apps, "static int check_trailer")
    # An app's file and a kernel's differ in the magic, so neither loads as the other.
    assert apps.count("MULTIAPP_TRAILER_MAGIC") == 2
    deps = apps[apps.index("static int kernel_check_deps"):]
    deps = deps[:deps.index("\n}\n")]
    assert "!= loaded" in deps and "LLEXT_MAX_DEPENDENCIES" in deps


def test_the_kernel_recipe_leaves_the_hosts_lists_alone():
    # A kernel is no embedded app, no input of exports.py and no part of the build id.
    macro = CMAKE[CMAKE.index("macro(multiapp_kernel_file"):]
    macro = macro[:macro.index("endmacro()")]
    own = macro.replace("set(ext_files ${ext_files} ${file} PARENT_SCOPE)", "")
    for name in ("ext_names", "ext_libs", "ext_files", "generate_inc_file_for_target"):
        assert name not in own
    assert "check-kernel" in macro and "--kernel ${kernel_place}" in macro
    # ... and the volume only when asked.
    assert "CONFIG_MULTIAPP_KERNELS_IN_VOLUME" in macro
    # multiapp_extension() ends for a kernel right after its file, before it joins any list.
    ext = CMAKE[CMAKE.index("  if(EXT_KERNEL)\n    multiapp_kernel_file"):]
    assert ext[:ext.index("endif()")].count("return()") == 1


def test_stages_need_the_unload_and_the_shell_has_the_subcommand():
    kconfig = (MULTIAPP / "Kconfig").read_text()
    stages = kconfig[kconfig.index("config MULTIAPP_STAGES"):]
    stages = stages[:stages.index("\nconfig ", 10)]
    assert "depends on MULTIAPP_UNLOAD" in stages
    apps = (MULTIAPP / "src/apps.c").read_text()
    assert "SHELL_CMD(stage, &multiapp_stage_cmds" in apps


# ---- the 802.15.4 driver's stage tx_gen: a stage of a module, not of the app ----

DRIVER_C = (REPO / "lib/ieee802154/ieee154_esp_sdr.c").read_text()


def function_body(text, signature):
    body = text[text.index(signature):]
    return body[:body.index("\n}\n")]


def test_the_drivers_stage_is_the_modules_and_its_holder_idles_before_it_lets_the_radio_go():
    # One owner at a time, handed on through radio_lock: whoever syncs holds the radio, and is idle
    # again before it gives it back, so a swap can land between two holders.
    assert "MULTIAPP_STAGE_DEFINE_INTERNAL(tx_gen, ieee154_dsp_tx_gen_fn_t, tx_gen);" in DRIVER_C
    ring = function_body(DRIVER_C, "static void ring_thread_fn")
    give = ring.index("tx_gen_give()")
    assert ring.index("ack_prefix()") < ring.index("esp_sdr_ring_run(") < give
    assert give < ring.index("k_mutex_unlock(&d->radio_lock)", give)
    tx = function_body(DRIVER_C, "static int sdr154_tx")
    assert (tx.index("radio_take(d)") < tx.index("tx_gen_take()") < tx.index("tx_gen_give()")
            < tx.index("radio_give(d)"))
    bench = function_body(DRIVER_C, "int ieee154_esp_sdr_bench(")
    assert bench.index("radio_take(d)") < bench.index("tx_gen_take()") < bench.index("tx_gen_give()")
    # ack_prefix() is the ring thread's sync and nobody else's.
    assert DRIVER_C.count("ack_prefix();") == 1
    assert "tx_gen_take()" in function_body(DRIVER_C, "static void ack_prefix")
    # The ring's burst is armed with the trampoline, never with a kernel's own address: a burst armed
    # by the PHY thread as a run ends must not call freed code.
    assert DRIVER_C.count("TX_GEN_BURST,") == 2
    assert "esp_sdr_ring_burst_arm(" in DRIVER_C and ", tx_gen," not in DRIVER_C
    burst = function_body(DRIVER_C, "static __maybe_unused void tx_gen_burst")
    assert "tx_gen_cur(" in burst
    # Give puts the built-in function back before it idles, so a stray call finds a function that
    # stays.
    giving = function_body(DRIVER_C, "static void tx_gen_give")
    assert giving.index("tx_gen_cur = tx_gen") < giving.index("tx_gen_stage_idle()")


def test_the_drivers_kernels_import_nothing_and_share_the_dsp_headers_with_the_module():
    assert "multiapp_stage_abi(tx_gen HEADERS" in CMAKE
    abi = CMAKE[CMAKE.index("multiapp_stage_abi(tx_gen HEADERS"):][:200]
    assert "ieee154_dsp.h" in abi and "ieee154_phy.h" in abi
    # The module, not an app, is built with the hash.
    assert "llext_compile_definitions(ieee154_ext MULTIAPP_STAGE_ABI_tx_gen=${stage_abi_tx_gen})" in CMAKE
    for kernel in ("tx_gen_ref", "tx_gen_alt"):
        assert f"multiapp_kernel({kernel} STAGE tx_gen APP ieee154 INTERNAL" in CMAKE
        source = (REPO / f"apps/wpan/kernels/{kernel}.c").read_text()
        assert "MULTIAPP_KERNEL_DEFINE(tx_gen, ieee154_dsp_tx_gen_fn_t, " in source
        # The state comes through the user argument: no symbol of the module (the loader would take
        # it, as the stage's owner, but the driver's own state is static and would not link).
        assert "high->" not in source and "high_ram" not in source
    # The standalone driver gets the header's macros as plain calls.
    assert "apps/multiapp/include" in (REPO / "lib/ieee802154/CMakeLists.txt").read_text()


def test_wpan_stops_the_radio_before_the_host_drops_the_drivers_kernels():
    # The kernels go before the app does (app_unload()), and the ring thread of the module may be
    # inside one: wpan's stop hook ends the radio, as the DFU download's does.
    main = (REPO / "apps/wpan/src/main.c").read_text()
    hook = function_body(main, "int multiapp_app_stop(void)")
    assert "app_usb_dfu_prepare()" in hook
    dfu = function_body(main, "void app_usb_dfu_prepare(void)")
    assert "api->stop(radio)" in dfu


# ---- the stage's owner: one extension, for the build check and the load check alike ----

STAGE_OWNERS = {
    # stage: (the extension multiapp_kernel(... APP <it>) names, the source that defines the stage)
    "tx_gen": ("ieee154", "lib/ieee802154/ieee154_esp_sdr.c"),
    "ring_sink": ("rfprobe", "apps/rfprobe/src/main.c"),
    "hello_op": ("hello", "apps/multiapp/ext/hello.c"),
}


def test_the_build_check_names_the_extension_that_defines_the_stage():
    # exports.py check-kernel reads the globals of the library of "APP"; the loader accepts the
    # extension that holds the stage's object (and the running app), so the two agree only if APP is
    # the defining extension: a module for the driver's stage, the app for the others.
    kernels = re.findall(r"^\s*multiapp_kernel\((\w+) STAGE (\w+) APP (\w+)", CMAKE, re.M)
    assert {stage for _, stage, _ in kernels} == set(STAGE_OWNERS)
    for kernel, stage, app in kernels:
        owner, source = STAGE_OWNERS[stage]
        assert app == owner, f"{kernel}: stage {stage} is defined in {owner}, not {app}"
        assert re.search(rf"MULTIAPP_STAGE_DEFINE\w*\({stage},", (REPO / source).read_text())


def test_the_load_check_takes_the_app_and_the_stages_owner_and_no_other_extension():
    apps = (MULTIAPP / "src/apps.c").read_text()
    deps = function_body(apps, "static int kernel_check_deps")
    assert "dep != loaded" in deps and "multiapp_stage_owned_by(stage, dep, ext_has)" in deps
    # The descriptor names the stage, so it is read (it is relocated at load) before the check, and
    # the check before the constructors run.
    load = function_body(apps, "int multiapp_kernel_load")
    assert (load.index('llext_find_sym(&k->ext->exp_tab, "multiapp_kernel")')
            < load.index("kernel_check_deps(k, desc->stage)") < load.index("llext_bringup("))
    # ... and the owner is where the stage's object is.
    stage = (MULTIAPP / "src/stage.c").read_text()
    owned = function_body(stage, "bool multiapp_stage_owned_by")
    assert "holds(ext, (uintptr_t)s->st)" in owned


def test_a_stage_in_internal_ram_takes_only_internal_kernels():
    # The driver's generator runs with interrupts off and the ring's sink at the ring's rate: a
    # PSRAM kernel stalls on the cache whenever the flash is written. hello_op may be anywhere.
    assert "MULTIAPP_STAGE_DEFINE_INTERNAL(ring_sink," in (REPO / "apps/rfprobe/src/main.c").read_text()
    assert "MULTIAPP_STAGE_DEFINE(hello_op," in (MULTIAPP / "ext/hello.c").read_text()
    for stage in ("tx_gen", "ring_sink"):
        abi = CMAKE[CMAKE.index(f"multiapp_stage_abi({stage} HEADERS"):]
        assert abi[:abi.index(")")].rstrip().endswith("INTERNAL")
    assert "multiapp_stage_abi(hello_op HEADERS ${CMAKE_CURRENT_LIST_DIR}/ext/hello_stage.h)" in CMAKE
    # multiapp_kernel() errors when the stage is internal and the kernel is not.
    kernel = function_body(CMAKE.replace("\nendfunction()", "\n}"), "function(multiapp_kernel name)")
    assert "stage_internal_${KERN_STAGE} AND NOT KERN_INTERNAL" in kernel and "FATAL_ERROR" in kernel
    # ... and the host refuses one whose function is not in the region the loader made internal.
    stage = (MULTIAPP / "src/stage.c").read_text()
    install = function_body(stage, "int multiapp_stage_install")
    assert "s->st->internal && !kernel_in_internal_ram(k)" in install
    inside = function_body(stage, "static bool kernel_in_internal_ram")
    assert "k->fast_size" in inside and "k->fast" in inside


def test_the_drop_of_a_stages_kernels_closes_the_stage_and_waits_for_its_owner():
    stage = (MULTIAPP / "src/stage.c").read_text()
    drop = function_body(stage, "int multiapp_stage_drop_kernels")
    assert (drop.index("s->closed = true") < drop.index("stage_wait_quiet(s)")
            < drop.index("multiapp_kernel_unload(KERNEL(s))") < drop.index("s->closed = false"))
    sync = function_body(stage, "void *multiapp_stage_sync")
    assert sync.index("s->closed") < sync.index("s->owner = me")
    assert "return -EBUSY" in function_body(stage, "static int stage_wait_quiet")
    # The app's unload fails (the caller reboots) when the drop does.
    apps = (MULTIAPP / "src/apps.c").read_text()
    unload = function_body(apps, "static int app_unload")
    after = unload[unload.index("multiapp_stage_drop_kernels()"):]
    assert after.index("if (ret != 0)") < after.index("return ret") < after.index("app_cutoff(def)")
