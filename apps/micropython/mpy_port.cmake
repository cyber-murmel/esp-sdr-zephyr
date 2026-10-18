# SPDX-License-Identifier: GPL-3.0-or-later
#
# MicroPython's Zephyr port, built into this application. Derived from
# modules/micropython/ports/zephyr/CMakeLists.txt (v1.29.0, MIT, Copyright 2020
# NXP): that file is meant to be the project's own CMakeLists.txt and takes
# its directory from CMAKE_CURRENT_SOURCE_DIR, so it cannot be include()d. The
# differences are the directories, the application's own manifest and
# configuration header, no USB stack of the port (app_usb.h owns the OTG
# port) and no RISC-V and dynamic pinctrl parts. Re-derive it when the pin in
# west.yml moves.

get_filename_component(MICROPY_DIR ${ZEPHYR_BASE}/../micropython ABSOLUTE)
set(MICROPY_PORT_DIR    ${MICROPY_DIR}/ports/zephyr)
set(MICROPY_TARGET      micropython)
string(TOUPPER          ZEPHYR_${BOARD} MICROPY_BOARD)

include(${MICROPY_DIR}/py/py.cmake)
include(${MICROPY_DIR}/extmod/extmod.cmake)

# Resolved before usermod.cmake (and thus py/manifest.cmake) runs, so that
# c_module() entries in the manifest are picked up.
if (CONFIG_MICROPY_FROZEN_MODULES)
    cmake_path(ABSOLUTE_PATH CONFIG_MICROPY_FROZEN_MANIFEST BASE_DIRECTORY ${CMAKE_CURRENT_LIST_DIR})
    set(MICROPY_FROZEN_MANIFEST ${CONFIG_MICROPY_FROZEN_MANIFEST})

    # The bundled apps (eggs/): their metadata.json files as the module the launcher lists
    # them from, eggs/__init__.py in $(EGGS_INDEX_DIR) for the manifest. Configuring again
    # when an app comes, goes or changes its metadata.
    set(MICROPY_MANIFEST_EGGS_INDEX_DIR ${CMAKE_CURRENT_BINARY_DIR}/eggs_index)
    file(GLOB egg_metadata CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/eggs/*/metadata.json)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${egg_metadata}
                 ${CMAKE_CURRENT_LIST_DIR}/tools/eggs_index.py)
    # CONFIG_APP_EGGS names the apps a board's manifest freezes when not all of them.
    set(eggs_only)
    if(CONFIG_APP_EGGS)
        set(eggs_only --only ${CONFIG_APP_EGGS})
    endif()
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/tools/eggs_index.py ${eggs_only}
                ${CMAKE_CURRENT_LIST_DIR}/eggs ${MICROPY_MANIFEST_EGGS_INDEX_DIR}/eggs/__init__.py
        RESULT_VARIABLE eggs_index_result)
    if (eggs_index_result)
        message(FATAL_ERROR "tools/eggs_index.py failed (${eggs_index_result})")
    endif()
endif()

# Zephyr's flags, for the qstr scan and (LVGL) the binding generator's preprocessing.
zephyr_get_include_directories_for_lang(C includes)
zephyr_get_system_include_directories_for_lang(C system_includes)
zephyr_get_compile_definitions_for_lang(C definitions)
zephyr_get_compile_options_for_lang(C options)

set(MICROPY_CPP_FLAGS_EXTRA ${includes} ${system_includes} ${definitions} ${options})

set(USER_C_MODULES ${CMAKE_CURRENT_LIST_DIR}/cmod)

# LVGL: the binding (lvgl module, lv_mp.c generated from Zephyr's LVGL and its lv_conf.h) and
# the ZDD module, as in the tube firmware (codeberg.org/tube/micropython 8cc63a8). LVGL
# allocates from the Python heap (CONFIG_LV_Z_MEM_POOL_MICROPYTHON) and its global state is a
# GC root, so the collector sees everything LVGL holds.
if(CONFIG_LVGL)
    set(LVGL_DIR ${ZEPHYR_LVGL_MODULE_DIR})
    set(LV_CFLAGS ${includes} ${system_includes} ${options})
    set(LV_CONF_PATH ${ZEPHYR_BASE}/modules/lvgl/include/lv_conf.h)
    set(MICROPY_MANIFEST_LV_BINDING_MICROPYTHON_DIR ${ZEPHYR_LV_BINDING_MICROPYTHON_MODULE_DIR})
    list(APPEND USER_C_MODULES ${ZEPHYR_LV_BINDING_MICROPYTHON_MODULE_DIR}/bindings.cmake)
endif()

include(${MICROPY_DIR}/py/usermod.cmake)

# LVGL itself is Zephyr's lvgl library; the binding's lvgl_interface target would compile it a
# second time into this one.
if(CONFIG_LVGL)
    list(FILTER MICROPY_SOURCE_USERMOD EXCLUDE REGEX "^${ZEPHYR_LVGL_MODULE_DIR}/src/")
endif()

set(MICROPY_SOURCE_PORT
    main.c
    help.c
    machine_i2c.c
    machine_spi.c
    machine_pin.c
    machine_timer.c
    modbluetooth_zephyr.c
    modsocket.c
    modzephyr.c
    modzsensor.c
    mphalport.c
    uart_core.c
    zephyr_device.c
    zephyr_storage.c
    zephyr_filesystem.c
    mpthreadport.c
)
list(TRANSFORM MICROPY_SOURCE_PORT PREPEND ${MICROPY_PORT_DIR}/)

# Fixes to zephyr_filesystem.c, built from a copy (configuring fails if the port's code no
# longer reads as expected). Its mount table, "{ &<every fstab entry> }", only compiles with a
# single entry; this app has two (FLASH, SD).
function(mpy_fix_replace var from to)
  string(FIND "${${var}}" "${from}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "zephyr_filesystem.c: '${from}' not found, re-derive the fix in mpy_port.cmake")
  endif()
  string(REPLACE "${from}" "${to}" fixed "${${var}}")
  set(${var} "${fixed}" PARENT_SCOPE)
endfunction()

set(ZFS_SRC ${MICROPY_PORT_DIR}/zephyr_filesystem.c)
set(ZFS_FIXED ${CMAKE_CURRENT_BINARY_DIR}/zephyr_filesystem.c)
file(READ ${ZFS_SRC} zfs)
mpy_fix_replace(zfs "#define FOREACH_FS(fs) FS_FSTAB_ENTRY(fs)"
                    "#define FOREACH_FS(fs) &FS_FSTAB_ENTRY(fs),")
mpy_fix_replace(zfs "&DT_FOREACH_STATUS_OKAY(zephyr_fstab, FOREACH_FSTAB),"
                    "DT_FOREACH_STATUS_OKAY(zephyr_fstab, FOREACH_FSTAB)")
# vfs.mount() of a volume FileSystem() has just mounted (every boot and soft reset) unmounts
# and mounts it again, and a FAT unmount powers the disk off: the SD card would be
# re-initialised each time, racing the USB host. Keep the mount instead.
mpy_fix_replace(zfs "    if (err == -EBUSY) {\n        err = fs_unmount(mount);"
                    "    if (err == -EBUSY) {\n        return mp_const_none;\n        err = fs_unmount(mount);")
set(zfs_old "")
if(EXISTS ${ZFS_FIXED})
  file(READ ${ZFS_FIXED} zfs_old)
endif()
if(NOT zfs STREQUAL zfs_old)
  file(WRITE ${ZFS_FIXED} "${zfs}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ZFS_SRC})
list(REMOVE_ITEM MICROPY_SOURCE_PORT ${ZFS_SRC})
list(APPEND MICROPY_SOURCE_PORT ${ZFS_FIXED})

if(CONFIG_LVGL)
    list(APPEND MICROPY_SOURCE_PORT
        ${ZEPHYR_LV_BINDING_MICROPYTHON_MODULE_DIR}/driver/zephyr/modlvzephyr.c
        ${CMAKE_CURRENT_LIST_DIR}/src/lv_mem_mp.c
    )
endif()

set(MICROPY_SOURCE_SHARED
    libc/printf.c
    readline/readline.c
    runtime/gchelper_generic.c
    runtime/interrupt_char.c
    runtime/mpirq.c
    runtime/pyexec.c
    runtime/stdout_helpers.c
    runtime/sys_stdio_mphal.c
    timeutils/timeutils.c
)
list(TRANSFORM MICROPY_SOURCE_SHARED PREPEND ${MICROPY_DIR}/shared/)

set(MICROPY_SOURCE_DRIVERS
    bus/softspi.c
)
list(TRANSFORM MICROPY_SOURCE_DRIVERS PREPEND ${MICROPY_DIR}/drivers/)

set(MICROPY_QSTRDEFS_PORT
    ${MICROPY_PORT_DIR}/qstrdefsport.h
)

set(MICROPY_SOURCE_QSTR
    ${MICROPY_SOURCE_PY}
    ${MICROPY_SOURCE_EXTMOD}
    ${MICROPY_SOURCE_USERMOD}
    ${MICROPY_SOURCE_SHARED}
    ${MICROPY_SOURCE_DRIVERS}
    ${MICROPY_SOURCE_PORT}
)

zephyr_library_named(${MICROPY_TARGET})

zephyr_library_include_directories(
    ${MICROPY_INC_CORE}
    ${MICROPY_PORT_DIR}
    ${MICROPY_INC_USERMOD}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/src
)

zephyr_library_compile_options(
    -std=gnu99 -fomit-frame-pointer
)

zephyr_library_compile_definitions(
    NDEBUG
    MP_CONFIGFILE=<${CONFIG_MICROPY_CONFIGFILE}>
    MICROPY_HEAP_SIZE=${CONFIG_MICROPY_HEAP_SIZE}
    FFCONF_H=\"${MICROPY_OOFATFS_DIR}/ffconf.h\"
    # The file systems are Zephyr's (FAT, ELM), not MicroPython's; modvfs.c still wants ff.h.
    MICROPY_VFS_FAT=0
    MICROPY_VFS_LFS1=0
    MICROPY_VFS_LFS2=0
)

zephyr_library_sources(${MICROPY_SOURCE_QSTR})
zephyr_library_link_libraries(kernel)

include(${MICROPY_DIR}/py/mkrules.cmake)

add_dependencies(BUILD_VERSION_HEADER zephyr_generated_headers)
add_dependencies(${MICROPY_TARGET} zephyr_generated_headers)

# The port's src/usbd.c and src/zephyr_start.c (main()) are left out: boot.c has the
# mp_usbd_init() it calls and a main() that runs MicroPython in its own thread. Without
# boot.c (MICROPY_APP_START, the ESP32-C5) the port's main() runs it in the main thread.
target_sources(app PRIVATE
    ${MICROPY_PORT_DIR}/src/zephyr_getchar.c
)
if(MICROPY_APP_START)
    target_sources(app PRIVATE ${MICROPY_PORT_DIR}/src/zephyr_start.c)
endif()

target_link_libraries(app PRIVATE ${MICROPY_TARGET})
