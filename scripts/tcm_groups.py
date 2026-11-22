#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bytes per 128 KiB TCM group of an ESP32-S31 image, and a check of the transmit layout.

    tcm_groups.py <zephyr.elf or a build directory> [--tx | --multiapp]
    tcm_groups.py --selftest

Lists the allocated sections in 0x2F000000..0x2F080000 by group and prints the totals. --tx
(CONFIG_ESP_SDR_S31_TXMEM) exits 1 when a section lies in groups 1 or 2 (0x2F020000..0x2F060000),
reaches 0x2F07AFB0 (the ROM boot stack), or when the libc arena (_end up to _heap_sentry) is not
in group 0. An ELF with no section in the TCM, or without those symbols, fails too. With PSRAM
(a section .esp_sdr_psram, CONFIG_ESP_SDR_S31_TXMEM_PSRAM) it also checks where the threads' stacks and
the receive pool are: the stacks of the threads with slack in PSRAM, everything the stream's timing, CPU 1 and
the MAC use in internal RAM (the section names carry a counter that changes with the configuration; this is
where a stack that stayed behind shows). The PSRAM and LP RAM sections are listed and totalled too.
--mp-ext <micropython.llext> [--arena-min N] checks the MicroPython extension of the multiapp host (apps/multiapp/micropython.cmake):
it defines no thread stack in PSRAM (its bss is there) but the ones that never touch the flash, and the host's libc arena
(_end up to _heap_sentry), from which its thread's stack comes, is at least N bytes.
--multiapp checks the multiapp host's S31 placement (net_psram_s31.ld, multiapp_lpram_s31.ld): the net_buf data
pools (the MAC's and the USB classes'; MCUmgr's two, CPU copies, are in LP RAM with its work queue's stack) and the MAC's descriptors and SCSI buffers internal, no k_heap or slab in
PSRAM, no slab or interrupt/idle stack in LP RAM, no stack of a thread that touches the flash in PSRAM, the stream's
threads (app, USB device, UDC, CDC-ACM) internal, and that each pattern of the two linker snippets still hits the
object it names (they match counter-suffixed sections). osmosdr's USB batch buffers are in the module heap, not
checked here.
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


# multiapp host (--multiapp). Stacks of threads that touch the flash (the cache is off during a flash operation, a
# stack in PSRAM stalls), and the threads the osmosdr_hs stream and its USB path run on (the LP RAM is slow).
MA_FLASH_STACKS = ("z_main_stack", "shell_uart_stack", "sys_work_q_stack", "usbd_stack", "_k_thread_stack_usbd_msc",
                   "udc_dwc2_stack_0", "app_stack", "cdc_acm_stack", "smp_work_queue_stack",
                   "ssh_sh_stack", "sshd_work_stack")
# (and what the USB drive touches while a stream runs: the PC polls it and reads it; in LP RAM it cost 567 gaps
# in 60 s at 20 MS/s on the board)
MA_STREAM_STACKS = ("app_stack", "usbd_stack", "udc_dwc2_stack_0", "cdc_acm_stack", "_k_thread_stack_usbd_msc",
                    "multiapp_fat_fs", "flashdisk0_cache")
# DMA: the MAC's descriptors and net_buf data, USB's; only the DNS pools of net_buf data may be in PSRAM
MA_DMA_PREFIX = ("net_buf_data_", "dwmac_", "scsi_buf")
# Required objects (a rename or a counter shift that makes a pattern miss shows here): in PSRAM, in LP RAM,
# internal
MA_EXPECT_PSRAM = ("mgmt_stack", "rx_stack", "timer_task_stack", "work_q_stack", "service_thread_stack.1",
                   "net_buf_data_mdns_msg_pool", "net_buf_data_dns_msg_pool")
MA_EXPECT_LP = ("z_main_stack", "shell_uart_stack", "sys_work_q_stack",
                "kheap__system_heap", "kheap_shell_uart_history_heap")
