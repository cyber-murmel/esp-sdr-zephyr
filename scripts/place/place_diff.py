#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare where two images put their code and data.

    place_diff.py record <elf> <out.json[.xz]>   write the placement record of an image or module
    place_diff.py diff <a> <b> [options]         compare two records or ELF files
    place_diff.py tree <dir a> <dir b> [options] compare every record below two directories

A record holds the allocated output sections (name, address, size, type) and every symbol in them
(name, address, size, type, section). Local symbols are keyed by the source file their STT_FILE
entry names, and by their order among equal names, since static names repeat across files. diff
and tree exit 1 when anything moved, grew, shrank, appeared or disappeared.

Options: --ignore REGEX (symbol names left out, repeatable), --rename OLD=NEW (a symbol renamed in
b, repeatable), --max N (differences listed per pair, default 40), --allow-size REGEX (functions,
as file:name or name, that may change size, repeatable: what follows one in its section may shift,
once, by the same amount, as long as order, sizes and sections stay, and the section may end that
much longer or shorter with its end markers; listed as notes, which do not count as differences). multiapp's build id, a hash of every header under include/, is such a case:
its bytes are immediates in apps.c's check_trailer and cmd_info.
"""

import argparse
import json
import lzma
import re
import sys
from pathlib import Path

SKIP_TYPES = ("STT_SECTION", "STT_FILE")


def record(elf_path):
    from elftools.elf.elffile import ELFFile
    from elftools.elf.constants import SH_FLAGS

    with open(elf_path, "rb") as f:
        elf = ELFFile(f)
        secs, alloc = [], {}
        for i, s in enumerate(elf.iter_sections()):
            if s["sh_flags"] & SH_FLAGS.SHF_ALLOC:
                secs.append([s.name, s["sh_addr"], s["sh_size"], s["sh_type"]])
                alloc[i] = s.name
        syms, seen, cur = [], {}, ""
        tab = elf.get_section_by_name(".symtab")
        for sym in (tab.iter_symbols() if tab is not None else []):
            typ = sym["st_info"]["type"]
            if typ == "STT_FILE":
                cur = sym.name
                continue
            if typ in SKIP_TYPES or sym["st_shndx"] not in alloc:
                continue
            local = sym["st_info"]["bind"] == "STB_LOCAL"
            key = (cur if local else "", sym.name)
            n = seen.get(key, 0)
            seen[key] = n + 1
            syms.append([key[0], sym.name, n, sym["st_value"], sym["st_size"], typ[4:],
                         alloc[sym["st_shndx"]]])
    return {"elf": str(elf_path), "sections": secs, "symbols": syms}


def load(path):
    p = Path(path)
    if p.suffix == ".xz":
        return json.loads(lzma.open(p, "rt").read())
    if p.suffix == ".json":
        return json.loads(p.read_text())
    return record(p)


def save(rec, path):
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(rec, separators=(",", ":"))
    if p.suffix == ".xz":
        with lzma.open(p, "wt") as f:
            f.write(text)
    else:
        p.write_text(text)


NOTE = "note: "


def compare(a, b, ignore=(), rename=None, limit=40, allow_size=()):
    """Differences between two records, as text lines (empty when nothing moved).

    Lines starting with NOTE describe what allow_size explains and are not differences.
    """
    rename = rename or {}
    skip = [re.compile(r) for r in ignore]
    allow = [re.compile(r) for r in allow_size]

    def keep(name):
        return not any(r.search(name) for r in skip)

    out = []
    sa = {s[0]: s[1:] for s in a["sections"]}
    sb = {s[0]: s[1:] for s in b["sections"]}
    grown = {n: sb[n][1] - sa[n][1] for n in set(sa) & set(sb)
             if sa[n][0] == sb[n][0] and sa[n][2] == sb[n][2]}

    def index(rec, ren):
        d = {}
        for f, name, n, addr, size, typ, sec in rec["symbols"]:
            name = ren.get(name, name)
            if keep(name):
                d[(f, name, n)] = (addr, size, typ, sec)
        return d

    ya, yb = index(a, rename), index(b, {})
    explained, notes, resized = explain(ya, yb, allow, grown) if allow else (set(), [], set())
    for name in sorted(set(sa) | set(sb)):
        if sa.get(name) == sb.get(name):
            continue
        if name in resized and grown.get(name) is not None:
            notes.append(f"section {name} {sa[name][1]} -> {sb[name][1]} B behind them")
            continue
        out.append(f"section {name}: {fmt_sec(sa.get(name))} -> {fmt_sec(sb.get(name))}")
    moved = []
    for key in sorted(set(ya) | set(yb), key=lambda k: (ya.get(k) or yb.get(k))[0]):
        if ya.get(key) != yb.get(key) and key not in explained:
            moved.append(f"{fmt_key(key)}: {fmt_sym(ya.get(key))} -> {fmt_sym(yb.get(key))}")
    out += [NOTE + n for n in notes]
    if moved:
        out.append(f"{len(moved)} symbols differ")
        out += moved[:limit]
        if len(moved) > limit:
            out.append(f"... {len(moved) - limit} more")
    return out


def explain(ya, yb, allow, grown=None):
    """The symbols whose change the allowed size changes account for, notes on them, and the
    sections in which an allowed function changed size.

    Per output section in a's address order: an allowed function may change size; its own address
    and that of every other symbol must have moved by the same amount as the symbol before it,
    except that the first symbol after an allowed one may set a new amount, and that a marker (no
    size) may move with the section's end (grown: output section -> its change in size). Symbols
    that changed type or section, or appeared, or disappeared, are never explained.
    """
    grown = grown or {}
    def allowed(key):
        return any(r.search(fmt_key(key)) for r in allow)

    by_sec = {}
    for key, (addr, size, typ, sec) in ya.items():
        vb = yb.get(key)
        if vb is not None and vb[2] == typ and vb[3] == sec:
            by_sec.setdefault(sec, []).append(key)
    explained, resized, shifted, in_secs = set(), [], 0, set()
    for sec, keys in by_sec.items():
        keys.sort(key=lambda k: (ya[k][0], ya[k][1], k))
        order_b = sorted(keys, key=lambda k: (yb[k][0], yb[k][1], k))
        if [yb[k][0] for k in keys] != [yb[k][0] for k in order_b]:
            continue
        delta, free, seen = 0, False, False
        for key in keys:
            (aa, sa, _, _), (ab, sb, _, _) = ya[key], yb[key]
            d = ab - aa
            if seen and sa == 0 and sb == 0 and d == grown.get(sec) and d != delta:
                explained.add(key)  # an end marker behind the section's new end
                shifted += 1
                continue
            if d != delta and not free:
                break
            delta, free = d, False
            if allowed(key):
                free = True
                if sa != sb:
                    resized.append(f"{fmt_key(key)} {sa} -> {sb}")
                    seen = True
                    in_secs.add(sec)
                explained.add(key)
            elif sa == sb:
                if d:
                    explained.add(key)
                    shifted += 1
            else:
                break
    notes = []
    if resized:
        notes.append(f"{shifted} symbols shifted behind allowed size changes: {', '.join(resized)}")
    return explained, notes, in_secs


def fmt_sec(s):
    return "-" if s is None else f"0x{s[0]:08x}+{s[1]} {s[2]}"


def fmt_sym(s):
    return "-" if s is None else f"0x{s[0]:08x}+{s[1]} {s[2]} {s[3]}"


def fmt_key(k):
    f, name, n = k
    return (f"{f}:" if f else "") + name + (f"#{n}" if n else "")


def records_below(root):
    root = Path(root)
    return {str(p.relative_to(root)): p for p in sorted(root.rglob("*.json*"))
            if p.name.endswith((".record.json", ".record.json.xz", "record.json",
                                "record.json.xz"))}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("cmd", choices=["record", "diff", "tree"])
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--ignore", action="append", default=[])
    ap.add_argument("--rename", action="append", default=[])
    ap.add_argument("--max", type=int, default=40)
    ap.add_argument("--allow-size", action="append", default=[])
    args = ap.parse_args()
    rename = dict(r.split("=", 1) for r in args.rename)

    if args.cmd == "record":
        save(record(args.a), args.b)
        return 0
    if args.cmd == "diff":
        diff = compare(load(args.a), load(args.b), args.ignore, rename, args.max, args.allow_size)
        real = [d for d in diff if not d.startswith(NOTE)]
        print("\n".join(diff) if diff else "identical")
        return 1 if real else 0

    ra, rb = records_below(args.a), records_below(args.b)
    bad = 0
    for rel in sorted(set(ra) | set(rb)):
        if rel not in ra or rel not in rb:
            print(f"{rel}: only in {'b' if rel not in ra else 'a'}")
            bad += 1
            continue
        diff = compare(load(ra[rel]), load(rb[rel]), args.ignore, rename, args.max,
                       args.allow_size)
        if diff:
            bad += any(not d.startswith(NOTE) for d in diff)
            print(f"{rel}:")
            print("\n".join("  " + d for d in diff))
    print(f"{len(set(ra) | set(rb))} images, {bad} differ")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
