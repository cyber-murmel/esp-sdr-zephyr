#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Where an image's code and data went: bytes per memory region, and the room left.

    place_report.py <zephyr.elf or a build directory> [--out FILE] [--sources compile_commands.json]
                    [--manifest FILE] [--inlined] [--untagged BYTES] [--quiet]

Regions come from the Espressif linker scripts' output section names (IRAM, DRAM, the esp_sdr high
region, PSRAM, flash code and rodata, RTC); sections of other names are listed by themselves.
The room left: below the receive banks (_libc_heap_size, which the esp_sdr bank snippets set to
the gap up to the lowest bank), in the high region up to DRAM_USER_END (from the build's
linker.cmd), and the flash hot block against its budget when the image has one.

With the build's compile_commands.json (--sources, found next to the build directory's zephyr/ by
default) it adds the usage categories (include/esp_sdr/esp_sdr_place.h): every ESP_SDR_CODE(),
ESP_SDR_DATA*(), ESP_SDR_BUF_DEFINE(), ESP_SDR_STACK_DEFINE() and ESP_SDR_ASM_SECTION() of the
compiled sources, also through a file's wrapper macros (#define HOT(cat, fn) ESP_SDR_CODE(cat, fn)
...), with the symbols each became and the region they are in, as bytes per category and region,
and the tags with no symbol in the image: removed by the linker (unused), or with no symbol in the
object either (inlined, or left out by #if). An assembler tag stands for the symbols of the input
section it names. The sources of a multiapp extension (a <name>_ext_llext_lib target) are counted
from their objects, by input section, per extension; a buffer the host defines for one
(multiapp_<extension>_<name>, multiapp/ext.h) from the host image. A parameterless wrapper
(#define W ESP_SDR_CODE(cat, group)) stands for the functions whose names start with its group's
name. --manifest writes the tags as JSON.

--inlined adds, for each code tag of the image with no symbol, the functions DWARF says it was
inlined into and their regions, and the region the rules chose for it (ESP_SDR_REGION_CODE(), by
preprocessing a probe with its source's compile command); a caller in another region is marked
"!": the code runs where its caller does. Tags in code the build leaves out by #if (the source
preprocessed with its compile command) are listed by themselves, so that a function of the same
name in the branch the build compiles is not taken for theirs. --untagged BYTES lists the input
sections of the repository's own sources (those of compile_commands.json below the directory this
script's repository sits in) of at least BYTES in the image whose symbols carry no tag. --quiet
prints nothing and turns a failure into a warning, for the post-build step
(lib/esp_sdr/place.cmake).
"""

import argparse
import json
import re
import sys
from pathlib import Path

REGIONS = [
    ("IRAM", re.compile(r"^\.iram0\.")),
    ("DRAM data", re.compile(r"^\.dram0\.(data|rodata|.*_area)|^\.dram0\.end|^\.(k_|z_).*_area$")),
    ("DRAM noinit", re.compile(r"^\.dram0\.noinit")),
    ("DRAM bss", re.compile(r"^\.dram0\.bss")),
    ("high region", re.compile(r"^\.esp_sdr_high")),
    ("PSRAM", re.compile(r"^\.ext_ram")),
    ("flash code", re.compile(r"^\.flash\.text|^\.text$")),
    ("flash rodata", re.compile(r"^\.flash\.|^\.rodata")),
    ("RTC", re.compile(r"^\.rtc")),
]
HOT_MARKERS = [("__esp_sdr_hot_text_start", "__esp_sdr_hot_text_end"),
               ("__ubt_hot_text_start", "__ubt_hot_text_end")]


def is_dummy(name):
    """Address space a linker script holds for another bus's view of the same memory."""
    return name.endswith(("_dummy", ".dummy"))


def find_elf(path):
    p = Path(path)
    if p.is_file():
        return p
    for cand in (p / "zephyr/zephyr.elf", p / "zephyr.elf"):
        if cand.is_file():
            return cand
    cands = sorted(p.glob("*/zephyr/zephyr.elf"))
    if len(cands) == 1:
        return cands[0]
    sys.exit(f"no single zephyr.elf below {p}")


def load(elf_path):
    from elftools.elf.elffile import ELFFile
    from elftools.elf.constants import SH_FLAGS

    with open(elf_path, "rb") as f:
        elf = ELFFile(f)
        secs = [(s.name, s["sh_addr"], s["sh_size"]) for s in elf.iter_sections()
                if s["sh_flags"] & SH_FLAGS.SHF_ALLOC and s["sh_size"]]
        syms = {}
        tab = elf.get_section_by_name(".symtab")
        for sym in (tab.iter_symbols() if tab is not None else []):
            if sym.name and sym["st_info"]["bind"] != "STB_LOCAL":
                syms.setdefault(sym.name, sym["st_value"])
    return secs, syms


TAG = re.compile(
    r"\b(?P<macro>ESP_SDR_CODE|ESP_SDR_DATA_F2|ESP_SDR_DATA_F|ESP_SDR_DATA|ESP_SDR_BUF_DEFINE"
    r"|ESP_SDR_STACK_DEFINE|ESP_SDR_ASM_SECTION)\s*\((?P<args>[^()]*(?:\([^()]*\)[^()]*)*)\)")
WRAPPER = re.compile(r"^[ \t]*#[ \t]*define[ \t]+(?P<name>\w+)(?:\((?P<params>[^)]*)\))?[ \t]+"
                     r"ESP_SDR_CODE\((?P<cat>\w+),\s*(?P<fn>\w+)\)", re.M)
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
DEFINE_LINE = re.compile(r"^[ \t]*#[ \t]*define\b.*?(?<!\\)$", re.M | re.S)
FILE_Q = re.compile(r"^[ \t]*#[ \t]*define[ \t]+ESP_SDR_PLACE_FILE[ \t]+(\w+)", re.M)
CLONE = r"(?:[.$](?:constprop|isra|part|cold|lto_priv|localalias)(?:[.$]\d+)?)*"
EXT_TARGET = re.compile(r"CMakeFiles/(\w+)_ext_llext_lib\.dir/")
# An extension's input sections (multiapp's ext.h and module scripts) and where they load.
OBJ_REGIONS = [
    ("IRAM", re.compile(r"^\.iram1")),
    ("DRAM", re.compile(r"^\.dram1")),
    ("PSRAM", re.compile(r"^\.(bss\.)?ext_ram")),
    ("noinit", re.compile(r"^\.(bss\.)?noinit")),
    ("high region", re.compile(r"^\.(bss\.)?esp_sdr_high")),
    ("text", re.compile(r"^\.(text|literal)")),
    ("data", re.compile(r"^\.(data|bss|rodata|sdata|sbss)")),
]


def strip_comments(text):
    return COMMENT.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)


def wrappers_in(text):
    """name -> (cat or the index of the cat parameter, fn or the index of the fn parameter)."""
    out = {}
    for m in WRAPPER.finditer(text):
        params = [p.strip() for p in (m.group("params") or "").split(",") if p.strip()]
        if m.group("params") is None:
            out[m.group("name")] = (m.group("cat"), m.group("fn"), True)
            continue
        cat = params.index(m.group("cat")) if m.group("cat") in params else m.group("cat")
        fn = params.index(m.group("fn")) if m.group("fn") in params else m.group("fn")
        out[m.group("name")] = (cat, fn, False)
    return out


def tags_in(text, wrappers):
    """The tags of one source: dicts of macro, cat, name, flags and, for a group, group=True.

    Tags inside a #define count only for data in an object-like macro (#define TX_STACK_ATTR
    ESP_SDR_DATA_F(STACK, tx_stack, NOINIT)): a function-like one is a wrapper, and code in an
    object-like one a group (wrappers_in())."""
    code = strip_comments(text)
    body = DEFINE_LINE.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), code)

    def line_at(pos):
        return code.count("\n", 0, pos) + 1

    out = []
    for d in DEFINE_LINE.finditer(code):
        if re.match(r"^[ \t]*#[ \t]*define[ \t]+\w+\(", d.group(0)):
            continue
        for m in TAG.finditer(d.group(0)):
            args = [a.strip() for a in m.group("args").split(",")]
            if m.group("macro").startswith("ESP_SDR_DATA") and len(args) >= 2:
                out.append({"macro": m.group("macro"), "cat": args[0], "name": args[1],
                            "flags": args[2:], "line": line_at(d.start())})
    for m in TAG.finditer(body):
        args = [a.strip() for a in m.group("args").split(",")]
        macro = m.group("macro")
        if macro == "ESP_SDR_BUF_DEFINE":
            if len(args) >= 6:
                out.append({"macro": macro, "cat": args[0], "name": args[2], "flags": [args[-1]],
                            "line": line_at(m.start())})
        elif macro == "ESP_SDR_STACK_DEFINE":
            out.append({"macro": macro, "cat": "STACK", "name": args[0], "flags": [],
                        "line": line_at(m.start())})
        elif len(args) >= 2:
            out.append({"macro": macro, "cat": args[0], "name": args[1], "flags": args[2:],
                        "line": line_at(m.start())})
    for name, (cat, fn, group) in wrappers.items():
        if group:
            if re.search(r"\b%s\b" % name, body):
                out.append({"macro": name, "cat": cat, "name": fn, "flags": [], "group": True})
            continue
        for m in re.finditer(r"\b%s\s*\(([^()]*)\)" % name, body):
            args = [a.strip() for a in m.group(1).split(",")]
            try:
                c = args[cat] if isinstance(cat, int) else cat
                f = args[fn] if isinstance(fn, int) else fn
            except IndexError:
                continue
            out.append({"macro": name, "cat": c, "name": f, "flags": [],
                        "line": line_at(m.start())})
    return out


