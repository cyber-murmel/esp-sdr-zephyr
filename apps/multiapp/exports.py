#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Export from the multiapp host what its extensions import.

Reads each extension's undefined symbols (a shared library's dynamic symbols, a
relocatable object's symbol table) and writes three C files for the host: exports of those symbols by name (group MULTIAPP, through asm labels, so
no header has to declare them), the log modules the extensions declare, and
the objects they describe in .multiapp_host (struct multiapp_host_object). A
symbol the host lacks fails the host's link. What a module defines comes from
that module (llext finds it among the loaded extensions), not from the host.

"exports.py check-kernel" checks a hot-swap kernel (multiapp_kernel() in CMakeLists.txt) against
that table, which it does not join: a kernel imports only what the host exports for the apps
already, or what the app it is for defines, so that the host's build id and every file stay valid
when a kernel changes.
"""

import argparse
import re
import struct
import sys
from pathlib import Path

from elftools.elf.elffile import ELFFile

# An optimized extension may reference only one of a module's two data items.
LOG_DATA = re.compile(r"^log_(?:const|dynamic)_(\w+)$")

# struct multiapp_host_object (include/multiapp/multiapp.h), little endian.
HOST_OBJECT = struct.Struct("<56sI3I")
HOST_POOL = 1
HOST_AREA = 2
HOST_PSRAM = 3

# From multiapp_extension(<name> ...): lib<name>_ext_llext_lib.so, a shared library (Xtensa), or
# <name>_ext_llext_lib.obj, a relocatable object (RISC-V).
LIB_NAME = re.compile(r"(?:lib)?(\w+)_ext_llext_lib\.(?:so|obj)$")


def symbols(elf):
    """A shared library's dynamic symbols, a relocatable object's symbol table."""
    if elf["e_type"] == "ET_REL":
        section = elf.get_section_by_name(".symtab")
    else:
        section = elf.get_section_by_name(".dynsym")
    return section.iter_symbols() if section is not None else ()


def nobits_sections(path):
    """The names of the NOBITS sections of a relocatable object (RISC-V): llext loads one at most."""
    with open(path, "rb") as f:
        elf = ELFFile(f)
        if elf["e_type"] != "ET_REL":
            return []
        return [s.name for s in elf.iter_sections() if s["sh_type"] == "SHT_NOBITS" and s["sh_size"]]


# Kernel objects the host initializes at boot by walking its own iterable sections (init_static_heaps(), the slabs' free lists,
# net_buf_pool_init(), z_init_static_threads()): inside an extension nothing walks them, so a k_heap is an uninitialized
# heap (a load fault at its first allocation, a board run: the dummy shell's history heap) and a K_THREAD_DEFINE thread
# never starts. The semaphores, mutexes, queues and work items are initialized at compile time and are fine.
BOOT_INIT_SECTION = re.compile(r"^\._(k_heap|k_mem_slab|net_buf_pool|static_thread_data)\.")
# One such object is unused: every shell defines its history heap (Z_SHELL_HISTORY_DEFINE), with history on or off,
# and only reaches it with CONFIG_SHELL_HISTORY (shell.h: .history is NULL otherwise). The S3's MicroPython extension
# has a dummy shell for esdr.shell() and no history, so its heap is let through only while neither the host's
# configuration nor the extension's own (llext/<app>_kconfig.h) turns history on.
SHELL_HISTORY_HEAP = re.compile(r"^\._k_heap\.static\.shell_\w+_history_heap_$")
SHELL_HISTORY_ON = re.compile(r"^#define CONFIG_SHELL_HISTORY 1$", re.M)


def shell_history_on(lib, app):
    """Whether the host's autoconf.h or the extension's <app>_kconfig.h next to lib enables CONFIG_SHELL_HISTORY
    (True when either cannot be read: then the heap counts as used)."""
    lib = Path(lib)
    for header in (lib.parent.parent / "zephyr" / "include" / "generated" / "zephyr" / "autoconf.h",
                   lib.parent / f"{app}_kconfig.h"):
        try:
            if SHELL_HISTORY_ON.search(header.read_text()):
                return True
        except OSError:
            return True
    return False


def boot_init_objects(lib):
    """The sections of the extension's own objects (CMakeFiles/<app>_ext_llext_lib.dir, before the link merges
    the kernel sections into .data) that hold kernel objects needing the host's boot-time initialization."""
    lib = Path(lib)
    m = LIB_NAME.search(lib.name)
    if m is None:
        return []
    objdir = lib.parent.parent / "CMakeFiles" / f"{m.group(1)}_ext_llext_lib.dir"
    found = []
    if not objdir.is_dir():
        return found
    history_unused = not shell_history_on(lib, m.group(1))
    # Only the objects the link used: the directory also holds those of sources dropped since. The FILE symbols of the
    # link's output name the sources (a path or a plain name, "mp_repl_s31.c"), the objects are "<source>.obj" or ".o".
    linked = set()
    try:
        with open(lib, "rb") as f:
            symtab = ELFFile(f).get_section_by_name(".symtab")
            if symtab is not None:
                linked = {Path(sym.name).name for sym in symtab.iter_symbols() if sym["st_info"]["type"] == "STT_FILE"}
    except Exception:
        linked = set()
    for obj in sorted(objdir.rglob("*.obj")) + sorted(objdir.rglob("*.o")):
        source = obj.name[:-4] if obj.name.endswith(".obj") else obj.name[:-2]
        if linked and source not in linked and Path(source).stem not in {Path(n).stem for n in linked}:
            continue
        try:
            with open(obj, "rb") as f:
                for sec in ELFFile(f).iter_sections():
                    if BOOT_INIT_SECTION.match(sec.name):
                        if history_unused and SHELL_HISTORY_HEAP.match(sec.name):
                            continue
                        found.append(f"{obj.name}: {sec.name}")
        except Exception:  # not an ELF object
            continue
    return found


