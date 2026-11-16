/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Added to the default script of a module that runs partly from internal RAM: its IRAM_ATTR
 * code as one section the host loads apart
 * from the rest (llext_load_param.text_alt_section), as a standalone image's IRAM and flash.
 */
SECTIONS
{
	.module_fast : { *(.iram1 .iram1.* .phyiram .phyiram.*) }
}
INSERT AFTER .text;
