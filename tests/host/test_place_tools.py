# SPDX-License-Identifier: GPL-3.0-or-later
"""scripts/place/: the placement diff, the profile mapper, the cache counter parser and the
placement report, on ELF files the host compiler builds."""

import os
import shutil
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest

from conftest import REPO, load

place_diff = load("scripts/place/place_diff.py")
prof_map = load("scripts/place/prof_map.py")
icache = load("scripts/place/icache.py")
place_report = load("scripts/place/place_report.py")

CC = shutil.which("cc") or shutil.which("gcc")
needs_cc = pytest.mark.skipif(CC is None, reason="no host C compiler")

A_C = """
static int twice(int x) { return 2 * x; }
int a_work(int x) { return twice(x) + 1; }
"""
B_C = """
static int twice(int x) { return x << 1; }
int b_work(int x) { return twice(x) - 1; }
int main(void) { extern int a_work(int); return a_work(1) + b_work(2); }
"""


def build(tmp_path, a_src=A_C, b_src=B_C, name="img", cflags=()):
    d = tmp_path / name
    d.mkdir()
    (d / "a.c").write_text(a_src)
    (d / "b.c").write_text(b_src)
    out = d / "img.elf"
    subprocess.run([CC, "-O1", "-fno-inline", *cflags, "-o", str(out), str(d / "a.c"),
                    str(d / "b.c")], check=True, cwd=d)
    return out


@needs_cc
def test_diff_identical_and_statics_keyed_by_file(tmp_path):
    a = build(tmp_path, name="a")
    b = build(tmp_path, name="b")
    ra, rb = place_diff.record(a), place_diff.record(b)
    assert place_diff.compare(ra, rb) == []
    keys = {(f, n) for f, n, *_ in ra["symbols"] if n == "twice"}
    assert {f for f, _ in keys} == {"a.c", "b.c"}


@needs_cc
def test_diff_reports_a_grown_function(tmp_path):
    a = build(tmp_path, name="a")
    grown = A_C.replace("return twice(x) + 1;", "return twice(x) * twice(x + 3) + x / 7 + 1;")
    b = build(tmp_path, a_src=grown, name="b")
    diff = place_diff.compare(place_diff.record(a), place_diff.record(b))
    assert any("a_work" in line for line in diff)


@needs_cc
def test_diff_rename_and_ignore(tmp_path):
    a = build(tmp_path, name="a")
    b = build(tmp_path, a_src=A_C.replace("a_work", "a_job"),
              b_src=B_C.replace("a_work", "a_job"), name="b")
    ra, rb = place_diff.record(a), place_diff.record(b)
    assert place_diff.compare(ra, rb) != []
    assert place_diff.compare(ra, rb, rename={"a_work": "a_job"}) == []
    assert place_diff.compare(ra, rb, ignore=[r"^a_(work|job)$"]) == []


@needs_cc
def test_tree_and_xz_records(tmp_path, run_main, capsys):
    img = build(tmp_path)
    for side in ("x", "y"):
        place_diff.save(place_diff.record(img), tmp_path / side / "dom" / "record.json.xz")
    assert run_main(place_diff, "tree", tmp_path / "x", tmp_path / "y") == 0
    assert "1 images, 0 differ" in capsys.readouterr().out
    (tmp_path / "y" / "dom" / "record.json.xz").unlink()
    assert run_main(place_diff, "tree", tmp_path / "x", tmp_path / "y") == 1
    assert "only in a" in capsys.readouterr().out


def rec(*syms):
    """A record of FUNC symbols in .text: (file, name, address, size) each."""
    return {"sections": [[".text", 0x1000, 0x400, "SHT_PROGBITS"]],
            "symbols": [[f, n, 0, addr, size, "FUNC", ".text"] for f, n, addr, size in syms]}


def test_diff_allow_size_explains_a_shift_behind_it():
    a = rec(("", "head", 0x1000, 16), ("x.c", "id_check", 0x1010, 30), ("", "tail", 0x1030, 8),
            ("", "last", 0x1038, 8))
    grown = rec(("", "head", 0x1000, 16), ("x.c", "id_check", 0x1010, 34), ("", "tail", 0x1034, 8),
                ("", "last", 0x103c, 8))
    assert place_diff.compare(a, grown) != []
    out = place_diff.compare(a, grown, allow_size=[r"^x\.c:id_check$"])
    assert out and all(line.startswith(place_diff.NOTE) for line in out)
    assert "2 symbols shifted" in out[0] and "x.c:id_check 30 -> 34" in out[0]