def imports(path):
    with open(path, "rb") as f:
        return {
            sym.name
            for sym in symbols(ELFFile(f))
            if sym.name
            and sym["st_shndx"] == "SHN_UNDEF"
            and sym["st_info"]["bind"] == "STB_GLOBAL"
        }


def defines(path):
    """The global symbols an extension defines."""
    with open(path, "rb") as f:
        return {
            sym.name
            for sym in symbols(ELFFile(f))
            if sym.name
            and sym["st_shndx"] != "SHN_UNDEF"
            and sym["st_info"]["bind"] in ("STB_GLOBAL", "STB_WEAK")
        }


def weak_relocations(path):
    """Weak symbols an extension's relocations name, by symbol: llext's Xtensa loader writes a
    JMP_SLOT slot only for a STB_GLOBAL symbol, so a weak one keeps the linker's PLT
    address (unmapped at run time) and the call faults."""
    with open(path, "rb") as f:
        elf = ELFFile(f)
        found = set()
        for section in elf.iter_sections():
            if section.name not in (".rela.plt", ".rel.plt"):
                continue
            table = elf.get_section(section["sh_link"])
            for rel in section.iter_relocations():
                if rel["r_info_sym"] == 0:
                    continue
                sym = table.get_symbol(rel["r_info_sym"])
                if sym["st_info"]["bind"] == "STB_WEAK":
                    found.add(sym.name)
        return found


def host_objects(path):
    with open(path, "rb") as f:
        section = ELFFile(f).get_section_by_name(".multiapp_host")
        data = section.data() if section is not None else b""
    for offset in range(0, len(data) - HOST_OBJECT.size + 1, HOST_OBJECT.size):
        name, kind, *arg = HOST_OBJECT.unpack_from(data, offset)
        yield name.split(b"\0", 1)[0].decode(), kind, arg


