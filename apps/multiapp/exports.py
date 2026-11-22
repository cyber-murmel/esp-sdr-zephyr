#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Export from the multiapp host what its extensions import.

Reads each extension's undefined symbols (a shared library's dynamic symbols, a
relocatable object's symbol table) and writes three C files for the host: exports of those symbols by name (group MULTIAPP, through asm labels, so
no header has to declare them), the log modules the extensions declare, and
the objects they describe in .multiapp_host (struct multiapp_host_object). A
symbol the host lacks fails the host's link. What a module defines comes from
that module (llext finds it among the loaded extensions), not from the host.
"""

import argparse
import re
import struct
import sys

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


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--exports", required=True, help="C file of exports to write")
    parser.add_argument("--logs", required=True, help="C file of log modules to write")
    parser.add_argument("--objects", required=True, help="C file of host objects to write")
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
