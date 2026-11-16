#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Map CPU profile captures to the image's functions.

    prof_map.py <elf> <capture>... [--top N] [--threads PREFIX,...] [--collapsed]

<elf> is the zephyr.elf of the image the captures came from (or its build directory). A capture is
console output holding one or more sample blocks in the layout of Zephyr's "perf printbuf"
(subsys/profiling/perf): "Perf buf length <n>" and n words of 16 hex digits, each sample a frame
count and that many addresses, the program counter first. A block after a line "prof: <n> samples,
<m> in interrupts" is apps/common's "prof dump" (esp_sdr_prof.h, the receive ring's samples of the
other CPU): there each sample's outermost frame stands for its thread, which the dump's table
before the block names, "thread <index> <thread> <frame> <samples> <in interrupts> <name>" (the
frame plus 1: in an interrupt).

Prints the samples per output section (IRAM, flash, ...), per thread (prof dumps), and the top
functions with samples, share, size, samples per byte and section. --threads keeps the threads
whose names start with one of the prefixes; --collapsed prints "thread;function count" lines for
flamegraph.pl instead (perf captures: the whole stack, outermost first).
"""

import argparse
import bisect
import re
import sys
from collections import Counter
from pathlib import Path


class Image:
    """Functions and allocated sections of an ELF file."""

    def __init__(self, path):
        from elftools.elf.elffile import ELFFile
        from elftools.elf.constants import SH_FLAGS

        p = Path(path)
        if p.is_dir():
            cands = sorted(p.glob("**/zephyr/zephyr.elf"))
            if not cands:
                sys.exit(f"no zephyr/zephyr.elf below {p}")
            p = cands[0]
        with open(p, "rb") as f:
            elf = ELFFile(f)
            self.secs = sorted((s["sh_addr"], s["sh_addr"] + s["sh_size"], s.name)
                               for s in elf.iter_sections()
                               if s["sh_flags"] & SH_FLAGS.SHF_EXECINSTR and s["sh_size"])
            funcs = {}
            tab = elf.get_section_by_name(".symtab")
            for sym in tab.iter_symbols():
                if sym["st_info"]["type"] == "STT_FUNC" and sym["st_value"]:
                    a = sym["st_value"] & ~1
                    if a not in funcs or sym["st_size"] > funcs[a][1]:
                        funcs[a] = (sym.name, sym["st_size"])
        self.funcs = sorted((a, n, s) for a, (n, s) in funcs.items())
        self.starts = [f[0] for f in self.funcs]
        self.sec_starts = [s[0] for s in self.secs]

    def func(self, pc):
        i = bisect.bisect_right(self.starts, pc) - 1
        if i >= 0:
            a, name, size = self.funcs[i]
            if pc < a + max(size, 1) or (size == 0 and pc - a < 0x1000):
                return name, size
        return f"0x{pc:08x}", 0

    def section(self, pc):
        i = bisect.bisect_right(self.sec_starts, pc) - 1
        if i >= 0 and pc < self.secs[i][1]:
            return self.secs[i][2]
        return "rom/other"


HEAD = re.compile(r"^Perf buf length (\d+)\s*$", re.M)
WORD = re.compile(r"^([0-9a-f]{16})\s*$", re.M)
PROF = re.compile(r"^prof: \d+ samples, \d+ in interrupts", re.M)
THREAD = re.compile(r"^thread \d+ [0-9a-f]{8} ([0-9a-f]{8}) +\d+ +\d+ (.*?)\s*$", re.M)


def parse(text):
    """Samples of a capture: [(stack, thread name, count)], stack innermost first, equal samples
    counted together. A prof dump's samples are their program counters under their threads'
    names."""
    out = Counter()
    heads = list(HEAD.finditer(text))
    for i, m in enumerate(heads):
        before = text[heads[i - 1].end() if i else 0:m.start()]
        end = heads[i + 1].start() if i + 1 < len(heads) else len(text)
        words = [int(w, 16) for w in WORD.findall(text, m.end(), end)][:int(m.group(1))]
        prof = PROF.search(before) is not None
        names = {}
        for t in THREAD.finditer(before) if prof else ():
            frame = int(t.group(1), 16)
            names[frame] = "?" if frame in names else t.group(2)
        k = 0
        while k < len(words):
            n = words[k]
            if n == 0 or k + 1 + n > len(words):
                break
            stack = tuple(words[k + 1:k + 1 + n])
            if prof and n >= 2:
                out[(stack[:-1], names.get(stack[-1] & ~1, "?"))] += 1
            else:
                out[(stack, "?")] += 1
            k += 1 + n
    return [(stack, th, n) for (stack, th), n in out.items()]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("elf")
    ap.add_argument("captures", nargs="+")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--threads", default="")
    ap.add_argument("--collapsed", action="store_true")
    args = ap.parse_args()
    img = Image(args.elf)
    keep = [p for p in args.threads.split(",") if p]

    samples = []
    for c in args.captures:
        samples += parse(Path(c).read_text(errors="replace"))
    if keep:
        samples = [s for s in samples if any(s[1].startswith(p) for p in keep)]
    if args.collapsed:
        lines = Counter()
        for stack, th, n in samples:
            names = [img.func(pc)[0] for pc in reversed(stack)]
            lines[";".join([th] + names)] += n
        for line, n in sorted(lines.items()):
            print(line, n)
        return 0

    total = sum(n for _, _, n in samples)
    if total == 0:
        sys.exit("no samples")
    per_sec, per_th, per_fn, info = Counter(), Counter(), Counter(), {}
    for stack, th, n in samples:
        pc = stack[0]
        name, size = img.func(pc)
        per_sec[img.section(pc)] += n
        per_th[th] += n
        per_fn[name] += n
        info[name] = (size, img.section(pc))
    print(f"{total} samples")
    for sec, n in per_sec.most_common():
        print(f"  {sec:24s} {n:8d} {100.0 * n / total:6.2f} %")
    if len(per_th) > 1 or "?" not in per_th:
        print("threads:")
        for th, n in per_th.most_common():
            print(f"  {th:24s} {n:8d} {100.0 * n / total:6.2f} %")
    print(f"top {args.top} functions: samples, share, size, samples per byte, section")
    for name, n in per_fn.most_common(args.top):
        size, sec = info[name]
        dens = f"{n / size:8.2f}" if size else "       -"
        print(f"  {name:40s} {n:8d} {100.0 * n / total:6.2f} % {size:6d} {dens} {sec}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