def check_kernel(kernel, host_names, app_symbols, kernel_name="the kernel", app_name="the app"):
    """The problems of a kernel's library: what it imports that nothing provides, a weak
    relocation, host objects of its own, no descriptor. A list of messages, empty when it is fine."""
    problems = []
    weak = weak_relocations(kernel)
    if weak:
        problems.append(f"{kernel_name} relocates weak symbols ({', '.join(sorted(weak))}): "
                        "llext does not relocate their PLT/GOT slots; use strong symbols")
    objects = [name for name, _, _ in host_objects(kernel)]
    if objects:
        problems.append(f"{kernel_name} defines host objects ({', '.join(objects)}): a kernel "
                        "cannot have buffers or pools the host defines for it; use static data")
    if "multiapp_kernel" not in defines(kernel):
        problems.append(f"{kernel_name} does not define multiapp_kernel: use "
                        "MULTIAPP_KERNEL_DEFINE() (include/multiapp/stage.h)")
    missing = sorted(imports(kernel) - set(host_names) - set(app_symbols))
    if missing:
        problems.append(
            f"{kernel_name} imports {', '.join(missing)}, which the host does not export and "
            f"{app_name} does not define. A kernel may use only what the host already "
            "exports (the union of what the apps import: the host's build id stays valid only if "
            "kernels do not add to it) and the global symbols of its app (not static, not in a "
            "module). Make the symbol global in the app, or keep the kernel self-contained")
    return problems