def scan_sources(cc_path):
    """The tags of every source in a compile_commands.json, with its scope and file qualifier."""
    entries = json.loads(Path(cc_path).read_text())
    files = []
    for e in entries:
        src = Path(e["file"])
        if not src.is_absolute():
            src = Path(e.get("directory", ".")) / src
        if src.suffix not in (".c", ".S", ".cpp") or not src.is_file():
            continue
        cmd = e.get("command") or " ".join(e.get("arguments", []))
        out = e.get("output")
        if out is None:
            m = re.search(r"(?:^|\s)-o\s*(\S+)", cmd)
            out = m.group(1) if m else None
        if out is not None and not Path(out).is_absolute():
            out = str(Path(e.get("directory", ".")) / out)
        files.append((src, cmd, out, e.get("directory", ".")))
    wrappers = {}
    for d in sorted({src.parent for src, _, _, _ in files}):
        for h in sorted(d.glob("*.h")):
            wrappers.update(wrappers_in(strip_comments(h.read_text(errors="replace"))))
    out, seen = [], set()
    for src, cmd, obj, cwd in files:
        m = EXT_TARGET.search(obj or "")
        ext = m.group(1) if m else None
        if (src, ext) in seen:
            continue
        seen.add((src, ext))
        text = src.read_text(errors="replace")
        if "ESP_SDR_" not in text and not any(w in text for w in wrappers):
            continue
        own = dict(wrappers)
        own.update(wrappers_in(strip_comments(text)))
        m = re.search(r"-DESP_SDR_PLACE_SCOPE=(\w+)", cmd)
        fq = FILE_Q.search(text)
        for t in tags_in(text, own):
            t.update(file=str(src), scope=m.group(1) if m else None,
                     file_q=fq.group(1) if fq else None, ext=ext, obj=obj, _cmd=cmd, _dir=cwd)
            out.append(t)
    return out


