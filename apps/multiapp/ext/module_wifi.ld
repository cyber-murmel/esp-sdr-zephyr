/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Added to the default script of the wifi module, in place of module_blobs.ld. It places the blobs
 * as an image with the driver and its IRAM options off places them: their .wifi*iram code and
 * read-only data where such an image has flash, here PSRAM, and only .dram1 in internal RAM.
 *
 * The blobs' read-only data needs dynamic relocations, which the linker allows because the input
 * sections are writable (multiapp_blob()). The output section is read-only, so llext loads it with
 * the rest of the read-only data. Directly after .got.loc, so that its region stays contiguous.
 */
SECTIONS
{
	.wifi_text :
	{
		*(.wifi0iram .wifi0iram.* .wifiextrairam .wifiextrairam.*)
		*(.wifirxiram .wifirxiram.* .wifislpiram .wifislpiram.*)
		*(.wifislprxiram .wifislprxiram.* .wifiorslpiram .wifiorslpiram.*)
		*(.coexiram .coexiram.*)
	}
}
INSERT BEFORE .text;
SECTIONS
{
	.blob_ro (READONLY) : { *(.rodata .rodata.* .rodata_wlog_*) }
}
INSERT AFTER .got.loc;
SECTIONS
{
	.blob_dram : { *(.dram1 .dram1.*) }
}
INSERT AFTER .got;
