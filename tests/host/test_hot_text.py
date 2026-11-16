# SPDX-License-Identifier: GPL-3.0-or-later
"""scripts/place/hot_text.py: the hot block list from profiles (map parsing, ranking against the
budget with literals, exclusions, statements per object, rewriting the list file)."""

import shutil
import subprocess
import textwrap

import pytest

from conftest import REPO, load

hot_text = load("scripts/place/hot_text.py")

# A linker map keeps one space before each input section: no dedent.
MAP = """\
 .text.le_run   0x42001000       0x80 app/libapp.a(ubt_le.c.obj)
 .literal.le_run
                0x42000ff0       0x10 app/libapp.a(ubt_le.c.obj)
 .text.le_run$constprop$0
                0x42001080       0x20 app/libapp.a(ubt_le.c.obj)
 .text.acquire  0x42001100       0x40 app/libapp.a(ubt_rx_sess.c.obj)
 .text.esp_sdr_hot.7
                0x42001200       0x30 app/libapp.a(ubt_rssi.c.obj)
 .iram1.3       0x40380000       0x40 app/libapp.a(ubt_le.c.obj)
"""


def test_map_sections_and_literals():
    secs = hot_text.map_sections(MAP)
    assert [s[0] for s in secs] == [".literal.le_run", ".text.le_run", ".text.le_run$constprop$0",
                                    ".text.acquire", ".text.esp_sdr_hot.7"]
    assert secs[0] == (".literal.le_run", 0x42000ff0, 0x10, "app/libapp.a(ubt_le.c.obj)")
    lits = hot_text.literal_sizes(secs)
    assert lits[("app/libapp.a(ubt_le.c.obj)", "le_run")] == 0x10
    assert hot_text.base_name("le_run$constprop$0") == "le_run"
    assert hot_text.base_name("chunk_size.part.0") == "chunk_size"


def test_pick_by_rank_within_the_budget():
    cost = {"a": 100, "b": 60, "c": 30, "d": 10}.__getitem__
    assert hot_text.pick(["a", "b", "c", "d"], cost, 200) == (["a", "b", "c", "d"], 200)
    assert hot_text.pick(["a", "b", "c", "d"], cost, 199) == (["a", "b", "c"], 190)
    # The densest prefix only: d would fit after c is refused, but the list stops there.
    assert hot_text.pick(["a", "b", "c", "d"], cost, 170) == (["a", "b"], 160)
    assert hot_text.pick(["a", "b", "c", "d"], cost, 170, skip={"b"}) == (["a", "c", "d"], 140)


def test_statements_group_per_object_and_cover_clones():
    obj = {"le_run$constprop$0": "app/libapp.a(ubt_le.c.obj)",
           "acquire": "app/libapp.a(ubt_rx_sess.c.obj)",
           "le_run": "app/libapp.a(ubt_le.c.obj)",
           "chunk_size": "zephyr/libzephyr.a(heap.c.obj)"}
    body = hot_text.statements(["le_run$constprop$0", "acquire", "le_run", "chunk_size"],
                               obj.get)
    assert body == [
        "    *libapp.a:ubt_le.c.obj(",
        "        .literal.le_run .literal.le_run$*",
        "        .text.le_run .text.le_run$*)",
        "    *libapp.a:ubt_rx_sess.c.obj(",
        "        .literal.acquire .literal.acquire$*",
        "        .text.acquire .text.acquire$*)",
        "    *libzephyr.a:heap.c.obj(",
        "        .literal.chunk_size .literal.chunk_size$*",
        "        .text.chunk_size .text.chunk_size$*)",
    ]


def test_rewrite_keeps_the_comment_and_is_idempotent():
    old = "/*\n * The list.\n */\n\n    *libapp.a:x.c.obj(\n        .text.x .text.x$*)\n"
    body = ["    *libapp.a:y.c.obj(", "        .text.y .text.y$*)"]
    new = hot_text.rewrite(old, body)
    assert new.startswith("/*\n * The list.\n */\n\n")
    assert new.endswith("    *libapp.a:y.c.obj(\n        .text.y .text.y$*)\n")
    assert hot_text.rewrite(new, body) == new
    assert hot_text.rewrite("", body) == "\n".join(body) + "\n"


