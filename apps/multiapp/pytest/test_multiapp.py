# SPDX-License-Identifier: GPL-3.0-or-later
"""Desk check of the multiapp host (doc/apps/multiapp-internals.md): the image and its FAT volume
are on the XIAO ESP32-S3 (scripts/twister-flash.py), and the boot runs hello from its file.

Run with the boards attached:

    west twister -T apps/multiapp -s sample.esp_sdr.multiapp.hil --device-testing \\
        --hardware-map tests/regression/hardware-map.yaml \\
        --flash-command "$PWD/scripts/twister-flash.py"
"""
import re

import pytest
from twister_harness import DeviceAdapter, Shell
from twister_harness.fixtures import determine_scope, get_ready_shell


@pytest.fixture(scope=determine_scope)
def shell(dut: DeviceAdapter) -> Shell:
    # Ctrl-C first: west dfu's CR LF leaves the shell swallowing the harness's bare LF
    # (tests/regression/conftest.py).
    dut.write(b"\x03")
    return get_ready_shell(dut)


def test_files(shell: Shell):
    out = shell.exec_command("multiapp list")
    apps = [line for line in out if re.match(r"\w+\s+(\d+ bytes|-)", line)]
    # The nine apps, the radio's and Wi-Fi's modules and, with hil.conf, the test modules hello
    # needs.
    assert len(apps) == 15, out
    assert sum(", module" in line for line in apps) == 6, out
    # Too big for a built-in copy.
    assert any(re.match(r"wifi\s+-, file \d+ bytes, module", line) for line in apps), out
    assert any(re.match(r"micropython\s+-, file \d+ bytes", line) for line in apps), out
    for line in apps:
        # Every app has its file, the same as the built-in copy.
        assert re.search(r"file \d+ bytes", line) and "differs" not in line, line


def test_hello_from_file(shell: Shell):
    out = shell.exec_command("multiapp info")
    assert any(line.startswith("boot: hello, start 0") for line in out), out
    assert any(line.startswith("hello (file):") for line in out), out


def test_usb_on_cpu0(shell: Shell):
    # USB's interrupt lands on the CPU that allocates it, and a ring on CPU 1 masks interrupts: a
    # firmware update with the ring running ended in a watchdog reset.
    out = shell.exec_command("multiapp info")
    assert any("USB started on CPU 0" in line for line in out), out


def test_reset_was_clean(shell: Shell):
    # The volume's DFU rebooted a boot with the radio up. The ROM's analog I2C calls the PHY's
    # code with the cache off then: outside internal RAM, that ended in a double exception.
    out = shell.exec_command("multiapp crash")
    assert any("no fatal error" in line for line in out), out
    assert not any("double exception" in line for line in out), out


def test_hello_runs(shell: Shell):
    out = shell.exec_command("hello 5")
    assert any(line.startswith("hello: ops 10 15, direct 15") for line in out), out
    out = shell.exec_command("hello atomic", timeout=20)
    m = [re.search(r"psram \S+ (\d+), internal \S+ (\d+) \(expect (\d+)\)", line) for line in out]
    m = [x for x in m if x]
    assert m and m[0][1] == m[0][2] == m[0][3], out


def test_modules(shell: Shell):
    out = shell.exec_command("multiapp info")
    fast = [line for line in out if line.startswith("module hellofast (file):")]
    lib = [line for line in out if line.startswith("module hellolib (file):")]
    assert fast and lib, out
    # hellofast's IRAM_ATTR code at the instruction bus alias of internal RAM; hellolib in PSRAM.
    assert re.search(r"fast \d+ at 0x403", fast[0]), out
    assert re.search(r"text \d+ at 0x42", lib[0]), out
    assert re.search(r"fast 0 at", lib[0]), out
    out = shell.exec_command("hello mod")
    m = [re.search(r"fast code (\S+) data (\S+) value (\d+), lib code (\S+), chain (\d+)", line)
         for line in out]
    m = [x for x in m if x]
    assert m, out
    fast_code, fast_data, value, lib_code, chain = m[0].groups()
    assert fast_code.startswith("0x403") and fast_data.startswith("0x3fc"), out
    assert lib_code.startswith("0x42"), out
    # The constructor made 20 into 21; hellofast_twice(5) + 21 across two modules.
    assert value == "21" and chain == "31", out
    # The host ran the module's init table, and an app finds the device: wpan's radio works so.
    assert any("mod: device found, 1 inits" in line for line in out), out


def test_radio_modules(shell: Shell):
    out = shell.exec_command("multiapp info")
    # As a standalone image: IRAM_ATTR code in internal RAM, the rest of the code in PSRAM.
    for name in ("phy", "sdr"):
        assert any(re.match(rf"module {name} \(file\): text \d+ at 0x42\S+, fast \d+ at 0x403", line)
                   for line in out), out
    # Only wpan needs the 802.15.4 driver.
    assert not any(line.startswith("module ieee154") for line in out), out
    out = shell.exec_command("hello phy")
    assert any(re.search(r"phy: blob code 0x42\S+, version \S+", line) for line in out), out
