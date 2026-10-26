# SPDX-License-Identifier: GPL-3.0-or-later
#
# What this app changes in the build of the MicroPython module (modules/micropython/zephyr,
# which includes ports/zephyr/CMakeLists.txt), after find_package(Zephyr) configured it:
# the bundled apps' index for the frozen manifest, the USB device left to the app, and two
# sources of the micropython library replaced. Each replacement fails the configuration
# when the module's source is no longer there, so that a move of the pin in west.yml cannot
# drop a fix unnoticed.

set(MICROPY_PORT_DIR ${ZEPHYR_MICROPYTHON_MODULE_DIR}/ports/zephyr)

# The bundled apps (eggs/): their metadata.json files as the module the launcher lists them
# from, eggs/__init__.py in $(EGGS_INDEX_DIR) for the manifest (CMakeLists.txt). Configuring
# again when an app comes, goes or changes its metadata. CONFIG_APP_EGGS names the apps a
# board's manifest freezes when not all of them.
file(GLOB egg_metadata CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/eggs/*/metadata.json)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${egg_metadata}
             ${CMAKE_CURRENT_LIST_DIR}/tools/eggs_index.py)
set(eggs_only)
if(CONFIG_APP_EGGS)
  set(eggs_only --only ${CONFIG_APP_EGGS})
endif()
execute_process(
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/tools/eggs_index.py ${eggs_only}
          ${CMAKE_CURRENT_LIST_DIR}/eggs ${EGGS_INDEX}
  RESULT_VARIABLE eggs_index_result)
if(eggs_index_result)
  message(FATAL_ERROR "tools/eggs_index.py failed (${eggs_index_result})")
endif()

# The USB device is the app's (app_usb.h on the S3, cmod/esdr_usb.c on the P4), as is the
# mp_usbd_init() the port's main() calls at every soft reset: not the port's src/usbd.c.
get_target_property(app_sources app SOURCES)
list(REMOVE_ITEM app_sources ${MICROPY_PORT_DIR}/src/usbd.c)
set_property(TARGET app PROPERTY SOURCES ${app_sources})

# Replaces the micropython library's source old (a path) by new.
function(mpy_replace_source old new)
  get_target_property(sources micropython SOURCES)
  list(FIND sources ${old} at)
  if(at EQUAL -1)
    message(FATAL_ERROR "${old} is no longer in the micropython library: re-derive mpy_module.cmake")
  endif()
  list(REMOVE_AT sources ${at})
  list(APPEND sources ${new})
  set_property(TARGET micropython PROPERTY SOURCES ${sources})
endfunction()

# The port's zephyr_filesystem.c, built from a copy (configuring fails if the port's code no
# longer reads as expected): vfs.mount() of a volume FileSystem() has just mounted (every boot
# and soft reset) unmounts and mounts it again, and a FAT unmount powers the disk off: the SD
# card would be re-initialised each time, racing the USB host. Keep the mount instead.
set(zfs_src ${MICROPY_PORT_DIR}/zephyr_filesystem.c)
set(zfs_fixed ${CMAKE_CURRENT_BINARY_DIR}/zephyr_filesystem.c)
set(zfs_from "    if (err == -EBUSY) {\n        err = fs_unmount(mount);")
set(zfs_to "    if (err == -EBUSY) {\n        return mp_const_none;\n        err = fs_unmount(mount);")
file(READ ${zfs_src} zfs)
string(FIND "${zfs}" "${zfs_from}" at)
if(at EQUAL -1)
  message(FATAL_ERROR "zephyr_filesystem.c: the unmount on EBUSY not found, re-derive the fix in mpy_module.cmake")
endif()
string(REPLACE "${zfs_from}" "${zfs_to}" zfs "${zfs}")
set(zfs_old "")
if(EXISTS ${zfs_fixed})
  file(READ ${zfs_fixed} zfs_old)
endif()
if(NOT zfs STREQUAL zfs_old)
  file(WRITE ${zfs_fixed} "${zfs}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${zfs_src})
mpy_replace_source(${zfs_src} ${zfs_fixed})

# LVGL allocates from the Python heap (CONFIG_LV_Z_MEM_POOL_MICROPYTHON): src/lv_mem_mp.c
# instead of LVGL's lv_mem_core_micropython.c, so that a failed allocation returns NULL to
# LVGL instead of raising MemoryError through its C frames.
if(CONFIG_LVGL)
  mpy_replace_source(${ZEPHYR_LVGL_MODULE_DIR}/src/stdlib/micropython/lv_mem_core_micropython.c
                     ${CMAKE_CURRENT_LIST_DIR}/src/lv_mem_mp.c)
  # The binding wraps zassert_post_action(), which Zephyr defines only with CONFIG_ASSERT.
  if(NOT CONFIG_ASSERT)
    target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/lv_zassert.c)
  endif()
endif()
