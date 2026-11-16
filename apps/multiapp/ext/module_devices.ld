/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Added to the default script of a module with devices. A device's init entry cannot join the
 * host's sections, so the module keeps its entries in the kernel's order (linker-defs.h,
 * CREATE_OBJ_LEVEL) and the host walks them as it loads the module (src/apps.c).
 */
SECTIONS
{
	.multiapp_init : ALIGN(4)
	{
		multiapp_init_start = .;
		KEEP(*(SORT(.z_init_PRE_KERNEL_1_P_?_*)));
		KEEP(*(SORT(.z_init_PRE_KERNEL_1_P_??_*)));
		KEEP(*(SORT(.z_init_PRE_KERNEL_1_P_???_*)));
		KEEP(*(SORT(.z_init_PRE_KERNEL_2_P_?_*)));
		KEEP(*(SORT(.z_init_PRE_KERNEL_2_P_??_*)));
		KEEP(*(SORT(.z_init_PRE_KERNEL_2_P_???_*)));
		KEEP(*(SORT(.z_init_POST_KERNEL_P_?_*)));
		KEEP(*(SORT(.z_init_POST_KERNEL_P_??_*)));
		KEEP(*(SORT(.z_init_POST_KERNEL_P_???_*)));
		KEEP(*(SORT(.z_init_APPLICATION_P_?_*)));
		KEEP(*(SORT(.z_init_APPLICATION_P_??_*)));
		KEEP(*(SORT(.z_init_APPLICATION_P_???_*)));
		multiapp_init_end = .;
	}
}
INSERT AFTER .got;
