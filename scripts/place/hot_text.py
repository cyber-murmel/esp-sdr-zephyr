#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write the list of an app's hot flash block (CONFIG_ESP_SDR_HOT_TEXT) from CPU profiles.

    hot_text.py <build dir> <list file> <capture>... [--budget BYTES] [--threads PREFIX,...]
                [--also NAME,...] [--dry-run]

<build dir> is the app's build of the image the captures came from (the directory holding
zephyr/zephyr.elf, zephyr/zephyr.map, zephyr/.config and compile_commands.json); <list file> is the
file the app passes to esp_sdr_hot_text() (lib/esp_sdr/place.cmake). A capture is console output
of "prof dump" (CONFIG_ESP_SDR_CPU_PROF) or of Zephyr's "perf printbuf" (prof_map.py reads both; a
perf sample counts for its program counter only); --threads keeps the samples of the threads whose
names start with one of the prefixes (prof dumps).

The flash functions the captures sampled are ranked by samples per byte, and the densest taken
while their text and literals (their sizes in the linked map, after relaxation) fit the budget:
CONFIG_ESP_SDR_HOT_TEXT_BUDGET of the build (or --budget) less what the rules already place in the
block (ESP_SDR_HOTBLOCK, the .text.esp_sdr_hot.* input sections). Functions tagged COLD
(esp_sdr_place.h) never go in. The list holds one statement per object, archive and member, in the
order of each object's densest function; the file keeps its leading comment and gets the
statements after it. --also counts the named functions wherever the profiled image ran them (IRAM,
say), for a rule change that moves them to flash. --dry-run prints the statements instead.
"""

import argparse
import re
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import place_report  # noqa: E402
import prof_map  # noqa: E402

FLASH_TEXT = (0x42000000, 0x44000000)
CODE = (0x40370000, 0x44000000)  # IRAM and flash text, for --also
HOT_RULED = re.compile(r"^\.(text|literal)\.esp_sdr_hot\.")


def map_sections(text, lo=FLASH_TEXT[0], hi=FLASH_TEXT[1]):
    """Input sections of a linker map in [lo, hi): (name, address, size, object), by address. A
    long section name stands on a line of its own, its address, size and object on the next."""
    secs, cur = [], None
    for line in text.splitlines():
        m = re.match(r"^ (\.\S+)\s*$", line)
        if m:
            cur = m.group(1)
            continue
        m = re.match(r"^ (\.\S+)?\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)", line)
        if m:
            addr, size = int(m.group(2), 16), int(m.group(3), 16)
            name = m.group(1) or cur
            if lo <= addr < hi and size and name:
                secs.append((name, addr, size, m.group(4)))
        cur = None
    return sorted(secs, key=lambda s: s[1])


def base_name(name):
    """A function's name without GCC's clone suffixes (foo$constprop$0, foo.part.0)."""
    return re.split(r"[$.]", name, maxsplit=1)[0]


def literal_sizes(secs):
    """Bytes of literals per (object, function base name), from .literal.<name> input sections."""
    out = Counter()
    for name, _, size, obj in secs:
        if name.startswith(".literal.") and not HOT_RULED.match(name):
            out[(obj, base_name(name[len(".literal."):]))] += size
    return out


def pick(ranked, cost, budget, skip=()):
    """The densest names while their costs fit the budget, in rank order, and the bytes used."""
    picked, used = [], 0
    for name in ranked:
        if name in skip:
            continue
        c = cost(name)
        if used + c > budget:
            break
        used += c
        picked.append(name)
    return picked, used


def statements(picked, obj_of):
    """Linker script statements: one per object (archive:member), in the order of each object's
    first picked function, claiming the functions' text and literals, clones included."""
    order, by_obj = [], {}
    for name in picked:
        m = re.match(r"(?:.*/)?([^/(]+)\((.+)\)$", obj_of(name) or "")
        if m is None:
            continue
        key = (m.group(1), m.group(2))
        if key not in by_obj:
            order.append(key)
            by_obj[key] = []
        f = base_name(name)
        if f not in by_obj[key]:
            by_obj[key].append(f)
    body = []
    for lib, member in order:
        pats = []
        for f in by_obj[(lib, member)]:
            pats += [f".literal.{f} .literal.{f}$*", f".text.{f} .text.{f}$*"]
        body.append(f"    *{lib}:{member}(")
        body += [f"        {p}" for p in pats[:-1]] + [f"        {pats[-1]})"]
    return body


