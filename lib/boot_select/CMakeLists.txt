# SPDX-License-Identifier: GPL-3.0-or-later

zephyr_library()
zephyr_library_sources(boot_select.c)
zephyr_library_link_libraries(MCUBOOT_BOOTUTIL)
