# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/multiapp/extconfig.py: what an extension's second Kconfig pass may change."""

import pytest

from conftest import load

extconfig = load("apps/multiapp/extconfig.py")

HOST = """CONFIG_KERNEL_THING=y
CONFIG_SHARED=7
# CONFIG_DRIVER is not set
CONFIG_DRIVER_LEFT=y
"""
EXT = """CONFIG_KERNEL_THING=y
CONFIG_SHARED=7
CONFIG_DRIVER=y
CONFIG_DRIVER_NAME="wifi"
CONFIG_DRIVER_BUFFERS=4
CONFIG_CRYPTO_LIB=y
"""


def run(tmp_path, run_main, host, ext, *args):
    h, e, out = tmp_path / "host", tmp_path / "ext", tmp_path / "out.h"
    h.write_text(host)
    e.write_text(ext)
    run_main(extconfig, "--host", h, "--ext", e, "--out", out, *args)
    return out.read_text()


def test_own_symbols_become_defines(tmp_path, run_main):
    out = run(tmp_path, run_main, HOST, EXT, "--own", r"DRIVER\w*", "--drop", r"CRYPTO\w*")
    assert "#define CONFIG_DRIVER 1\n" in out
    assert '#define CONFIG_DRIVER_NAME "wifi"\n' in out
    assert "#define CONFIG_DRIVER_BUFFERS 4\n" in out
    # Each one replaces what autoconf.h may say.
    assert "#undef CONFIG_DRIVER\n" in out
    # Set in the host, not in the pass: the extension must not see it.
    assert "#undef CONFIG_DRIVER_LEFT\n" in out
    assert "#define CONFIG_DRIVER_LEFT" not in out


def test_agreeing_and_dropped_symbols_stay_out(tmp_path, run_main):
    out = run(tmp_path, run_main, HOST, EXT, "--own", r"DRIVER\w*", "--drop", r"CRYPTO\w*")
    assert "KERNEL_THING" not in out
    assert "SHARED" not in out
    assert "CRYPTO_LIB" not in out


def test_a_foreign_symbol_fails_the_build(tmp_path, run_main):
    # A kernel option the host lacks could change a struct both sides use.
    with pytest.raises(SystemExit) as e:
        run(tmp_path, run_main, HOST, EXT + "CONFIG_THREAD_THING=y\n",
            "--own", r"DRIVER\w*", "--drop", r"CRYPTO\w*")
    assert "CONFIG_THREAD_THING: host not set, extension y" in str(e.value)


def test_a_changed_host_value_fails_the_build(tmp_path, run_main):
    with pytest.raises(SystemExit) as e:
        run(tmp_path, run_main, HOST, EXT.replace("SHARED=7", "SHARED=8"),
            "--own", r"DRIVER\w*", "--drop", r"CRYPTO\w*")
    assert "CONFIG_SHARED: host 7, extension 8" in str(e.value)
