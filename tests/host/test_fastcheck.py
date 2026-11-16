# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/multiapp/fastcheck.py: fast code must not reach other sections PC-relative."""

import sys

import pytest

from conftest import load

fastcheck = load("apps/multiapp/fastcheck.py")

CLEAN = """
Relocation section '.rela.iram1.3' at offset 0x100 contains 2 entries:
 Offset     Info    Type                Sym. Value  Symbol's Name + Addend
00000003  00001214 R_XTENSA_SLOT0_OP      00000000   .iram1.3.literal + 0
00000009  0000130b R_XTENSA_ASM_EXPAND    00000000   other_function + 0

Relocation section '.rela.text.foo' at offset 0x200 contains 1 entry:
 Offset     Info    Type                Sym. Value  Symbol's Name + Addend
00000000  00001414 R_XTENSA_SLOT0_OP      00000000   .literal.foo + 0
"""

LEAVING = """
Relocation section '.rela.phyiram.reg' at offset 0x100 contains 1 entry:
 Offset     Info    Type                Sym. Value  Symbol's Name + Addend
00000003  00001214 R_XTENSA_SLOT0_OP      00000000   .literal.reg + 0
"""


def test_references_within_fast_code():
    assert fastcheck.leaving(CLEAN) == []


def test_reference_to_a_literal_outside():
    assert fastcheck.leaving(LEAVING) == [(".phyiram.reg", ".literal.reg")]


def test_main_fails_on_a_reference_leaving(monkeypatch, capsys, tmp_path):
    readelf = tmp_path / "readelf"
    readelf.write_text(f"#!/bin/sh\ncat <<'END'\n{LEAVING}\nEND\n")
    readelf.chmod(0o755)
    monkeypatch.setattr(sys, "argv", ["fastcheck.py", "--readelf", str(readelf), "blob.a"])
    assert fastcheck.main() == 1
    assert "blob.a: .phyiram.reg reaches .literal.reg" in capsys.readouterr().err


def test_main_passes_clean_files(monkeypatch, tmp_path):
    readelf = tmp_path / "readelf"
    readelf.write_text(f"#!/bin/sh\ncat <<'END'\n{CLEAN}\nEND\n")
    readelf.chmod(0o755)
    monkeypatch.setattr(sys, "argv", ["fastcheck.py", "--readelf", str(readelf), "blob.a"])
    assert fastcheck.main() == 0


def test_readelf_failure_is_an_error(monkeypatch, tmp_path):
    readelf = tmp_path / "readelf"
    readelf.write_text("#!/bin/sh\nexit 3\n")
    readelf.chmod(0o755)
    monkeypatch.setattr(sys, "argv", ["fastcheck.py", "--readelf", str(readelf), "blob.a"])
    with pytest.raises(Exception):
        fastcheck.main()