def test_diff_allow_size_keeps_other_moves():
    a = rec(("", "head", 0x1000, 16), ("x.c", "id_check", 0x1010, 30), ("", "tail", 0x1030, 8),
            ("", "last", 0x1038, 8))
    # tail shifts with id_check, last by more: something else moved it.
    b = rec(("", "head", 0x1000, 16), ("x.c", "id_check", 0x1010, 34), ("", "tail", 0x1034, 8),
            ("", "last", 0x1040, 8))
    out = place_diff.compare(a, b, allow_size=[r"id_check"])
    real = [line for line in out if not line.startswith(place_diff.NOTE)]
    assert any("last" in line for line in real)
    # A function that grows without being allowed is reported, the allowed one is not.
    c = rec(("", "head", 0x1000, 20), ("x.c", "id_check", 0x1014, 30), ("", "tail", 0x1034, 8),
            ("", "last", 0x103c, 8))
    real = [line for line in place_diff.compare(a, c, allow_size=[r"id_check"])
            if not line.startswith(place_diff.NOTE)]
    assert any("head" in line for line in real)


def test_report_finds_tags_through_wrappers():
    src = textwrap.dedent("""\
        #define ESP_SDR_PLACE_FILE dac
        #define HOT(cat, fn) ESP_SDR_CODE(cat, fn) __attribute__((hot))
        #define APP_HOT(fn) ESP_SDR_CODE(PACED_CHUNK, fn)
        /* ESP_SDR_CODE(COLD, in_a_comment) */
        HOT(PACED_SAMPLE, fill)
        static void fill(void) {}
        APP_HOT(feed)
        static void feed(void) {}
        ESP_SDR_CODE(EVENT, done)
        static void done(void) {}
        static ESP_SDR_DATA_F2(STATE, sk, NOINIT, ATOMIC) struct { int a; } sk;
        static int ring[4] ESP_SDR_DATA(BULK, ring);
        ESP_SDR_BUF_DEFINE(SCRATCH, uint8_t, heap, [64], 16, NOINIT);
        ESP_SDR_STACK_DEFINE(rx_stack, 2048);
        #define TX_STACK_ATTR ESP_SDR_DATA_F(STACK, tx_stack, NOINIT)
        """)
    tags = place_report.tags_in(src, place_report.wrappers_in(place_report.strip_comments(src)))
    got = sorted((t["cat"], t["name"], tuple(t["flags"])) for t in tags)
    assert got == sorted([
        ("PACED_SAMPLE", "fill", ()), ("PACED_CHUNK", "feed", ()), ("EVENT", "done", ()),
        ("STATE", "sk", ("NOINIT", "ATOMIC")), ("BULK", "ring", ()),
        ("SCRATCH", "heap", ("NOINIT",)), ("STACK", "rx_stack", ()),
        ("STACK", "tx_stack", ("NOINIT",)),
    ])
    assert place_report.FILE_Q.search(src).group(1) == "dac"


def test_diff_allow_size_takes_the_section_end_along():
    a = rec(("", "head", 0x1000, 16), ("x.c", "id_check", 0x1010, 30), ("", "tail", 0x1030, 8),
            ("", "_etext", 0x1040, 0))
    b = rec(("", "head", 0x1000, 16), ("x.c", "id_check", 0x1010, 34), ("", "tail", 0x1034, 8),
            ("", "_etext", 0x1050, 0))
    b["sections"] = [[".text", 0x1000, 0x410, "SHT_PROGBITS"]]
    out = place_diff.compare(a, b, allow_size=[r"id_check"])
    assert out and all(line.startswith(place_diff.NOTE) for line in out), out
    # Without the allowed change the same end is a difference.
    c = rec(("", "head", 0x1000, 16), ("x.c", "id_check", 0x1010, 30), ("", "tail", 0x1030, 8),
            ("", "_etext", 0x1050, 0))
    c["sections"] = [[".text", 0x1000, 0x410, "SHT_PROGBITS"]]
    real = [line for line in place_diff.compare(a, c, allow_size=[r"id_check"])
            if not line.startswith(place_diff.NOTE)]
    assert any("section .text" in line for line in real) and any("_etext" in line for line in real)