def region_of(sec):
    for region, pat in REGIONS:
        if pat.search(sec):
            return region
    return sec


def symbols_of(path, kinds=("STT_FUNC", "STT_OBJECT")):
    """An ELF file's function and object symbols, by name without clone suffixes:
    (name, file of a local, type, address, size, section name)."""
    from elftools.elf.elffile import ELFFile

    with open(path, "rb") as f:
        elf = ELFFile(f)
        names = {i: s.name for i, s in enumerate(elf.iter_sections())}
        tab = elf.get_section_by_name(".symtab")
        syms, cur = [], ""
        for sym in (tab.iter_symbols() if tab is not None else []):
            typ = sym["st_info"]["type"]
            if typ == "STT_FILE":
                cur = sym.name
                continue
            if typ not in kinds or not isinstance(sym["st_shndx"], int) or not sym.name:
                continue
            local = sym["st_info"]["bind"] == "STB_LOCAL"
            syms.append((sym.name, cur if local else "", typ, sym["st_value"], sym["st_size"],
                         names.get(sym["st_shndx"], "")))
    by_name = {}
    for s in syms:
        base = re.sub(CLONE + "$", "", s[0])
        base = re.sub(r"[.$]\d+$", "", base)
        by_name.setdefault(base, []).append(s)
    return by_name


