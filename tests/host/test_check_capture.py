# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/micropython/tools/check_capture.py: the recording check (needs numpy)."""

import pytest

from conftest import load

np = pytest.importorskip("numpy")
check_capture = load("apps/micropython/tools/check_capture.py")


def test_tone_fc32(tmp_path, run_main, capsys):
    rate, f = 250e3, 31.25e3
    t = np.arange(20000)
    x = (0.5 * np.exp(2j * np.pi * f / rate * t)).astype(np.complex64)
    path = tmp_path / "rx.fc32"
    x.tofile(path)
    assert run_main(check_capture, path, "--rate", rate, "--center", 2.4e9) == 0
    out = capsys.readouterr().out
    assert "samples        20000" in out
    assert "(+31250 Hz from centre" in out


def test_sc16_and_failures(tmp_path, run_main, capsys):
    iq = np.zeros(8192, dtype=np.int16)
    iq[0::2] = 8192
    path = tmp_path / "rx.sc16"
    iq.tofile(path)
    assert run_main(check_capture, path, "--format", "sc16") == 0
    assert "dc share" in capsys.readouterr().out
    empty = tmp_path / "empty.fc32"
    empty.write_bytes(b"")
    assert run_main(check_capture, empty) == 1
    bad = tmp_path / "bad.fc32"
    np.array([np.nan, 1], dtype=np.complex64).tofile(bad)
    assert run_main(check_capture, bad) == 1
