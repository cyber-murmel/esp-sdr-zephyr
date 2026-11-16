# SPDX-License-Identifier: GPL-3.0-or-later
"""Placement goes through usage categories (include/esp_sdr/esp_sdr_place.h,
doc/code-placement.md): no source names a region's attribute or section itself, apart from the
files that define the regions and the exceptions listed here."""

import re

from conftest import REPO

DIRS = ("apps", "lib", "include", "tests", "drivers", "modules", "subsys")
SUFFIXES = (".c", ".h", ".S", ".cpp")

# The regions' own definitions, and what may name them directly.
EXEMPT = {
    # The macros and the attributes the regions expand to.
    "include/esp_sdr/esp_sdr_place.h",
    "include/esp_sdr/esp_sdr.h",
    # An extension's definitions of those attributes for its loader.
    "apps/multiapp/include/multiapp/ext.h",
    # They try the loader with IRAM_ATTR code itself.
    "apps/multiapp/ext/hello.c",
    "apps/multiapp/ext/hellofast.c",
}
# Unit tests build for the host (native_sim), where no region exists.
EXEMPT_DIRS = ("tests/unit/",)

RAW = re.compile(
    r"\b(IRAM_ATTR|DRAM_ATTR|EXT_RAM_BSS_ATTR|EXT_RAM_NOINIT_ATTR|ESP_SDR_HIGH_RAM"
    r"|ESP_SDR_HIGH_BUF_DEFINE|ESP_SDR_HIGH_STACK_DEFINE|ESP_SDR_PSRAM_BUF_DEFINE|__noinit)\b"
    r"|section\s*\(\s*\"\.(iram|dram|ext_ram|esp_sdr_high|noinit|text)"
    r"|^\s*\.section\s+\"?\.(iram|text)"
)
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def sources():
    for top in DIRS:
        base = REPO / top
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            rel = path.relative_to(REPO).as_posix()
            if path.suffix in SUFFIXES and path.is_file() and "/build" not in rel:
                yield rel, path


def raw_sites(text):
    """(line, text) of every raw placement in a source, comments left out."""
    code = COMMENT.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)
    for n, line in enumerate(code.splitlines(), 1):
        if RAW.search(line):
            yield n, line.strip()


def test_raw_sites_match_their_own_pattern():
    assert list(raw_sites("static IRAM_ATTR void f(void);")) != []
    assert list(raw_sites('int x __attribute__((section(".ext_ram.bss")));')) != []
    assert list(raw_sites('\t.section .iram1.f, "ax"')) != []
    assert list(raw_sites("/* IRAM_ATTR in a comment */ int y;")) == []
    assert list(raw_sites("static ESP_SDR_DATA(BULK, z) int z[4];")) == []


def test_no_raw_placement_outside_the_regions():
    found = []
    for rel, path in sources():
        if rel in EXEMPT or rel.startswith(EXEMPT_DIRS):
            continue
        for n, line in raw_sites(path.read_text(errors="replace")):
            found.append(f"{rel}:{n}: {line}")
    assert found == [], "raw placement, use ESP_SDR_CODE()/ESP_SDR_DATA():\n" + "\n".join(found)
