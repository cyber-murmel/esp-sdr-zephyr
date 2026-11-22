# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/multiapp: every root shell command an extension registers has a PROXY() in src/apps.c.

The host registers a fixed root for each name (the extension's own registration cannot join the
shell's sections); a root without one is "command not found" on the board."""

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
MULTIAPP = REPO / "apps/multiapp"
REGISTER = re.compile(r"SHELL_CMD(?:_ARG)?_REGISTER\(\s*(\w+)")


def extension_sources():
    """The .c files multiapp_extension() lists: by ${apps_dir}/dir/..., ${var}/file.c for a
    variable set to such a directory, ext/, and link's sources.cmake."""
    cmake = (MULTIAPP / "CMakeLists.txt").read_text() + (MULTIAPP / "wifi.cmake").read_text()
    variables = {"apps_dir": "apps", "CMAKE_CURRENT_LIST_DIR": "apps/multiapp"}
    for name, path in re.findall(r"^\s*set\((\w+) \$\{(?:apps_dir)\}/([\w/]+)\)", cmake, re.M):
        variables[name] = "apps/" + path
    files = set()
    for var, rest in re.findall(r"\$\{(\w+)\}/([\w/.]+\.c)\b", cmake):
        if var in variables:
            files.add(f"{variables[var]}/{rest}")
    link = (REPO / "apps/link/sources.cmake").read_text()
    files |= {f"apps/link/src/{m}" for m in re.findall(r"\$\{LINK_SRC_DIR\}/([\w/]+\.c)", link)}
    return sorted(f for f in files if (REPO / f).is_file())


def registered_roots():
    roots = {}
    for f in extension_sources():
        for name in REGISTER.findall((REPO / f).read_text()):
            roots.setdefault(name, []).append(f)
    return roots


def test_sources_are_found():
    sources = extension_sources()
    assert "apps/multiapp/ext/coex.c" in sources
    assert "apps/multiapp/ext/espnow.c" in sources
    assert "apps/rfprobe/src/main.c" in sources
    assert "apps/link/src/ofdm_test.c" in sources


def test_every_root_has_a_proxy():
    apps = (MULTIAPP / "src/apps.c").read_text()
    proxies = set(re.findall(r"^PROXY\((\w+)\);", apps, re.M))
    roots = registered_roots()
    assert {"coex", "espnow", "wifi", "probe", "wpan", "sdr", "link", "ofdm", "hello"} <= set(roots)
    missing = {name: files for name, files in roots.items() if name not in proxies}
    assert not missing, f"shell roots without PROXY() in apps/multiapp/src/apps.c: {missing}"
