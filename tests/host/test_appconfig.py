# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/multiapp/appconfig.py: an app's own Kconfig values for its extension build."""

import pytest

from conftest import load

appconfig = load("apps/multiapp/appconfig.py")

KCONFIG = """\
mainmenu "app"

config APP_RATE
	int "Rate"
	default 1000
	help
	  Something.

config APP_FAST
	bool "Fast path"
	default y
	select HOST_THING

config APP_EXTRA
	bool "Extra"
	depends on APP_FAST

config APP_NAME
	string "Name"
	default "probe"

source "Kconfig.zephyr"
"""


def setup(tmp_path, kconfig=KCONFIG, host="CONFIG_HOST_THING=y\n", confs=()):
    k = tmp_path / "Kconfig"
    k.write_text(kconfig)
    h = tmp_path / "host.config"
    h.write_text(host)
    paths = []
    for i, text in enumerate(confs):
        p = tmp_path / f"{i}.conf"
        p.write_text(text)
        paths.append(p)
    return k, h, paths


def generate(tmp_path, run_main, **kw):
    k, h, confs = setup(tmp_path, **kw)
    out = tmp_path / "appconfig.h"
    args = ["--kconfig", k, "--host-config", h, "--out", out]
    for c in confs:
        args += ["--conf", c]
    assert run_main(appconfig, *args) == 0
    return out.read_text().splitlines()


def test_parse_kconfig(tmp_path):
    k, _, _ = setup(tmp_path)
    opts = appconfig.parse_kconfig(k)
    assert list(opts) == ["APP_RATE", "APP_FAST", "APP_EXTRA", "APP_NAME"]
    assert opts["APP_RATE"] == {"type": "int", "default": "1000", "depends": None, "select": []}
    assert opts["APP_FAST"]["select"] == ["HOST_THING"]
    assert opts["APP_EXTRA"]["depends"] == "APP_FAST"


def test_parse_conf(tmp_path):
    p = tmp_path / "prj.conf"
    p.write_text("# comment\nCONFIG_A=y\n  CONFIG_B=\"x y\"\nnot a line\n")
    assert appconfig.parse_conf(p) == {"A": "y", "B": '"x y"'}


def test_defaults(tmp_path, run_main):
    lines = generate(tmp_path, run_main)
    assert "#define CONFIG_APP_RATE 1000" in lines
    assert "#define CONFIG_APP_FAST 1" in lines
    assert '#define CONFIG_APP_NAME "probe"' in lines
    # Every option is undefined first; one without a default stays undefined.
    assert "#undef CONFIG_APP_EXTRA" in lines
    assert not any(l.startswith("#define CONFIG_APP_EXTRA") for l in lines)


def test_conf_files_in_order(tmp_path, run_main):
    lines = generate(tmp_path, run_main, confs=("CONFIG_APP_RATE=10\nCONFIG_APP_EXTRA=y\n",
                                                "CONFIG_APP_RATE=20\nCONFIG_OTHER=y\n"))
    assert "#define CONFIG_APP_RATE 20" in lines
    assert "#define CONFIG_APP_EXTRA 1" in lines
    assert not any("CONFIG_OTHER" in l for l in lines)


def test_depends_off(tmp_path, run_main):
    lines = generate(tmp_path, run_main, confs=("CONFIG_APP_FAST=n\nCONFIG_APP_EXTRA=y\n",))
    assert not any(l.startswith("#define CONFIG_APP_FAST") for l in lines)
    assert not any(l.startswith("#define CONFIG_APP_EXTRA") for l in lines)


def test_select_needs_host(tmp_path, run_main):
    with pytest.raises(SystemExit):
        generate(tmp_path, run_main, host="CONFIG_HOST_THING=n\n")


@pytest.mark.parametrize("line", ["\tdefault 5 if FOO", "\tdepends on A && B", "\timply X"])
def test_unsupported(tmp_path, line):
    k, _, _ = setup(tmp_path, kconfig=f"config APP_X\n\tint \"x\"\n{line}\n")
    with pytest.raises(SystemExit):
        appconfig.parse_kconfig(k)