# Only with deferred logging (the UART test variant logs immediately and has no buffer); only with MCUmgr
# (apps/common/net/mcumgr.conf): its work queue runs the image and file writes, so its stack must not be in PSRAM,
# and it and the two request/response buffers (CPU copies, no DMA) are in LP RAM
MA_OPTIONAL_LP = ("buf32", "smp_work_queue_stack", "net_buf_data_pkt_pool", "ssh_sh_stack", "sshd_work_stack")
# Only with SSH (apps/common/net/ssh.conf): the mbedTLS heap is a static buffer in PSRAM (formatted at POST_KERNEL,
# after PSRAM works, used by the server and sshd_work only), the server's object (kernel objects inside: a thread,
# a message queue) and the session's shell instance stay internal; the two stacks above are the LP RAM's
MA_OPTIONAL_PSRAM = ("_mbedtls_heap",)
MA_OPTIONAL_INTERNAL = ("ssh_server_instances", "ssh_sh_ctx")
MA_LP_OK = ("net_buf_data_pkt_pool",)
MA_EXPECT_INTERNAL = ("app_stack", "usbd_stack", "udc_dwc2_stack_0", "z_interrupt_stacks", "z_idle_stacks",
                      "_k_thread_stack_usbd_msc", "multiapp_fat_fs", "flashdisk0_cache",
                      "net_buf_data_rx_bufs", "net_buf_data_tx_bufs", "dwmac_rx_descs", "dwmac_tx_descs",
                      "dwmac_instance", "_system_heap")
MA_PSRAM_OK = ("net_buf_data_mdns_msg_pool", "net_buf_data_dns_msg_pool")


# MicroPython's extension (--mp-ext): its data and bss are in PSRAM, so a thread of it with a stack there must never touch the
# flash. Its own thread's stack is not a symbol: it comes from the libc arena (ext/mp/mp_repl_s31.c). Known stacks: esdr's
# radio worker (bursts only) and MicroPython's own (mp_stack_mem: its flash accesses run on a worker
# with an internal stack, and src/flashguard.c refuses a flash operation from a PSRAM stack at run time).
MP_EXT_STACKS_OK = ("worker_stack", "mp_stack_mem")
MP_STACK_NAME = re.compile(r"(^|_)stack($|_|\d)")


def parse_objects(text):
    """{name: [(value, size)]} of the data objects (not functions), from readelf -sW."""
    syms = {}
    for m in re.finditer(r"^\s*\d+:\s+([0-9a-f]+)\s+(\d+)\s+OBJECT\s+\S+\s+\S+\s+\S+\s+(\S+)$", text, re.M):
        syms.setdefault(m.group(3), []).append((int(m.group(1), 16), int(m.group(2))))
    return syms


def check_mp_ext(allsyms):
    """Problems of the MicroPython extension's stacks (data objects), as strings."""
    bad = []
    for name, lst in allsyms.items():
        if MP_STACK_NAME.search(name) and name not in MP_EXT_STACKS_OK and any(sz >= 256 for _, sz in lst):
            bad.append(f"{name}: a thread stack in the MicroPython extension (PSRAM): a thread that touches the flash "
                       f"stalls there; take it from the libc arena like mp_repl_s31.c, or list it in MP_EXT_STACKS_OK")
    return bad


MODULE_HEAP_SLACK = 1536
HEAP_REGIONS = (".module_fast", ".data", ".bss")


def module_heap_slack(heap, llexts):
    """Bytes left of the module heap after the modules' fast code, data and bss (each region aligned to 64)."""
    used = 0
    for f in llexts:
        for line in run("-SW", f).splitlines():
            m = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+[0-9a-f]+\s+[0-9a-f]+\s+([0-9a-f]+)", line)
            if m and m.group(1) in HEAP_REGIONS:
                used += (int(m.group(2), 16) + 63) // 64 * 64
    return heap - used


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


