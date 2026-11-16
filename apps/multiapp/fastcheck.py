#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that a module's fast code does not reach other sections PC-relative.

A module's IRAM_ATTR code runs from internal RAM, the rest of its code from PSRAM
(ext/module_fast.ld). The linker resolves a PC-relative reference between the two as they lie in
the file and leaves no relocation for it, so such a reference would silently point elsewhere
once they are loaded apart. Reads an archive's or an object's relocations with readelf.
"""

import argparse
import re
import subprocess
import sys

FAST = re.compile(r"^\.(iram1|phyiram)(\.|$)")
SECTION = re.compile(r"Relocation section '\.rela([^']+)'")
PC_RELATIVE = "R_XTENSA_SLOT0_OP"


def leaving(relocations):
    """(fast section, target section) of each PC-relative reference from fast code to elsewhere."""
    found = []
    section = None
    for line in relocations.splitlines():
        m = SECTION.match(line)
        if m:
            section = m.group(1)
            continue
        if section is None or not FAST.match(section):
            continue
        fields = line.split()
        # Offset, info, type, symbol value, symbol name. A named symbol is checked by its section
        # only: the assembler writes references within a section to its section symbol.
        if len(fields) < 5 or fields[2] != PC_RELATIVE:
            continue
        target = fields[4]
        if target.startswith(".") and not FAST.match(target):
            found.append((section, target))
    return found


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--readelf", required=True, help="the toolchain's readelf")
    p.add_argument("files", nargs="+", help="archives or objects")
    a = p.parse_args()
    failed = False
    for path in a.files:
        out = subprocess.run([a.readelf, "-rW", path], capture_output=True, text=True, check=True)
        for section, target in leaving(out.stdout):
            print(f"fastcheck.py: {path}: {section} reaches {target} PC-relative", file=sys.stderr)
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
