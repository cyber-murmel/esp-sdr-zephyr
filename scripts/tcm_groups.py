#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bytes per 128 KiB TCM group of an ESP32-S31 image, and a check of the transmit layout.

    tcm_groups.py <zephyr.elf or a build directory> [--tx]
    tcm_groups.py --selftest

Lists the allocated sections in 0x2F000000..0x2F080000 by group and prints the totals. --tx
(CONFIG_ESP_SDR_S31_TXMEM) exits 1 when a section lies in groups 1 or 2 (0x2F020000..0x2F060000),
reaches 0x2F07AFB0 (the ROM boot stack), or when the libc arena (_end up to _heap_sentry) is not
in group 0. An ELF with no section in the TCM, or without those symbols, fails too. With PSRAM
(a section .esp_sdr_psram, CONFIG_ESP_SDR_S31_TXMEM_PSRAM) it also checks where the threads' stacks and
the receive pool are: the stacks of the threads with slack in PSRAM, everything the stream's timing, CPU 1 and
the MAC use in internal RAM (the section names carry a counter that changes with the configuration; this is
where a stack that stayed behind shows). The PSRAM and LP RAM sections are listed and totalled too.
readelf comes from $READELF or the PATH (riscv64-zephyr-elf-readelf, or any readelf).
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

BASE = 0x2F000000
GROUP = 0x20000
LIMIT = 0x2F07AFB0
HEAP_MAX = BASE + GROUP
PSRAM_LO, PSRAM_HI = 0x50000000, 0x58000000
LPRAM_LO, LPRAM_HI = 0x2E000000, 0x2E008000
# Stacks defined by Zephyr or the app, which esp_sdr_txmem_s31.ld / the app's place.h send to PSRAM
IN_PSRAM = ("rx_stack", "shell_uart_stack", "mgmt_stack", "logging_stack", "sys_work_q_stack", "z_main_stack",
            "ctl_stack")
# What the stream's timing, CPU 1, the interrupts and the MAC use: never in PSRAM
IN_INTERNAL = ("tx_stack", "st_stack", "z_interrupt_stacks", "z_idle_stacks", "net_buf_data_rx_bufs",
               "_net_buf_rx_bufs", "dwmac_instance", "kheap__system_heap", "z_interrupt_stacks")
# Set up by kernel init hooks before PRE_KERNEL_1 priority 40, when PSRAM becomes accessible (a store fault at
# boot), or read in interrupts: no symbol with these prefixes may lie in PSRAM or LP RAM
NEVER_EXT_PREFIX = ("kheap_", "_k_mem_slab_buf_", "z_idle_stacks", "z_interrupt_stacks", "_k_heap_", "_k_mem_slab_")


def readelf():
    for c in (os.environ.get("READELF"), "riscv64-zephyr-elf-readelf", "readelf"):
        if c and shutil.which(c):
            return shutil.which(c)
    sys.exit("tcm_groups: no readelf")


def run(*args):
    return subprocess.run([readelf(), *args], capture_output=True, text=True, check=True).stdout


def parse_sections(text):
    """(name, addr, size) of the allocated sections in the TCM, from readelf -SW."""
    out = []
    for m in re.finditer(r"^\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+([0-9a-f]{8,16})\s+[0-9a-f]+\s+"
                         r"([0-9a-f]+)\s+[0-9a-f]+\s+(\S*)\s+\d+\s+\d+\s+\d+\s*$", text, re.M):
        name, addr, size, flg = m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4)
        if "A" in flg and size and BASE <= addr < BASE + 8 * GROUP:
            out.append((name, addr, size))
    return sorted(out, key=lambda s: s[1])


def parse_symbols(text):
    """{name: value} of _end and _heap_sentry, from readelf -sW."""
    syms = {}
    for m in re.finditer(r"^\s*\d+:\s+([0-9a-f]+)\s+\d+\s+\S+\s+\S+\s+\S+\s+\S+\s+(_end|_heap_sentry)$",
                         text, re.M):
        syms[m.group(2)] = int(m.group(1), 16)
    return syms


