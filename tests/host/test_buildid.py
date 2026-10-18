# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/multiapp/buildid.py: the id host and extensions must share."""

from conftest import load

buildid = load("apps/multiapp/buildid.py")


def make(tmp_path, run_main, compiler="gcc 14.3.0", extra=()):
    out_h, out_bin = tmp_path / "id.h", tmp_path / "id.bin"
    run_main(buildid, "--out-h", out_h, "--out-bin", out_bin, "--compiler", compiler,
             "--file", tmp_path / "autoconf.h", "--dir", tmp_path / "inc", *extra)
    return out_bin.read_bytes(), out_h.read_text()


def tree(tmp_path):
    (tmp_path / "autoconf.h").write_text("#define CONFIG_X 1\n")
    inc = tmp_path / "inc" / "sub"
    inc.mkdir(parents=True)
    (inc / "a.h").write_text("int a;\n")
    (inc / "notes.txt").write_text("not hashed")


def test_header_and_bytes(tmp_path, run_main):
    tree(tmp_path)
    digest, header = make(tmp_path, run_main)
    assert len(digest) == 16
    assert "#define MULTIAPP_BUILD_ID {" + ", ".join(f"0x{b:02x}" for b in digest) + "}" in header


def test_what_changes_the_id(tmp_path, run_main):
    tree(tmp_path)
    base, _ = make(tmp_path, run_main)
    assert make(tmp_path, run_main)[0] == base
    # Only .h files count in the header trees.
    (tmp_path / "inc" / "sub" / "notes.txt").write_text("changed")
    assert make(tmp_path, run_main)[0] == base
    assert make(tmp_path, run_main, compiler="gcc 15.1.0")[0] != base
    (tmp_path / "inc" / "sub" / "a.h").write_text("int b;\n")
    changed, _ = make(tmp_path, run_main)
    assert changed != base
    (tmp_path / "autoconf.h").write_text("#define CONFIG_X 2\n")
    assert make(tmp_path, run_main)[0] not in (base, changed)