def matches(t, by_name, any_file=False):
    want = "STT_OBJECT" if t["macro"].startswith(("ESP_SDR_DATA", "ESP_SDR_BUF", "ESP_SDR_STACK")) \
        else "STT_FUNC"
    if t["macro"] == "ESP_SDR_ASM_SECTION":
        want = None
    fname = Path(t["file"]).name
    if t.get("group"):
        cands = [s for k, v in by_name.items() if k.startswith(t["name"]) for s in v]
    else:
        cands = by_name.get(t["name"], []) + by_name.get(f"_k_mem_slab_buf_{t['name']}", [])
    cands = [s for s in cands if want in (None, s[2]) and (any_file or s[1] in ("", fname))]
    local = [s for s in cands if s[1] == fname]
    return local or [s for s in cands if s[1] == "" or any_file]


def undefined_in(path):
    """The names an object refers to without defining them."""
    from elftools.elf.elffile import ELFFile

    with open(path, "rb") as f:
        tab = ELFFile(f).get_section_by_name(".symtab")
        return {sym.name for sym in (tab.iter_symbols() if tab is not None else [])
                if sym["st_shndx"] == "SHN_UNDEF" and sym.name}


def section_sizes(path):
    from elftools.elf.elffile import ELFFile

    with open(path, "rb") as f:
        return {sec.name: sec["sh_size"] for sec in ELFFile(f).iter_sections()}


def obj_region(sec):
    for region, pat in OBJ_REGIONS:
        if pat.search(sec):
            return region
    return sec


def resolve(tags, elf_path, hot=None):
    """Each tag's symbols ([name, address, size, section, region]) and, without any, why
    ("unused": in its object but not in the image; "inlined": in neither)."""
    image = symbols_of(elf_path)
    objs = {}

    def obj_syms(path):
        if path not in objs:
            try:
                objs[path] = symbols_of(path)
            except (OSError, ValueError):
                objs[path] = None
        return objs[path]

    def asm_names(t):
        """The symbols an object defines in the input section an assembler tag names, and the
        section's size."""
        if not t.get("obj"):
            return [], 0
        try:
            syms = symbols_of(t["obj"], ("STT_FUNC", "STT_NOTYPE", "STT_OBJECT"))
            sizes = section_sizes(t["obj"])
        except (OSError, ValueError):
            return [], 0
        sec = re.compile(r"\.%s$" % re.escape(t["name"]))
        size = sum(v for k, v in sizes.items() if sec.search(k))
        return sorted({v[0] for vs in syms.values() for v in vs if sec.search(v[5])
                       and not v[0].startswith((".L", "$"))}), size

    for t in tags:
        out, t["absent"] = [], None
        if t["macro"] == "ESP_SDR_ASM_SECTION":
            names, sec_size = asm_names(t)
            if t.get("ext"):
                syms = obj_syms(t["obj"]) or {}
                for n in names:
                    for name, _, _, addr, size, sec in syms.get(n, []):
                        out.append([name, addr, size, sec, f"{t['ext']}: {obj_region(sec)}"])
            else:
                for n in names:
                    for name, _, _, addr, size, sec in image.get(n, []):
                        out.append([name, addr, size, sec, region_of(sec)])
            if out and not sum(o[2] for o in out):
                out[0][2] = sec_size  # labels without .size: the section's bytes
        elif t.get("ext"):
            syms = obj_syms(t["obj"]) if t.get("obj") else None
            for name, _, _, addr, size, sec in (matches(t, syms, True) if syms else []):
                out.append([name, addr, size, sec, f"{t['ext']}: {obj_region(sec)}"])
            host = f"multiapp_{t['ext']}_{t['name']}"
            for name, _, _, addr, size, sec in ([] if out else image.get(host, [])):
                out.append([name, addr, size, sec, f"{t['ext']}: host {region_of(sec)}"])
            if not out and t.get("obj"):
                try:
                    if host in undefined_in(t["obj"]):
                        t["absent"] = "host area"  # placed by the host when it loads
                except (OSError, ValueError):
                    pass
        else:
            for name, _, _, addr, size, sec in matches(t, image):
                region = region_of(sec)
                if hot and region == "flash code" and hot[0] <= addr < hot[1]:
                    region = "hot block"
                out.append([name, addr, size, sec, region])
        if not out and t["absent"] is None:
            syms = obj_syms(t["obj"]) if t.get("obj") else None
            t["absent"] = "unused" if syms and matches(t, syms, True) else "no symbol"
        t["symbols"] = out
    return tags