def parse_sections_ext(text):
    """(name, addr, size) of the allocated sections in PSRAM and LP RAM."""
    out = []
    for m in re.finditer(r"^\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+([0-9a-f]{8,16})\s+[0-9a-f]+\s+"
                         r"([0-9a-f]+)\s+[0-9a-f]+\s+(\S*)\s+\d+\s+\d+\s+\d+\s*$", text, re.M):
        name, addr, size, flg = m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4)
        if "A" in flg and size and (PSRAM_LO <= addr < PSRAM_HI or LPRAM_LO <= addr < LPRAM_HI):
            out.append((name, addr, size))
    return sorted(out, key=lambda s: s[1])


def parse_all_symbols(text):
    """{name: [(value, size)]} of every symbol, from readelf -sW."""
    syms = {}
    for m in re.finditer(r"^\s*\d+:\s+([0-9a-f]+)\s+(\d+)\s+\S+\s+\S+\s+\S+\s+\S+\s+(\S+)$", text, re.M):
        syms.setdefault(m.group(3), []).append((int(m.group(1), 16), int(m.group(2))))
    return syms


def check_placement(allsyms, ext):
    """Problems of where the stacks and the receive pool are, when the image has PSRAM sections for them."""
    if not any(n == ".esp_sdr_psram" for n, _, _ in ext):
        return []
    bad = []
    for name, lst in allsyms.items():
        if name.startswith(NEVER_EXT_PREFIX):
            for a, _ in lst:
                if PSRAM_LO <= a < PSRAM_HI or LPRAM_LO <= a < LPRAM_HI:
                    bad.append(f"{name} {a:#x} is in PSRAM or LP RAM (set up before PSRAM works, or used in interrupts)")
    for name in IN_PSRAM:
        for a, _ in allsyms.get(name, []):
            if not PSRAM_LO <= a < PSRAM_HI:
                bad.append(f"{name} {a:#x} is not in PSRAM")
        if name not in allsyms and name != "ctl_stack":
            bad.append(f"{name} not found")
    for name in IN_INTERNAL:
        for a, _ in allsyms.get(name, []):
            if not BASE <= a < BASE + 8 * GROUP:
                bad.append(f"{name} {a:#x} is not in internal RAM")
    return bad


def check(secs, syms):
    """Problems of the transmit layout, as strings."""
    bad = []
    if not secs:
        return ["no section in 0x2F000000..0x2F080000: not an S31 image"]
    for name, addr, size in secs:
        end = addr + size
        if end > LIMIT or (addr < HEAP_MAX < end) or HEAP_MAX <= addr < BASE + 3 * GROUP:
            bad.append(f"section {name} {addr:#x}..{end:#x}")
    if "_end" not in syms or "_heap_sentry" not in syms:
        bad.append("_end or _heap_sentry missing")
    elif not syms["_end"] <= syms["_heap_sentry"] <= HEAP_MAX:
        bad.append(f"libc arena {syms['_end']:#x}..{syms['_heap_sentry']:#x} not in group 0")
    return bad


def totals(secs):
    tot = [0] * 4
    for _, addr, size in secs:
        for g in range(4):
            lo, hi = BASE + g * GROUP, BASE + (g + 1) * GROUP
            tot[g] += max(0, min(addr + size, hi) - max(addr, lo))
    return tot


SEC = """  [Nr] Name              Type            Addr     Off    Size   ES Flg Lk Inf Al
  [ 6] .iram0.text       PROGBITS        2f000000 000240 01079c 00  AX  0   0 64
  [11] .dram0.bss        NOBITS          2f013b20 013cf0 004390 0a  WA  0   0 16
  [27] .esp_sdr_grp3     NOBITS          2f060000 047500 018040 00  WA  0   0 64
  [28] .text             PROGBITS        40000000 000000 001000 00  AX  0   0  4
"""


def sym(v, n):
    return f"   123: {v:08x}     0 NOTYPE  GLOBAL DEFAULT  ABS {n}\n"