def rewrite(text, body):
    """The list file: its leading comment kept, the statements after it."""
    head = ""
    if text.lstrip().startswith("/*"):
        end = text.index("*/") + 2
        head = text[:end] + "\n\n"
    return head + "\n".join(body) + "\n"


def config_value(build, name):
    cfg = Path(build) / "zephyr" / ".config"
    if cfg.is_file():
        m = re.search(rf"^{name}=(\d+)$", cfg.read_text(), re.M)
        if m:
            return int(m.group(1))
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("build")
    ap.add_argument("list")
    ap.add_argument("captures", nargs="+")
    ap.add_argument("--budget", type=int)
    ap.add_argument("--threads", default="")
    ap.add_argument("--also", default="")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    build = Path(args.build)

    budget = args.budget or config_value(build, "CONFIG_ESP_SDR_HOT_TEXT_BUDGET")
    if budget is None:
        sys.exit("no budget: CONFIG_ESP_SDR_HOT_TEXT_BUDGET is not in the build's .config")
    img = prof_map.Image(build / "zephyr" / "zephyr.elf")
    also = {n for n in args.also.split(",") if n}
    secs = map_sections((build / "zephyr" / "zephyr.map").read_text(errors="replace"),
                        CODE[0] if also else FLASH_TEXT[0], CODE[1])
    starts = [s[1] for s in secs]
    ruled = sum(s[2] for s in secs if HOT_RULED.match(s[0]))
    lits = literal_sizes(secs)

    def sec_at(addr):
        import bisect
        i = bisect.bisect_right(starts, addr) - 1
        return secs[i] if i >= 0 and addr < secs[i][1] + secs[i][2] else None

    cc = build / "compile_commands.json"
    cold = {t["name"] for t in place_report.scan_sources(cc) if t["cat"] == "COLD"} \
        if cc.is_file() else set()

    keep = [p for p in args.threads.split(",") if p]
    samples, where = Counter(), {}
    for c in args.captures:
        for stack, thread, count in prof_map.parse(Path(c).read_text(errors="replace")):
            if keep and not thread.startswith(tuple(keep)):
                continue
            pc = stack[0]
            name, size = img.func(pc)
            if not FLASH_TEXT[0] <= pc < FLASH_TEXT[1] and base_name(name) not in also:
                continue
            sec = sec_at(pc)
            if size == 0 or sec is None or HOT_RULED.match(sec[0]):
                continue
            samples[name] += count
            where[name] = (sec[3], size)

    def cost(name):
        obj, size = where[name]
        return ((size + 3) & ~3) + lits.get((obj, base_name(name)), 0)

    total = sum(samples.values())
    ranked = sorted(samples, key=lambda n: (-samples[n] / max(where[n][1], 1), n))
    skip = {n for n in ranked if base_name(n) in cold}
    picked, used = pick(ranked, cost, budget - ruled, skip)
    body = statements(picked, lambda n: where[n][0])
    if args.dry_run:
        print("\n".join(body))
    else:
        path = Path(args.list)
        path.write_text(rewrite(path.read_text() if path.is_file() else "", body))
    covered = sum(samples[n] for n in picked)
    print(f"{len(picked)} of {len(samples)} functions, {used} B with literals of {budget} B "
          f"({ruled} B placed by the rules), {100.0 * covered / max(total, 1):.1f} % of {total} "
          f"flash samples; {len(skip)} COLD left out", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