CODE_CATS = ("PACED_SAMPLE", "PACED_CHUNK", "PACED_EVENT", "SAMPLE", "CHUNK", "EVENT", "COLD",
             "NOCACHE")
# A region token for a code region, as region_of() names the sections.
CODE_REGION = {"ESP_SDR_IRAM": "IRAM", "ESP_SDR_FLASH": "flash code",
               "ESP_SDR_HOTBLOCK": "hot block"}


def absent_code(tags):
    return [t for t in tags if t.get("absent") == "no symbol" and not t.get("ext")
            and not t.get("group") and t["cat"] in CODE_CATS and t["macro"] != "ESP_SDR_ASM_SECTION"
            and t.get("_cmd")]


def preprocess_args(cmd, src):
    """A source's compile command without its source, output and dependency files: add -E and a
    file to preprocess that one as the source would be."""
    import shlex

    out, skip = [], False
    for a in shlex.split(cmd):
        if skip:
            skip = False
            continue
        if a in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
            continue
        if a in ("-c", "-MD", "-MMD") or a == src or a.startswith(("-o", "-MF")):
            continue
        out.append(a)
    return out


LINEMARKER = re.compile(r'^# (\d+) "((?:[^"\\]|\\.)*)"')


def live_lines(src, cmd, cwd):
    """The lines of src that its compile command keeps (not left out by #if), or None when the
    preprocessor fails."""
    import subprocess

    r = subprocess.run(preprocess_args(cmd, src) + ["-E", src], cwd=cwd, capture_output=True,
                       text=True)
    if r.returncode != 0:
        return None
    want = Path(cwd, src).resolve()
    live, line, here, names = set(), 0, False, {}
    for text in r.stdout.splitlines():
        m = LINEMARKER.match(text)
        if m:
            line = int(m.group(1))
            name = m.group(2)
            if name not in names:
                names[name] = not name.startswith("<") and Path(cwd, name).resolve() == want
            here = names[name]
            continue
        if here and text.strip():
            live.add(line)
        line += 1
    return live


def left_out(tags):
    """Absent code tags in code the build does not compile (an #if branch it leaves out): absent
    "left out", so that a function of the same name in the other branch is not taken for them."""
    by_src = {}
    for t in absent_code(tags):
        if t.get("line"):
            by_src.setdefault((t["file"], t["_cmd"], t["_dir"]), []).append(t)
    for (src, cmd, cwd), ts in by_src.items():
        live = live_lines(src, cmd, cwd)
        if live is None:
            continue
        for t in ts:
            # A tag on its own line expands to nothing in flash: its definition follows.
            if t["line"] not in live and t["line"] + 1 not in live:
                t["absent"] = "left out"


def rule_regions(tags):
    """Each absent code tag's region as the rules choose it (t["rule"]), from its source's compile
    command run on a probe that expands ESP_SDR_REGION_CODE()."""
    import subprocess
    import tempfile

    by_src = {}
    for t in absent_code(tags):
        by_src.setdefault((t["file"], t["_cmd"], t["_dir"], t.get("file_q")), []).append(t)
    for (src, cmd, cwd, fq), ts in by_src.items():
        out = preprocess_args(cmd, src)
        with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as f:
            if fq:
                f.write(f"#define ESP_SDR_PLACE_FILE {fq}\n")
            f.write("#include <esp_sdr/esp_sdr_place.h>\n")
            for i, t in enumerate(ts):
                f.write(f"@@{i} ESP_SDR_REGION_CODE({t['cat']}, {t['name']})\n")
            probe = f.name
        try:
            r = subprocess.run(out + ["-E", "-P", probe], cwd=cwd, capture_output=True, text=True)
        finally:
            Path(probe).unlink()
        for m in re.finditer(r"^@@(\d+) (\S+)", r.stdout, re.M):
            ts[int(m.group(1))]["rule"] = CODE_REGION.get(m.group(2), m.group(2))


