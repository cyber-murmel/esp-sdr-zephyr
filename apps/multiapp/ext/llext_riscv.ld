/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The partial link (-r) of an extension on RISC-V, where llext loads relocatable objects. Each
 * output section groups one kind, as Zephyr's cmake/linker/llext_relocatable.ld does, so that each
 * llext region spans one stretch of the file; and:
 * - one .bss (with the .noinit sections, which are NOBITS too): llext takes a single NOBITS section (libphy alone has six; exports.py fails the build on a second);
 * - .module_fast: IRAM_ATTR code, which a module built INTERNAL runs from internal RAM
 *   (llext_load_param.text_alt_section, src/apps.c); in other extensions it is code like the rest;
 * - the kernel objects defined statically (K_SEM_DEFINE() and the like) with the data;
 * - no .eh_frame: its SET6/SUB6 relocations are not llext's, and nothing unwinds.
 * With --gc-sections (an extension with LIBS), what the extension compiles itself (its objects
 * below CMakeFiles/<name>_ext_llext_lib.dir) is kept, and of the archives only what it reaches:
 * their test code calls what the host does not have. The hal's sources it compiles (below a
 * components/ directory) count as archives: built with a section per function, they keep only
 * what is reached (phy_common.c's antenna helper calls gpio_config(), which the host lacks).
 */

SECTIONS
{
	.module_fast : {
		KEEP(EXCLUDE_FILE(*/components/*) *_ext_llext_lib.dir/*(.iram1 .iram1.* .phyiram .phyiram.*))
		*(.iram1 .iram1.* .phyiram .phyiram.*)
	}
	.text : {
		KEEP(EXCLUDE_FILE(*/components/*) *_ext_llext_lib.dir/*(.text .text.*))
		*(.text .text.*)
	}
	.rodata : {
		KEEP(EXCLUDE_FILE(*/components/*) *_ext_llext_lib.dir/*(.rodata .rodata.* .srodata .srodata.*))
		*(.rodata .rodata.* .srodata .srodata.*)
	}
	.data : {
		KEEP(EXCLUDE_FILE(*/components/*) *_ext_llext_lib.dir/*(.data .data.* .sdata .sdata.* .dram1 .dram1.* ._k_*))
		*(.data .data.* .sdata .sdata.* .dram1 .dram1.* ._k_*)
	}
	.bss : {
		KEEP(EXCLUDE_FILE(*/components/*) *_ext_llext_lib.dir/*(.bss .bss.* .sbss .sbss.* COMMON .noinit .noinit.*))
		*(.bss .bss.* .sbss .sbss.* COMMON .noinit .noinit.*)
	}
	.preinit_array : { KEEP(*(.preinit_array .preinit_array.*)) }
	.init_array    : { KEEP(*(.init_array .init_array.*)) }
	.fini_array    : { KEEP(*(.fini_array .fini_array.*)) }
	.exported_sym  : { KEEP(*(.exported_sym)) }
	/DISCARD/      : { *(.eh_frame) }
}
