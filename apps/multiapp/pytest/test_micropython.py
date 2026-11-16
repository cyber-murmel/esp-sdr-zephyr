# SPDX-License-Identifier: GPL-3.0-or-later
"""MicroPython as an app (doc/multiapp.md): the boot runs it from its file, which has no built-in copy,
and it has the CDC-ACM port for its REPL. Everything here goes through that REPL. On a board with a
display the launcher comes up first, and a GUI app must draw with the ring gapless.

    west twister -T apps/multiapp -s sample.esp_sdr.multiapp.micropython.hil --device-testing \\
        --hardware-map tests/regression/hardware-map.yaml \\
        --flash-command "$PWD/scripts/twister-flash.py"
"""
import re
import time

import pytest
from twister_harness import DeviceAdapter


def lines_until(dut: DeviceAdapter, regex: str, timeout: float = 10.0) -> list[str]:
    return dut.readlines_until(regex=regex, timeout=timeout, print_output=True)


def prompt(dut: DeviceAdapter) -> None:
    """To the REPL: Ctrl-C ends the launcher or what it runs."""
    end = time.time() + 30
    while True:
        dut.write(b"\x03\r\n")
        try:
            lines_until(dut, r">>> ", 2.0)
            return
        except AssertionError:
            if time.time() > end:
                raise


def answer(dut: DeviceAdapter, expr: str, timeout: float = 10.0) -> str:
    """The value of a Python expression, on a line of its own. Short: the REPL drops what overflows
    the port's buffer."""
    dut.clear_buffer()
    dut.write(f"print('<' + 'A>', {expr})\r\n".encode())
    lines = lines_until(dut, r"^<A> ", timeout)
    return lines[-1].split("<A> ", 1)[1].strip()


def do(dut: DeviceAdapter, line: str, timeout: float = 5.0) -> None:
    dut.clear_buffer()
    dut.write(line.encode() + b"\r\n")
    lines_until(dut, r">>> ", timeout)


def shell_line(dut: DeviceAdapter, command: str, start: str) -> str:
    """The line of a host's shell command that starts so, through esdr.shell()."""
    expr = (f"[l for l in esdr.shell('{command}').split(chr(10)) if l.startswith('{start}')]")
    return answer(dut, expr)


@pytest.fixture(scope="module")
def repl(dut: DeviceAdapter) -> DeviceAdapter:
    prompt(dut)
    do(dut, "import esdr, gc, os, time")
    return dut


def test_boots_from_its_file(repl):
    assert "boot: micropython, start 0" in shell_line(repl, "multiapp info", "boot:")
    assert "micropython (file):" in shell_line(repl, "multiapp info", "micropython")


def test_has_no_built_in_copy(repl):
    assert re.search(r"micropython\s+-, file \d+ bytes", shell_line(repl, "multiapp list", "micropython"))


def test_loads_the_radio_modules_only(repl):
    assert "module phy (file)" in shell_line(repl, "multiapp info", "module phy")
    assert "module sdr (file)" in shell_line(repl, "multiapp info", "module sdr")
    assert shell_line(repl, "multiapp info", "module wifi") == "[]"


def test_usb_on_cpu0(repl):
    assert "USB started on CPU 0" in shell_line(repl, "multiapp info", "host main")


def test_heap_is_the_spare_psram(repl):
    do(repl, "gc.collect()")
    free = int(answer(repl, "gc.mem_free() + gc.mem_alloc()"))
    assert 4_000_000 < free < 8_000_000, free


def test_flash_volume(repl):
    do(repl, "f = open('/FLASH:/hil.txt', 'w'); f.write('multiapp'); f.close()")
    assert answer(repl, "open('/FLASH:/hil.txt').read()") == "multiapp"
    do(repl, "os.remove('/FLASH:/hil.txt')")
    # Its neighbours on the volume are the apps' files.
    assert "micropython.llext" in answer(repl, "os.listdir('/FLASH:')[-1:] + os.listdir('/FLASH:')[-2:]")


def test_ring_is_gapless(repl):
    do(repl, "esdr.init(); esdr.tune(2450000000); esdr.rx_start(250000)")
    do(repl, "time.sleep(3); esdr.rx_stop(); esdr.rx_wait(2000); s = esdr.rx_stats()", 10.0)
    assert int(answer(repl, "s['samples']")) > 500000
    assert answer(repl, "s['gaps'], s['errors'], s['runs']") == "0 0 1"


def test_soft_reset_keeps_the_heap(repl):
    do(repl, "gc.collect()")
    before = int(answer(repl, "gc.mem_free() + gc.mem_alloc()"))
    repl.write(b"\x04")
    time.sleep(3)
    prompt(repl)
    do(repl, "import esdr, gc, os, time")
    assert int(answer(repl, "gc.mem_free() + gc.mem_alloc()")) == before


def test_gui_app_draws_gapless(repl):
    size = answer(repl, "getattr(esdr, 'display_size', lambda: (0, 0))()")
    if size == "(0, 0)":
        pytest.skip("no display")
    repl.write(b"import launcher; launcher.run('spectrum')\r\n")
    time.sleep(12)
    prompt(repl)
    do(repl, "import esdr; from gnuradio import _gui; s = esdr.rx_stats()")
    assert int(answer(repl, "min(_gui._frames.values())")) > 20
    assert answer(repl, "s['gaps'], s['errors'], s['runs']") == "0 0 1"


def test_reset_was_clean(repl):
    do(repl, "import esdr")
    assert answer(repl, "esdr.crash()") == "None"
    assert "no fatal error" in answer(
        repl, "[l for l in esdr.shell('multiapp crash').split(chr(10)) if 'fatal' in l]")