def test_report_reads_only_the_linked_sections_of_a_map():
    text = ("Discarded input sections\n\n"
            " .text.gone     0x00000000       0x40 app/libapp.a(a.c.obj)\n"
            "\nLinker script and memory map\n\n"
            " .text.kept     0x42001000       0x80 app/libapp.a(a.c.obj)\n"
            " .noinit.\"WEST_TOPDIR/x/main.c\".0\n"
            "                0x3fc90000      0x600 app/libapp.a(main.c.obj)\n"
            " .bss           0x3fc90600        0x0 app/libapp.a(main.c.obj)\n")
    assert place_report.linked_sections(text) == [
        (".noinit.\"WEST_TOPDIR/x/main.c\".0", 0x3fc90000, 0x600, "app/libapp.a(main.c.obj)"),
        (".text.kept", 0x42001000, 0x80, "app/libapp.a(a.c.obj)"),
    ]


# The board's functions start aligned (Xtensa), which the dump's thread frames rely on (plus 1 in an
# interrupt); a host build need not align them.
ALIGNED = ("-falign-functions=4",)


def prof_dump(threads, samples):
    """A "prof dump" as apps/common prints it: threads (frame, name), samples (pc, frame)."""
    lines = [f"prof: {len(samples)} samples, 0 in interrupts",
             "prof: cpu 0, period 64 us, span 0 ms, missed 0, gaps over 1.5 periods 0, longest 0 us"]
    lines += [f"thread {j} {0x3fc80000 + 0x100 * j:08x} {frame:08x} {1:6d} {0:6d} {name}"
              for j, (frame, name) in enumerate(threads)]
    lines.append(f"Perf buf length {3 * len(samples)}")
    for pc, frame in samples:
        lines += [f"{2:016x}", f"{pc:016x}", f"{frame:016x}"]
    return "\r\n".join(lines + ["prof: end", ""])


def test_prof_map_parses_both_capture_kinds():
    dump = prof_dump([(0x42000100, "ubt_mode"), (0x42000200, "idle 00"), (0x42000102, "sysworkq"),
                      (0, "?")],
                     [(0x42001000, 0x42000100), (0x42001000, 0x42000100), (0x40378000, 0x42000201),
                      (0x42002000, 0x42000102), (0x42003000, 0)])
    got = prof_map.parse(dump)
    assert ((0x42001000,), "ubt_mode", 2) in got
    # In an interrupt (the frame plus 1), and the second thread of the same entry function (plus 2).
    assert ((0x40378000,), "idle 00", 1) in got
    assert ((0x42002000,), "sysworkq", 1) in got
    assert ((0x42003000,), "?", 1) in got
    perf = "Perf buf length 5\n" + "\n".join(f"{w:016x}" for w in (2, 0x1000, 0x2000, 1, 0x3000))
    assert prof_map.parse(perf) == [((0x1000, 0x2000), "?", 1), ((0x3000,), "?", 1)]
    # Both in one console log, a "prof stop" line before perf's: each block read as what it is.
    both = prof_map.parse(dump + "prof: 5 samples\n" + perf)
    assert ((0x42001000,), "ubt_mode", 2) in both and ((0x1000, 0x2000), "?", 1) in both


@needs_cc
def test_prof_map_names_functions(tmp_path, run_main, capsys):
    img = build(tmp_path, cflags=ALIGNED)
    image = prof_map.Image(img)
    addr = next(a for a, n, _ in image.funcs if n == "b_work")
    entry = next(a for a, n, _ in image.funcs if n == "main")
    cap = tmp_path / "cap.txt"
    cap.write_text(prof_dump([(entry, "main")], [(addr + 1, entry)] * 4))
    assert run_main(prof_map, img, cap) == 0
    out = capsys.readouterr().out
    assert "b_work" in out and "4 samples" in out
    assert run_main(prof_map, img, cap, "--collapsed") == 0
    assert capsys.readouterr().out.strip() == "main;b_work 4"


@needs_cc
def test_prof_dump_reads_as_perf_for_stackcollapse(tmp_path):
    """Zephyr's stackcollapse.py reads the dump's block as perf's: the samples under their
    threads' entry functions."""
    zephyr = Path(os.environ.get("ZEPHYR_BASE", REPO.parent / "modules/zephyr"))
    script = zephyr / "scripts/profiling/stackcollapse.py"
    if not script.is_file():
        pytest.skip("no Zephyr tree")
    pytest.importorskip("elftools")
    img = build(tmp_path, cflags=ALIGNED)
    image = prof_map.Image(img)
    addr = next(a for a, n, _ in image.funcs if n == "b_work")
    entry = next(a for a, n, _ in image.funcs if n == "main")
    dump = prof_dump([(entry, "main")], [(addr + 1, entry), (addr + 1, entry + 1)])
    block = tmp_path / "block.txt"
    block.write_text(dump[dump.index("Perf buf length"):dump.index("prof: end")])
    out = subprocess.run([sys.executable, str(script), str(block), str(img)], check=True,
                         capture_output=True, text=True).stdout
    assert out.splitlines() == ["main;b_work 1", "main;b_work 1"]