def inline_sites(elf_path, tags):
    """The functions DWARF says each absent code tag was inlined into: t["inlined"], a sorted list
    of (caller, region)."""
    from elftools.elf.elffile import ELFFile
    from elftools.elf.constants import SH_FLAGS

    want = {}
    for t in absent_code(tags):
        want.setdefault(Path(t["file"]).name, {}).setdefault(t["name"], []).append(t)

    def name_of(die, depth=0):
        if die is None or depth > 4:
            return None
        if "DW_AT_name" in die.attributes:
            return die.attributes["DW_AT_name"].value.decode(errors="replace")
        for ref in ("DW_AT_abstract_origin", "DW_AT_specification"):
            if ref in die.attributes:
                try:
                    return name_of(die.get_DIE_from_attribute(ref), depth + 1)
                except Exception:
                    return None
        return None

    def low_pc(die):
        for a in ("DW_AT_low_pc", "DW_AT_entry_pc"):
            if a in die.attributes and isinstance(die.attributes[a].value, int):
                return die.attributes[a].value
        return None

    with open(elf_path, "rb") as f:
        elf = ELFFile(f)
        if not want or not elf.has_dwarf_info():
            return
        secs = sorted((s["sh_addr"], s["sh_addr"] + s["sh_size"], s.name)
                      for s in elf.iter_sections()
                      if s["sh_flags"] & SH_FLAGS.SHF_ALLOC and s["sh_size"]
                      and not is_dummy(s.name))
        for cu in elf.get_dwarf_info().iter_CUs():
            top = cu.get_top_DIE()
            cu_name = name_of(top) or ""
            names = want.get(cu_name.rsplit("/", 1)[-1])
            if not names:
                continue
            for die in cu.iter_DIEs():
                if (die.tag != "DW_TAG_inlined_subroutine"
                        or "DW_AT_abstract_origin" not in die.attributes):
                    continue
                callee = name_of(die)
                if callee not in names:
                    continue
                addr, caller, p = low_pc(die), None, die.get_parent()
                while p is not None and p.tag != "DW_TAG_subprogram":
                    p = p.get_parent()
                if p is not None:
                    caller = name_of(p)
                    addr = addr if addr is not None else low_pc(p)
                if addr is None:
                    continue  # an abstract instance's inlining, no code of its own
                sec = next((n for lo, hi, n in secs if lo <= addr < hi), None)
                if sec is None:
                    continue  # in a function the linker removed: that copy never runs
                for t in names[callee]:
                    t.setdefault("inlined", set()).add((caller or "?", region_of(sec)))
    for ts in want.values():
        for group in ts.values():
            for t in group:
                if "inlined" in t:
                    t["inlined"] = sorted(t["inlined"])


def linked_sections(map_text):
    """The linked input sections of a map, address order: (name, address, size, object)."""
    at = map_text.find("Linker script and memory map")
    secs, cur = [], None
    for line in map_text[max(at, 0):].splitlines():
        m = re.match(r"^ (\.\S+)\s*$", line)
        if m:
            cur = m.group(1)
            continue
        m = re.match(r"^ (\.\S+)?\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+\(\S+\))$", line)
        if m:
            addr, size = int(m.group(2), 16), int(m.group(3), 16)
            if addr and size:
                secs.append((m.group(1) or cur, addr, size, m.group(4)))
        cur = None
    return sorted(secs, key=lambda x: x[1])


