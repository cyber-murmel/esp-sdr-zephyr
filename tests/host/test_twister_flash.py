# SPDX-License-Identifier: GPL-3.0-or-later
"""scripts/twister-flash.py: which command flashes which build, with subprocess mocked."""

import os
import subprocess

import pytest

from conftest import load

flash = load("scripts/twister-flash.py")


@pytest.fixture
def calls(monkeypatch, tmp_path):
    seen = []

    def run(cmd, *a, **kw):
        seen.append(cmd)
        return subprocess.CompletedProcess(cmd, 0)

    monkeypatch.setattr(flash.subprocess, "run", run)
    monkeypatch.setattr(flash, "LINK_DIR", tmp_path / "links")
    return seen


def sysbuild(tmp_path, fat=False):
    b = tmp_path / "build"
    (b / "app" / "zephyr").mkdir(parents=True)
    (b / "domains.yaml").write_text("default: app\nbuild_dir: x\n")
    (b / "app" / "zephyr" / "zephyr.signed.bin").write_bytes(b"img")
    if fat:
        (b / "app" / "multiapp_fat.img").write_bytes(b"fat")
    return b


def exit_code(run_main, *args):
    with pytest.raises(SystemExit) as e:
        run_main(flash, *args)
    return e.value.code


def test_plain_build_on_a_dev_path(tmp_path, run_main, calls):
    b = tmp_path / "plain"
    b.mkdir()
    assert exit_code(run_main, "--build-dir", b, "--board-id", "/dev/ttyACM9") == 0
    assert calls == [["west", "flash", "-d", str(b), "--no-rebuild", "--esp-device",
                      "/dev/ttyACM9"]]
    assert not (tmp_path / "links").exists()


def test_mcuboot_image_goes_over_dfu(tmp_path, run_main, calls, monkeypatch):
    b = sysbuild(tmp_path, fat=True)
    monkeypatch.setattr(flash.glob, "glob", lambda pat: [str(tmp_path / "ttyACM0")])
    assert exit_code(run_main, "--build-dir", b, "--board-id", "A4CB8FD1D880") == 0
    assert calls == [["west", "dfu", "-d", str(b), "-s", "A4CB8FD1D880"],
                     ["west", "dfu", "--storage", "-d", str(b), "-s", "A4CB8FD1D880"]]
    link = tmp_path / "links" / "A4CB8FD1D880"
    assert os.readlink(link) == str(tmp_path / "ttyACM0")


def test_mcuboot_needs_a_board_id(tmp_path, run_main, calls):
    assert "board-id" in str(exit_code(run_main, "--build-dir", sysbuild(tmp_path)))
    assert calls == []


def test_signed_image_lookup(tmp_path):
    b = tmp_path / "b"
    b.mkdir()
    assert flash.signed_image(b) is None
    (b / "domains.yaml").write_text("default: app\n")
    assert flash.signed_image(b) is None      # named, but not built
    assert flash.fat_volume(b) is None


def test_port_lookup(monkeypatch):
    monkeypatch.setattr(flash.glob, "glob", lambda pat: [])
    with pytest.raises(SystemExit):
        flash.esp_port("NOPE")
    monkeypatch.setattr(flash.glob, "glob", lambda pat: ["/dev/serial/by-id/x_SN-if00"])
    assert flash.esp_port("SN") == "/dev/serial/by-id/x_SN-if00"