def check_multiapp(allsyms):
    """Problems of the multiapp host's placement (symbols only, no PSRAM section needed)."""
    bad = []
    ps = lambda a: PSRAM_LO <= a < PSRAM_HI
    lp = lambda a: LPRAM_LO <= a < LPRAM_HI
    for name, lst in allsyms.items():
        for a, _ in lst:
            if name.startswith(("kheap_", "_k_heap_", "_k_mem_slab_", "z_idle_stacks", "z_interrupt_stacks")) and ps(a):
                bad.append(f"{name} {a:#x} is in PSRAM (set up before PSRAM works)")
            if name.startswith(("_k_mem_slab_", "z_idle_stacks", "z_interrupt_stacks")) and lp(a):
                bad.append(f"{name} {a:#x} is in LP RAM")
            if name.startswith(MA_DMA_PREFIX) and name not in MA_PSRAM_OK and (ps(a) or (lp(a) and name not in MA_LP_OK)):
                bad.append(f"{name} {a:#x}: DMA data outside internal RAM")
    for name in MA_FLASH_STACKS:
        for a, _ in allsyms.get(name, []):
            if ps(a):
                bad.append(f"{name} {a:#x}: a thread that touches the flash has its stack in PSRAM")
    for name in MA_STREAM_STACKS:
        for a, _ in allsyms.get(name, []):
            if lp(a) or ps(a):
                bad.append(f"{name} {a:#x}: a stream thread's stack is outside internal RAM")
    for names, where, test in ((MA_EXPECT_PSRAM, "PSRAM", ps), (MA_EXPECT_LP, "LP RAM", lp),
                               (MA_EXPECT_INTERNAL, "internal RAM",
                                lambda a: BASE <= a < BASE + 8 * GROUP)):
        for name in names:
            if name not in allsyms:
                bad.append(f"{name} not found (a linker pattern or a name changed)")
            elif not all(test(a) for a, _ in allsyms[name]):
                bad.append(f"{name} is not in {where}")
    for name in MA_OPTIONAL_LP:
        if name in allsyms and not all(lp(a) for a, _ in allsyms[name]):
            bad.append(f"{name} is not in LP RAM")
    for name in MA_OPTIONAL_PSRAM:
        if name in allsyms and not all(ps(a) for a, _ in allsyms[name]):
            bad.append(f"{name} is not in PSRAM (ssh.conf, MBEDTLS_HEAP_CUSTOM_SECTION: net_psram_s31.ld did not catch it)")
    for name in MA_OPTIONAL_INTERNAL:
        if name in allsyms and not all(BASE <= a < BASE + 8 * GROUP for a, _ in allsyms[name]):
            bad.append(f"{name} is not in internal RAM")
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
    ma = {n: [(0x2F060000, 8)] for n in MA_EXPECT_INTERNAL + MA_FLASH_STACKS if n not in MA_OPTIONAL_LP}
    ma.update({n: [(0x2E000100, 64)] for n in MA_EXPECT_LP})
    ma.update({n: [(0x50000100, 64)] for n in MA_EXPECT_PSRAM})
    assert check_multiapp(ma) == [], check_multiapp(ma)
    assert check_multiapp({k: v for k, v in ma.items() if k != "work_q_stack"}), "a PSRAM pattern missed"
    assert check_multiapp({**ma, "rx_stack": [(0x2F060000, 8)]}), "a PSRAM stack stayed internal"
    assert check_multiapp({**ma, "z_main_stack": [(0x2F060000, 8)]}), "the main stack stayed internal"
    assert check_multiapp({k: v for k, v in ma.items() if k != "udc_dwc2_stack_0"}), "a hot object missing"
    assert check_multiapp({**ma, "shell_uart_stack": [(0x50000000, 4096)]}), "flash thread stack in PSRAM"
    assert check_multiapp({**ma, "app_stack": [(0x2E000400, 4096)]}), "stream stack in LP RAM"
    assert check_multiapp({**ma, "net_buf_data_rx_bufs": [(0x50000000, 3000)]}), "RX buffers in PSRAM"
    assert module_heap_slack.__doc__
    mcumgr = {"smp_work_queue_stack": [(0x2E001000, 3072)], "net_buf_data_pkt_pool": [(0x2E002000, 2900)]}
    assert check_multiapp({**ma, **mcumgr}) == [], check_multiapp({**ma, **mcumgr})
    assert check_multiapp({**ma, **mcumgr, "smp_work_queue_stack": [(0x50000100, 3072)]}), "MCUmgr stack in PSRAM"
    assert check_multiapp({**ma, **mcumgr, "smp_work_queue_stack": [(0x2F060000, 3072)]}), "MCUmgr stack not in LP RAM"
    assert check_multiapp({**ma, **mcumgr, "net_buf_data_pkt_pool": [(0x50000100, 2900)]}), "MCUmgr buffers in PSRAM"
    ssh = {"ssh_sh_stack": [(0x2E003000, 3072)], "sshd_work_stack": [(0x2E004000, 6144)],
           "_mbedtls_heap": [(0x50020000, 131072)], "ssh_server_instances": [(0x2F02A750, 25408)]}
    assert check_multiapp({**ma, **ssh}) == [], check_multiapp({**ma, **ssh})
    assert check_multiapp({**ma, **ssh, "ssh_sh_stack": [(0x50000100, 3072)]}), "the SSH shell's stack in PSRAM"
    assert check_multiapp({**ma, **ssh, "ssh_sh_stack": [(0x2F060000, 3072)]}), "the SSH shell's stack not in LP RAM"
    assert check_multiapp({**ma, **ssh, "sshd_work_stack": [(0x50000100, 6144)]}), "the work thread's stack in PSRAM"
    assert check_multiapp({**ma, **ssh, "_mbedtls_heap": [(0x2F060000, 131072)]}), "the mbedTLS heap internal"
    assert check_multiapp({**ma, **ssh, "ssh_server_instances": [(0x50000100, 25408)]}), "the server's object in PSRAM"
    objs = parse_objects("    10: 00045170  2048 OBJECT  GLOBAL DEFAULT    16 worker_stack\n"
                         "    11: 0001b190  32768 OBJECT  LOCAL  DEFAULT    16 mp_stack_mem\n"
                         "    12: 000099a2    30 FUNC    LOCAL  DEFAULT     3 adjust_stack\n")
    assert sorted(objs) == ["mp_stack_mem", "worker_stack"], objs
    mp = objs
    assert check_mp_ext(mp) == [], check_mp_ext(mp)
    assert check_mp_ext({**mp, "mp_stack": [(0x2000, 16384)]}), "a flash thread's stack in the extension"
    assert check_mp_ext({**mp, "scan_stack": [(0x2000, 3584)]}), "an unknown stack in the extension"
    assert check_mp_ext({**mp, "stack": [(0x2000, 512)]}), "a plain stack"
    assert check_multiapp({**ma, "kheap_x": [(0x50000000, 64)]}), "k_heap in PSRAM"
    assert check_multiapp({**ma, "_k_mem_slab_buf_rx_pkts": [(0x2E000000, 64)]}), "slab in LP RAM"
    print("tcm_groups selftest: ok")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf", nargs="?")
    ap.add_argument("--tx", action="store_true")
    ap.add_argument("--multiapp", action="store_true")
    ap.add_argument("--module-heap", metavar="SIZE:LLEXT[:LLEXT...]",
                    help="the module heap's size and the INTERNAL modules that load into it in a boot of "
                         "an app without BANK2 (rfprobe, capture): fails below 1536 bytes of slack")
    ap.add_argument("--mp-ext", metavar="LLEXT", help="the MicroPython extension: no thread stack of it in PSRAM "
                    "but the known ones")
    ap.add_argument("--arena-min", type=int, metavar="BYTES", help="with --mp-ext: the libc arena must have at least this")
    ap.add_argument("--arena-note", help="with --arena-min: what the minimum is made of (in the messages)")
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
    if a.multiapp:
        bad = check_multiapp(parse_all_symbols(symtext))
        if bad:
            print("FAIL: " + "; ".join(bad))
            return 1
        print("PASS: net_buf pools and MAC descriptors internal, no k_heap or slab in PSRAM, no flash thread "
              "stack in PSRAM, the stream's stacks internal, the snippets' patterns hit their objects")
    if a.mp_ext:
        bad = check_mp_ext(parse_objects(run("-sW", a.mp_ext)))
        if a.arena_min is not None:
            arena = syms.get("_heap_sentry", 0) - syms.get("_end", 0)
            note = f" = {a.arena_note}" if a.arena_note else ""
            print(f"MicroPython: libc arena {arena} B, needs {a.arena_min} B{note}")
            if arena < a.arena_min:
                bad.append(f"libc arena {arena} B < {a.arena_min} B{note}: the flash worker's stack (taken from the arena) would not fit "
                           "(worker stack = its measured peak / 0.9 from esdr.stacks(): CONFIG_MULTIAPP_MICROPYTHON_WORKER_STACK; "
                           "the rest: CONFIG_MULTIAPP_MICROPYTHON_ARENA_USE)")
        if bad:
            print("FAIL: " + "; ".join(bad))
            return 1
        print("PASS: no flash thread stack in the MicroPython extension, the arena holds its thread")
    if a.module_heap:
        size, *mods = a.module_heap.split(":")
        slack = module_heap_slack(int(size), mods)
        print(f"module heap: {slack} bytes free after {', '.join(Path(f).stem for f in mods)}")
        if slack < MODULE_HEAP_SLACK:
            print(f"FAIL: module heap slack {slack} < {MODULE_HEAP_SLACK} (a hot swap holds an old and a new kernel)")
            return 1
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