def untagged(elf_path, cc_path, tags, threshold):
    """Lines for the input sections of the repository's own sources of at least threshold bytes
    whose symbols carry no tag."""
    from elftools.elf.elffile import ELFFile
    from elftools.elf.constants import SH_FLAGS

    repo = str(Path(__file__).resolve().parents[2]) + "/"
    build = Path(cc_path).resolve().parent
    ours = set()
    for e in json.loads(Path(cc_path).read_text()):
        src = Path(e.get("directory", "."), e["file"]).resolve()
        out = e.get("output")
        if out is None:
            m = re.search(r"(?:^|\s)-o\s*(\S+)", e.get("command", ""))
            out = m.group(1) if m else None
        if out is None or not str(src).startswith(repo):
            continue
        out = Path(e.get("directory", "."), out).resolve()
        try:
            rel = out.relative_to(build).as_posix()
        except ValueError:
            continue
        # The archive CMake writes a target's objects into: <dir>/lib<target>.a, the app's app/.
        m = re.match(r"^(?:(.*)/)?CMakeFiles/([^/]+)\.dir/", rel)
        if m:
            d, target = m.group(1), m.group(2)
            archive = "app/libapp.a" if d is None and target == "app" else \
                (f"{d}/" if d else "") + f"lib{target}.a"
            ours.add(f"{archive}({out.name})")
    tagged = {base for t in tags for base in [t["name"]] + [x[0] for x in t["symbols"]]}
    tagged |= {f"_k_mem_slab_buf_{t['name']}" for t in tags}
    tagged |= {re.sub(CLONE + "$", "", n) for n in tagged}
    with open(elf_path, "rb") as f:
        elf = ELFFile(f)
        osecs = sorted((s["sh_addr"], s["sh_addr"] + s["sh_size"], s.name)
                       for s in elf.iter_sections()
                       if s["sh_flags"] & SH_FLAGS.SHF_ALLOC and s["sh_size"]
                       and not is_dummy(s.name))
        syms = []
        tab = elf.get_section_by_name(".symtab")
        for sym in (tab.iter_symbols() if tab is not None else []):
            if sym["st_info"]["type"] in ("STT_FUNC", "STT_OBJECT") and sym["st_size"]:
                syms.append((sym["st_value"], sym.name))
    syms.sort()
    starts = [a for a, _ in syms]
    map_path = Path(elf_path).with_suffix(".map")
    out = []
    import bisect
    for name, addr, size, obj in linked_sections(map_path.read_text(errors="replace")):
        m = re.search(r"\(([^()]+)\.obj\)$", obj)
        if size < threshold or m is None or obj not in ours:
            continue
        lo = bisect.bisect_left(starts, addr)
        hi = bisect.bisect_left(starts, addr + size)
        names = sorted({n for _, n in syms[lo:hi]})
        bases = {re.sub(r"[.$]\d+$", "", re.sub(CLONE + "$", "", n)) for n in names}
        if not names or bases & tagged or len(names) > 8 or name in (".rodata", ".data", ".bss"):
            continue  # a table of many objects, or a file's unnamed constants: not one item
        sec = next((n for a, b, n in osecs if a <= addr < b), "?")
        out.append((size, f"    {m.group(1)}: {', '.join(names)} {size} B ({region_of(sec)})"))
    lines = [f"  untagged, {threshold} B or more: {len(out)}"]
    return lines + [line for _, line in sorted(out, key=lambda x: -x[0])]


def category_lines(tags):
    lines = ["  categories, bytes by region (symbols):"]
    table = {}
    for t in tags:
        for _, _, size, _, region in t["symbols"]:
            cell = table.setdefault(t["cat"], {}).setdefault(region, [0, 0])
            cell[0] += size
            cell[1] += 1
    for cat in sorted(table):
        cells = ", ".join(f"{r} {b} ({n})" for r, (b, n) in sorted(table[cat].items()))
        lines.append(f"    {cat:13s} {cells}")
    whats = {"no symbol": "with no symbol (inlined, or left out by #if)",
             "left out": "in code this build leaves out by #if",
             "unused": "removed by the linker",
             "host area": "in the host's area, placed when the extension loads"}
    for why, what in whats.items():
        gone = [t for t in tags if t.get("absent") == why]
        if gone:
            lines.append(f"  tagged, {what}: {len(gone)}")
            for t in sorted(gone, key=lambda t: (t["file"], t.get("ext") or "", t["name"])):
                ext = f" ({t['ext']})" if t.get("ext") else ""
                line = f"    {Path(t['file']).name}{ext}: {t['cat']} {t['name']}"
                if t.get("rule"):
                    line += f", ruled {t['rule']}"
                if t.get("inlined"):
                    line += ", inlined into " + ", ".join(
                        f"{c} ({r}){' !' if t.get('rule') and r != t['rule'] else ''}"
                        for c, r in t["inlined"])
                lines.append(line)
    return lines


def dram_user_end(elf_path):
    """DRAM_USER_END from the build's linker script, else the ESP32-S3's value."""
    for cmd in (elf_path.parent / "linker.cmd", elf_path.parent / "linker_zephyr_pre0.cmd"):
        if cmd.is_file():
            m = re.search(r"DRAM_USER_END\s*=\s*(0x[0-9a-fA-F]+)", cmd.read_text(errors="replace"))
            if m:
                return int(m.group(1), 16)
    cfg = elf_path.parent / ".config"
    if cfg.is_file() and "CONFIG_SOC_SERIES_ESP32S3=y" in cfg.read_text():
        return 0x3FCE9704
    return None