CMAKE = shutil.which("cmake")
LD_CC = shutil.which("cc") or shutil.which("gcc")


def cmake_script(tmp_path, body):
    """Run a CMake script that has lib/esp_sdr/hot_text.cmake: (return code, output)."""
    script = tmp_path / "t.cmake"
    script.write_text(f'include("{REPO}/lib/esp_sdr/hot_text.cmake")\n' + textwrap.dedent(body))
    r = subprocess.run([CMAKE, f"-DZEPHYR_BINARY_DIR={tmp_path}", "-P", str(script)],
                       capture_output=True, text=True)
    return r.returncode, " ".join((r.stdout + r.stderr).split())  # CMake wraps its messages


@pytest.mark.skipif(CMAKE is None, reason="no cmake")
def test_hook_check_stops_without_the_text_sections_snippet(tmp_path):
    good, bad = tmp_path / "good.ld", tmp_path / "bad.ld"
    good.write_text("  #include <snippets-text-sections.ld>\n"
                    "  *(.literal .text .literal.* .text.*)\n")
    bad.write_text("  *(.literal .text .literal.* .text.*)\n")
    assert cmake_script(tmp_path, f'esp_sdr_hot_text_hook_check("{good}")\n')[0] == 0
    code, out = cmake_script(tmp_path, f'esp_sdr_hot_text_hook_check("{bad}")\n')
    assert code != 0 and "has no TEXT_SECTIONS hook" in out


@pytest.mark.skipif(CMAKE is None, reason="no cmake")
def test_wrapper_keeps_the_block_statements_in_order(tmp_path):
    lst = tmp_path / "hot.ld"
    lst.write_text("    *libapp.a:x.c.obj(.text.x)\n")
    code, out = cmake_script(tmp_path, f"""
        set(CONFIG_ESP_SDR_HOT_TEXT y)
        esp_sdr_hot_text("{lst}")
        """)
    assert code == 0, out
    text = (tmp_path / "esp_sdr_hot_text.ld").read_text()
    body = [line.strip() for line in text.split("*/", 1)[1].splitlines() if line.strip()]
    assert body[:5] == [
        ". = ALIGN(CONFIG_ESP32S3_INSTRUCTION_CACHE_SIZE / CONFIG_ESP32S3_ICACHE_ASSOCIATED_WAYS);",
        "__esp_sdr_hot_text_start = ABSOLUTE(.);",
        f'#include "{lst}"',
        "*(.literal.esp_sdr_hot.* .text.esp_sdr_hot.*)",
        "__esp_sdr_hot_text_end = ABSOLUTE(.);",
    ]
    assert body[5].startswith("ASSERT(__esp_sdr_hot_text_end - __esp_sdr_hot_text_start <= "
                              "CONFIG_ESP_SDR_HOT_TEXT_BUDGET")
    # Inside an output section a statement needs its semicolon, or the next one is a syntax error.
    assert body[6].endswith(");")
    # Without CONFIG_ESP_SDR_HOT_TEXT the app's call leaves the wrapper alone.
    (tmp_path / "esp_sdr_hot_text.ld").unlink()
    assert cmake_script(tmp_path, f'esp_sdr_hot_text("{lst}")\n')[0] == 0
    assert not (tmp_path / "esp_sdr_hot_text.ld").exists()


@pytest.mark.skipif(LD_CC is None, reason="no host C compiler")
def test_link_check_needs_the_block(tmp_path):
    (tmp_path / "a.c").write_text("int a;\nvoid _start(void) {}\n")
    check = (REPO / "lib/esp_sdr/hot_text_check.ld").read_text()
    results = []
    for define in ("__esp_sdr_hot_text_start = .;", ""):
        script = tmp_path / "t.ld"
        script.write_text("SECTIONS {\n .text : { *(.text*) " + define + " }\n" + check + "\n}\n")
        r = subprocess.run([LD_CC, "-nostdlib", "-static", "-Wl,-T," + str(script),
                            str(tmp_path / "a.c"), "-o", str(tmp_path / "a.out")],
                           capture_output=True, text=True)
        results.append((r.returncode, r.stderr))
    assert results[0][0] == 0, results[0][1]
    assert results[1][0] != 0 and "no hot block" in results[1][1]