def selftest():
    secs = parse_sections(SEC)
    assert [s[0] for s in secs] == [".iram0.text", ".dram0.bss", ".esp_sdr_grp3"], secs
    assert secs[1][2] == 0x4390
    ok = parse_symbols(sym(0x2F017F00, "_end") + sym(0x2F020000, "_heap_sentry"))
    assert check(secs, ok) == []
    assert check([], ok), "empty ELF must fail"
    assert check(secs, {}), "no symbols must fail"
    assert check(secs, {"_end": 0x2F017F00, "_heap_sentry": 0x2F07AFB0}), "arena in group 3"
    assert check(secs, {"_end": 0x2F058200, "_heap_sentry": 0x2F058200}), "end in group 2"
    bad = parse_sections(SEC + "  [30] .x   NOBITS 2f030000 000000 000100 00 WA 0 0 4\n")
    assert check(bad, ok), "section in group 1"
    bad = parse_sections(SEC + "  [30] .y   NOBITS 2f07aff0 000000 000100 00 WA 0 0 4\n")
    assert check(bad, ok), "section past the ROM stack"
    ext = parse_sections_ext(SEC + "  [29] .esp_sdr_psram NOBITS 50000000 000000 004000 00 WA 0 0 64\n"
                             "  [30] .rtc_noinit NOBITS 2e000000 000000 000100 00 WA 0 0 4\n")
    assert [e[0] for e in ext] == [".rtc_noinit", ".esp_sdr_psram"] or [e[0] for e in ext] == [".esp_sdr_psram", ".rtc_noinit"]
    good = {n: [(0x50000100, 1)] for n in IN_PSRAM}
    good.update({n: [(0x2F060000, 1)] for n in IN_INTERNAL})
    assert check_placement(good, ext) == []
    assert check_placement(good, []) == [], "no PSRAM section: nothing to check"
    assert check_placement({**good, "rx_stack": [(0x2F072F00, 4096)]}, ext), "a stack left behind"
    assert check_placement({**good, "tx_stack": [(0x50000000, 3072)]}, ext), "the flip stack in PSRAM"
    assert check_placement({k: v for k, v in good.items() if k != "mgmt_stack"}, ext), "a stack not found"
    assert check_placement({**good, "kheap_shell_uart_history_heap": [(0x50002308, 512)]}, ext), "a k_heap in PSRAM"
    assert check_placement({**good, "_k_mem_slab_buf_tx_pkts": [(0x50002308, 288)]}, ext), "a slab buffer in PSRAM"
    assert check_placement({**good, "z_main_stack": [(0x2F075300, 3072)]}, ext), "the pattern missed the main stack"
    print("tcm_groups selftest: ok")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf", nargs="?")
    ap.add_argument("--tx", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.elf:
        ap.error("an ELF or build directory")
    p = Path(a.elf)
    if p.is_dir():
        p = p / "zephyr" / "zephyr.elf"
    secs = parse_sections(run("-SW", str(p)))
    symtext = run("-sW", str(p))
    syms = parse_symbols(symtext)
    ext = parse_sections_ext(run("-SW", str(p)))
    for name, addr, size in secs:
        print(f"  {addr:#010x}..{addr + size:#010x} {size:7d} B  {name}")
    for name, addr, size in ext:
        print(f"  {addr:#010x}..{addr + size:#010x} {size:7d} B  {name}")
    psram = sum(sz for _, a, sz in ext if PSRAM_LO <= a < PSRAM_HI)
    lpram = sum(sz for _, a, sz in ext if LPRAM_LO <= a < LPRAM_HI)
    print(f"PSRAM ({PSRAM_LO:#010x}): {psram} B in sections of the image (the heap and bss of .ext_ram.data included)")
    print(f"LP RAM ({LPRAM_LO:#010x}): {lpram} B of {LPRAM_HI - LPRAM_LO}")
    for g, t in enumerate(totals(secs)):
        print(f"group {g} ({BASE + g * GROUP:#010x}): {t} B of {GROUP}")
    if "_end" in syms and "_heap_sentry" in syms:
        print(f"libc arena {syms['_end']:#x}..{syms['_heap_sentry']:#x}: "
              f"{syms['_heap_sentry'] - syms['_end']} B")
    if a.tx:
        bad = check(secs, syms) + check_placement(parse_all_symbols(symtext), ext)
        if bad:
            print("FAIL: " + "; ".join(bad))
            return 1
        print("PASS: groups 1 and 2 hold nothing, group 3 ends below the ROM boot stack, "
              "the libc arena is in group 0"
              + (", the stacks with slack are in PSRAM and the stream's, the interrupts' and the MAC's "
                 "memory is internal" if any(n == ".esp_sdr_psram" for n, _, _ in ext) else ""))
    return 0


sys.exit(main())
