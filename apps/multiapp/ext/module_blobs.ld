/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Added to the default script of a module linked with blobs. Their .rodata* is writable
 * (multiapp_blob()), so it must not stay in front of .got.loc, which is read-only: llext merges
 * sections by their flags into regions, and the data region would enclose .got.loc.
 */
SECTIONS
{
	.blob_data : { *(.rodata .rodata.*) }
}
INSERT AFTER .got;