def report(elf_path, budget=None, tags=None, inlined=False, untagged_min=None, cc=None):
    secs, syms = load(elf_path)
    lines = [f"{elf_path}"]
    totals, spans, banks, other = {}, {}, [], []
    for name, addr, size in secs:
        if is_dummy(name):
            continue  # address space held for the other bus's view of the same RAM
        if name.startswith(".esp_sdr_bank"):
            banks.append((name, size))
            continue
        for region, pat in REGIONS:
            if pat.search(name):
                totals[region] = totals.get(region, 0) + size
                lo, hi = spans.get(region, (addr, addr + size))
                spans[region] = (min(lo, addr), max(hi, addr + size))
                break
        else:
            other.append((name, addr, size))
    for name, addr, size in other:
        # Iterable sections and the like: the region nearest in address, within 64 KiB.
        near = min(spans.items(), default=None,
                   key=lambda r: 0 if r[1][0] <= addr < r[1][1] else min(abs(addr - r[1][0]),
                                                                         abs(addr - r[1][1])))
        if near is not None and min(abs(addr - near[1][0]), abs(addr - near[1][1])) < 0x10000:
            totals[near[0]] += size
        else:
            totals[name] = size
    for region in [r for r, _ in REGIONS] + sorted(set(totals) - {r for r, _ in REGIONS}):
        if region in totals:
            lines.append(f"  {region:16s} {totals[region]:9d} B")
    for name, size in banks:
        lines.append(f"  {name:16s} {size:9d} B (receive bank)")
    if "_libc_heap_size" in syms and "_end" in syms:
        lines.append(f"  free below the banks: {syms['_libc_heap_size']} B "
                     f"(_end 0x{syms['_end']:08x})")
    if "__esp_sdr_high_end" in syms:
        end = dram_user_end(elf_path)
        if end is not None:
            lines.append(f"  free in the high region: {end - syms['__esp_sdr_high_end']} B "
                         f"(end 0x{syms['__esp_sdr_high_end']:08x}, limit 0x{end:08x})")
    for start, stop in HOT_MARKERS:
        if start in syms and stop in syms:
            size = syms[stop] - syms[start]
            extra = f" of {budget} B" if budget else ""
            lines.append(f"  hot block: {size} B{extra} at 0x{syms[start]:08x}")
            break
    if tags is not None:
        hot = next(((syms[a], syms[b]) for a, b in HOT_MARKERS if a in syms and b in syms), None)
        resolve(tags, elf_path, hot)
        if inlined:
            left_out(tags)
            rule_regions(tags)
            inline_sites(elf_path, tags)
        lines += category_lines(tags)
        if untagged_min:
            lines += untagged(elf_path, cc, tags, untagged_min)
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("build")
    ap.add_argument("--out")
    ap.add_argument("--sources")
    ap.add_argument("--manifest")
    ap.add_argument("--inlined", action="store_true")
    ap.add_argument("--untagged", type=int, metavar="BYTES")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()
    if args.quiet:
        try:
            return run(args)
        except Exception as e:  # the post-build step must not fail a build
            print(f"place_report.py: warning: no report: {e}", file=sys.stderr)
            return 0
    return run(args)


def run(args):
    elf = find_elf(args.build)
    budget = None
    cfg = elf.parent / ".config"
    if cfg.is_file():
        m = re.search(r"^CONFIG_ESP_SDR_HOT_TEXT_BUDGET=(\d+)", cfg.read_text(), re.M)
        budget = int(m.group(1)) if m else None
    cc = Path(args.sources) if args.sources else elf.parent.parent / "compile_commands.json"
    tags = scan_sources(cc) if cc.is_file() else None
    text = report(elf, budget, tags, args.inlined, args.untagged, cc)
    if not args.quiet:
        print(text)
    if args.out:
        Path(args.out).write_text(text + "\n")
    if args.manifest and tags is not None:
        public = [{k: v for k, v in t.items() if not k.startswith("_")} for t in tags]
        Path(args.manifest).write_text(json.dumps(public, indent=1, default=list) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
