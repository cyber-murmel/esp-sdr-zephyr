# SPDX-License-Identifier: GPL-3.0-or-later
"""Desk check of the multiapp host on the ESP32-S31-Function-CoreBoard-1
(doc/apps/multiapp-internals.md): the image and its FAT volume are on the board
(scripts/twister-flash.py), which boots into osmosdr_hs.

Run with the board's USB HS port J4 on the PC through a cable with VBUS cut
(apps/osmosdr_hs/README.rst):

    west twister -T apps/multiapp -s sample.esp_sdr.multiapp.s31.hil --device-testing \\
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
    # hello, capture, rfprobe, osmosdr_hs and the radio's module.
    assert len(apps) == 5, out
    assert sum(", module" in line for line in apps) == 1, out
    for line in apps:
        # Every app has its file, the same as the built-in copy.
        assert re.search(r"file \d+ bytes", line) and "differs" not in line, line


def test_osmosdr_hs_from_file(shell: Shell):
    # The board's default app, with the vendor class for its stream.
    out = shell.exec_command("multiapp info")
    assert any(line.startswith("boot: osmosdr_hs, start 0, USB profile 0x6") for line in out), out
    assert any(line.startswith("osmosdr_hs (file):") for line in out), out


def test_radio_module(shell: Shell):
    # As a standalone image: IRAM_ATTR code in internal RAM, the rest of the code in PSRAM.
    out = shell.exec_command("multiapp info")
    assert any(re.match(r"module sdr \(file\): text \d+ at 0x5\S+, fast \d+ at 0x2f0", line)
               for line in out), out


def test_reset_was_clean(shell: Shell):
    out = shell.exec_command("multiapp crash")
    assert any("no fatal error" in line for line in out), out
