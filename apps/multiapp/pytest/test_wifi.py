# SPDX-License-Identifier: GPL-3.0-or-later
"""The stock Wi-Fi driver as a module (doc/multiapp.md): the boot runs wifitest, and the board finds
the access points around it. Needs at least one in range.

    west twister -T apps/multiapp -s sample.esp_sdr.multiapp.wifi.hil --device-testing \\
        --hardware-map tests/regression/hardware-map.yaml \\
        --flash-command "$PWD/scripts/twister-flash.py"
"""
import re

import pytest
from twister_harness import DeviceAdapter, Shell
from twister_harness.fixtures import determine_scope, get_ready_shell


@pytest.fixture(scope=determine_scope)
def shell(dut: DeviceAdapter) -> Shell:
    dut.write(b"\x03")
    return get_ready_shell(dut)


def test_module_from_its_file(shell: Shell):
    out = shell.exec_command("multiapp info")
    assert any(line.startswith("boot: wifitest, start 0") for line in out), out
    # As a standalone image with the driver: little in internal RAM, the rest in PSRAM.
    line = next((x for x in out if x.startswith("module wifi (file):")), None)
    assert line and re.search(r"text \d+ at 0x42\S+, fast \d+ at 0x403", line), out
    # Neither of the radio's other modules: Wi-Fi has its own PHY.
    assert not any(x.startswith(("module phy", "module sdr")) for x in out), out


def test_interface_is_the_hosts(shell: Shell):
    out = shell.exec_command("net iface")
    # The driver's device took over the host's interface: it says it is Wi-Fi.
    assert any("esp32-wifi" in line or "wifi" in line.lower() for line in out), out


def test_scan(shell: Shell):
    out = shell.exec_command("wifi scan", timeout=30)
    m = [re.match(r"(\d+) access points", line) for line in out]
    m = [x for x in m if x]
    assert m and int(m[0][1]) > 0, out


def test_no_crash(shell: Shell):
    out = shell.exec_command("multiapp crash")
    assert any("no fatal error" in line for line in out), out