def check_kernel_main(argv):
    parser = argparse.ArgumentParser(prog="exports.py check-kernel",
                                     description=check_kernel.__doc__.splitlines()[0])
    parser.add_argument("--kernel", required=True, help="the kernel's shared library or object")
    parser.add_argument("--app", required=True, help="the library or object of the app it is for")
    parser.add_argument("--names", required=True, help="the host's export list (exports.py --names)")
    parser.add_argument("--kernel-name", default="the kernel")
    parser.add_argument("--app-name", default="the app")
    parser.add_argument("--stamp", help="a file to write when the check passes")
    args = parser.parse_args(argv)
    with open(args.names) as f:
        host_names = {line.strip() for line in f if line.strip()}
    problems = check_kernel(args.kernel, host_names, defines(args.app), args.kernel_name,
                            args.app_name)
    if problems:
        sys.exit("exports.py: kernel check failed:\n  " + "\n  ".join(problems))
    if args.stamp:
        with open(args.stamp, "w") as f:
            f.write("ok\n")
    return 0


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "check-kernel":
        return check_kernel_main(sys.argv[2:])
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--exports", required=True, help="C file of exports to write")
    parser.add_argument("--logs", required=True, help="C file of log modules to write")
    parser.add_argument("--objects", required=True, help="C file of host objects to write")
    parser.add_argument("--names", help="a list of every name the host exports, one per line, for "
                        "the kernel check")
    parser.add_argument("--area-size", type=int, required=True,
                        help="bytes in the area the apps share (multiapp_area)")
    parser.add_argument("--module", action="append", default=[], metavar="NAME",
                        help="an extension that is a module: others import from it")
    parser.add_argument("--area-owner", action="append", default=[], metavar="NAME",
                        help="an extension that may have buffers in the area (an AREA app, wifi): "
                        "the area has one owner per boot, so any other extension's are an error")
    parser.add_argument("--psram-section", metavar="NAME",
                        help="the section that puts a host object in PSRAM, where "
                        "EXT_RAM_BSS_ATTR does not")
    parser.add_argument("libs", nargs="+", help="extension shared libraries or objects")
    args = parser.parse_args()
    psram_attr = (f'__attribute__((section("{args.psram_section}")))' if args.psram_section
                  else "EXT_RAM_BSS_ATTR")

    names = set()
    from_modules = set()
    objects = {}
    areas = {}
    for lib in args.libs:
        weak = weak_relocations(lib)
        if weak:
            sys.exit(f"exports.py: {lib} relocates weak symbols ({', '.join(sorted(weak))}). "
                     "llext does not relocate PLT/GOT slots of weak symbols, so the call would "
                     "jump to the linker's unmapped PLT stub. Use a strong function or a "
                     "function pointer set at run time (see wifitest_stop_hook in ext/wifitest.c)")
        nobits = nobits_sections(lib)
        if len(nobits) > 1:
            sys.exit(f"exports.py: {lib} has {len(nobits)} NOBITS sections ({', '.join(nobits)}); llext "
                     "loads one. Merge the others into .bss in ext/llext_riscv.ld")
        static = boot_init_objects(lib)
        if static:
            sys.exit(f"exports.py: {lib} defines kernel objects that need the host's boot-time initialization and get "
                     f"none in an extension: {'; '.join(static)}. Allocate them at run time (k_heap_init(), "
                     "k_mem_slab_init()) or leave the code that defines them out of the extension")
        names |= imports(lib)
        app = LIB_NAME.search(lib).group(1)
        if app in args.module:
            from_modules |= defines(lib)
        for name, kind, arg in host_objects(lib):
            if kind == HOST_AREA:
                areas.setdefault(app, []).append((name, arg[0], arg[1]))
            objects[name] = (kind, arg)
    # Every app's objects start at the area's beginning, so a boot's extensions overlap each
    # other there: only the extensions that own the area (never two in one boot) may have any.
    for app, objs in sorted(areas.items()):
        if app not in args.area_owner:
            sys.exit(f"exports.py: {app} has buffers in the area ({', '.join(o[0] for o in objs)}) "
                     f"but does not own it (multiapp_extension(AREA); owners: "
                     f"{', '.join(args.area_owner) or 'none'}). A stack or buffer placed in the "
                     "high region (ESP_SDR_PLACE_STACK, ESP_SDR_HIGH_BUF_DEFINE) lands there: pin "
                     "it with ESP_SDR_PIN_DATA_<app>_<name> (apps/multiapp/place.h)")
    # The objects are exported where they are defined.
    names = sorted(names - set(objects) - from_modules)
    if args.names:
        with open(args.names, "w") as out:
            out.write("".join(f"{n}\n" for n in sorted(set(names) | set(objects))))
    modules = sorted({m.group(1) for m in map(LOG_DATA.match, names) if m})

    with open(args.exports, "w") as out:
        out.write("/* Generated by exports.py: the symbols the extensions import. */\n\n")
        out.write("#include <zephyr/llext/symbol.h>\n\n")
        for i, name in enumerate(names):
            out.write(f'extern const char multiapp_import_{i} __asm__("{name}");\n')
            out.write(f"EXPORT_GROUP_SYMBOL_NAMED(MULTIAPP, multiapp_import_{i}, {name});\n")

    with open(args.logs, "w") as out:
        out.write("/* Generated by exports.py: the log modules the extensions declare. */\n\n")
        out.write("#include <zephyr/logging/log.h>\n\n")
        for module in modules:
            # What LOG_MODULE_REGISTER() creates, without making it this file's module.
            out.write(f"_LOG_MODULE_DATA_CREATE({module}, CONFIG_LOG_DEFAULT_LEVEL);\n")

    with open(args.objects, "w") as out:
        out.write("/* Generated by exports.py: objects the host defines for the extensions. */\n\n")
        out.write("#include <zephyr/llext/symbol.h>\n")
        out.write("#include <zephyr/net_buf.h>\n")
        out.write("#include <zephyr/sys/util.h>\n")
        out.write("#include <esp_attr.h>\n\n")
        for name, (kind, arg) in sorted(objects.items()):
            if kind == HOST_AREA:
                continue
            if kind == HOST_PSRAM:
                out.write(f"{psram_attr} uint8_t {name}[{arg[0]}] __aligned({arg[1]});\n")
                out.write(f"EXPORT_GROUP_SYMBOL(MULTIAPP, {name});\n")
                continue
            if kind != HOST_POOL:
                sys.exit(f"exports.py: {name}: unknown host object kind {kind}")
            count, size, ud_size = arg
            out.write(f"NET_BUF_POOL_FIXED_DEFINE({name}, {count}, {size}, {ud_size}, NULL);\n")
            out.write(f"EXPORT_GROUP_SYMBOL(MULTIAPP, {name});\n")
        # Every app's objects start at the area's beginning: one app per boot.
        out.write("\n/* The area the apps share, one per boot (src/apps.c). */\n")
        out.write("extern uint8_t multiapp_area[];\n")
        for app, objs in sorted(areas.items()):
            offset = 0
            out.write(f"\n/* {app} */\n")
            for name, size, align in objs:
                offset = (offset + align - 1) // align * align
                out.write(f"EXPORT_GROUP_SYMBOL_NAMED(MULTIAPP, multiapp_area[{offset}], {name});\n")
                offset += size
            if offset > args.area_size:
                sys.exit(f"exports.py: {app} needs {offset} bytes of the area, "
                         f"CONFIG_MULTIAPP_AREA_SIZE is {args.area_size}")
            out.write(f"/* {offset} of {args.area_size} bytes */\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