def test_icache_parse_pairs_replies_with_reads():
    log = textwrap.dedent("""\
        @@ 1.0 devmem 0x600C40C4 32 2
        @@ 2.0 devmem 0x600C40CC
        Read value 0x00100000
        @@ 2.1 devmem 0x600C40C8
        Read value 0x00000800
        @@ 7.0 devmem 0x600C40CC
        Read value 0x00200000
        @@ 7.1 devmem 0x600C40C8
        Read value 0x00001000
        """)
    rows = icache.parse(log)
    assert rows == [(0x100000, 0x800), (0x200000, 0x1000)]
    assert "2 windows" in icache.summary(rows, 1.0)


@needs_cc
def test_report_leaves_out_tags_the_build_does_not_compile(tmp_path):
    # link's FFT: tagged in the C branch, an inline wrapper of the same name in the one built.
    src = tmp_path / "x.c"
    src.write_text(textwrap.dedent("""\
        #define ESP_SDR_CODE(cat, fn)
        #define HOT(cat, fn) ESP_SDR_CODE(cat, fn) __attribute__((noinline))
        #if defined(USE_KERNEL)
        static inline int twice(int x) { return x << 1; }
        #else
        HOT(SAMPLE, twice)
        static int twice(int x) { return 2 * x; }
        #endif
        ESP_SDR_CODE(EVENT, kept)
        static inline int kept(int x) { return x + 1; }
        ESP_SDR_CODE(CHUNK, same_line) static inline int same_line(int x) { return x - 1; }
        int caller(int x) { return twice(x) + kept(x) + same_line(x); }
        """))
    text = src.read_text()
    tags = place_report.tags_in(text, place_report.wrappers_in(place_report.strip_comments(text)))
    assert {t["name"]: t["line"] for t in tags} == {"twice": 6, "kept": 9, "same_line": 11}

    def absent(flags):
        cmd = f"{CC} -O1 {flags} -c {src} -o {tmp_path / 'x.o'}"
        ts = [dict(t, file=str(src), _cmd=cmd, _dir=str(tmp_path), absent="no symbol")
              for t in tags]
        place_report.left_out(ts)
        return {t["name"]: t["absent"] for t in ts}

    assert absent("-DUSE_KERNEL") == {"twice": "left out", "kept": "no symbol",
                                      "same_line": "no symbol"}
    assert absent("") == {"twice": "no symbol", "kept": "no symbol", "same_line": "no symbol"}
    # A failing preprocessor leaves the tags as they were.
    assert absent("-include does/not/exist.h")["twice"] == "no symbol"


@needs_cc
def test_report_inline_sites_leave_out_callers_the_linker_removed(tmp_path):
    src = tmp_path / "y.c"
    src.write_text(textwrap.dedent("""\
        #define ESP_SDR_CODE(cat, fn)
        static volatile int sink;
        ESP_SDR_CODE(EVENT, step)
        static inline int step(int x) { sink = x; return sink * 3 + 1; }
        __attribute__((noinline)) int used(int x) { return step(x) + 2; }
        __attribute__((noinline)) int unused(int x) { return step(x) - 2; }
        int main(int argc, char **argv) { (void)argv; return used(argc); }
        """))
    elf = tmp_path / "y.elf"
    subprocess.run([CC, "-O2", "-g", "-ffunction-sections", "-Wl,--gc-sections", "-o", str(elf),
                    str(src)], check=True, cwd=tmp_path)
    text = src.read_text()
    tags = place_report.tags_in(text, {})
    for t in tags:
        t.update(file=str(src), absent="no symbol", _cmd=CC, _dir=str(tmp_path))
    place_report.inline_sites(str(elf), tags)
    assert [c for c, _ in tags[0]["inlined"]] == ["used"]


@needs_cc
def test_report_lists_sections(tmp_path):
    text = place_report.report(build(tmp_path))
    assert "flash code" in text or ".text" in text
