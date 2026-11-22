# SPDX-License-Identifier: GPL-3.0-or-later
#
# apps/micropython as an app of this host (doc/apps/multiapp-internals.md): the port, its C modules
# and frozen Python, built as one extension. On the ESP32-S31 (doc/dev/network-services.md, "The
# MicroPython REPL over SSH") the same, as a RISC-V relocatable object, without the S3's own USB disks,
# its Xtensa assembly (esp-dsp's FFT, the Scan app's vector passes) and the display. The host stays CONFIG_MICROPYTHON=n, so the module's
# own build does not run; this builds the same sources with the same flags
# (ports/zephyr/CMakeLists.txt) and the rules of MicroPython's py/mkrules.cmake into the extension's
# target. ext/mp/ has what differs.

set(mp_app ${apps_dir}/micropython)
set(mp_ext ${CMAKE_CURRENT_LIST_DIR}/ext/mp)

set(MICROPY_TARGET micropython_ext_llext_lib)
set(MICROPY_DIR ${ZEPHYR_MICROPYTHON_MODULE_DIR})
set(MICROPY_PORT_DIR ${MICROPY_DIR}/ports/zephyr)

include(${MICROPY_DIR}/py/py.cmake)
include(${MICROPY_DIR}/extmod/extmod.cmake)

set(USER_C_MODULES ${mp_app}/cmod)
set(mp_kconfig ${mp_ext}/micropython.conf)
if(CONFIG_SOC_SERIES_ESP32S31)
  # The REPL's stream hook (ext/mp/mp_repl_s31.c) and what the chip's esdr module (cmod/esdr_local.c) needs.
  list(APPEND mp_kconfig ${mp_ext}/micropython_s31.conf)
endif()
set(mp_options)
set(mp_include_dirs)
set(mp_definitions)
if(CONFIG_MULTIAPP_MICROPYTHON_GUI)
  # As the port's CMakeLists.txt does for CONFIG_LVGL, which only this extension's Kconfig pass has.
  set(CONFIG_LVGL y)
  set(MICROPY_FROZEN_MANIFEST ${mp_app}/manifest.py)
  set(LVGL_DIR ${ZEPHYR_LVGL_MODULE_DIR})
  set(LV_INCLUDE ${ZEPHYR_BASE}/modules/lvgl/include/)
  set(LV_CONF_PATH ${LV_INCLUDE}/lv_conf.h)
  set(mp_lv_dirs ${LV_INCLUDE} ${LVGL_DIR} ${LVGL_DIR}/src)
  set(zephyr_includes $<TARGET_PROPERTY:zephyr_interface,INTERFACE_INCLUDE_DIRECTORIES>)
  set(zephyr_system_includes $<TARGET_PROPERTY:zephyr_interface,INTERFACE_SYSTEM_INCLUDE_DIRECTORIES>)
  zephyr_get_compile_options_for_lang(C zephyr_options)
  set(LV_CFLAGS
    "-I$<JOIN:${zephyr_includes},$<SEMICOLON>-I>"
    "$<$<BOOL:${zephyr_system_includes}>:-isystem$<JOIN:${zephyr_system_includes},$<SEMICOLON>-isystem>>"
    ${zephyr_options}
    -I${LV_INCLUDE} -I${LVGL_DIR}/src
    -include ${ext_dir}/micropython_kconfig.h
    "-DLV_CONF_INCLUDE_SIMPLE=1"
    "-DLV_CONF_PATH=\"${LV_CONF_PATH}\""
    "-D__attribute__(x)="
  )
  set(MICROPY_MANIFEST_LV_BINDING_MICROPYTHON_DIR ${ZEPHYR_LV_BINDING_MICROPYTHON_MODULE_DIR})
  list(APPEND USER_C_MODULES "${ZEPHYR_LV_BINDING_MICROPYTHON_MODULE_DIR}/bindings.cmake")

  list(APPEND mp_kconfig ${mp_ext}/gui.conf)
  dt_nodelabel(mp_sd_fs NODELABEL sd_fs)
  if(mp_sd_fs)
    list(APPEND mp_kconfig ${mp_ext}/sd.conf)
  endif()
  # The board's values for the standalone app, whose name for the board is the same.
  string(REGEX REPLACE ".*/" "" mp_cpu "${BOARD_QUALIFIERS}")
  string(REGEX REPLACE "/.*" "" mp_board "${BOARD}")
  if(NOT EXISTS ${mp_app}/boards/${mp_board}_${mp_cpu}.conf)
    message(FATAL_ERROR "no apps/micropython/boards/${mp_board}_${mp_cpu}.conf for a display")
  endif()
  list(APPEND mp_kconfig ${mp_app}/boards/${mp_board}_${mp_cpu}.conf)
  list(APPEND mp_options DEVICES)
  list(APPEND mp_include_dirs ${mp_lv_dirs} ${ZEPHYR_LV_BINDING_MICROPYTHON_MODULE_DIR})
  list(APPEND mp_definitions LV_CONF_INCLUDE_SIMPLE=1 LV_CONF_PATH=\"${LV_CONF_PATH}\")
else()
  set(CONFIG_APP_EGGS datalink)
  set(MICROPY_FROZEN_MANIFEST ${mp_app}/manifest_headless.py)
endif()
include(${MICROPY_PORT_DIR}/sources.cmake)
set(MICROPY_MANIFEST_EGGS_INDEX_DIR ${CMAKE_BINARY_DIR}/eggs_index)
set(EGGS_INDEX ${MICROPY_MANIFEST_EGGS_INDEX_DIR}/eggs/__init__.py)
if(NOT EXISTS ${EGGS_INDEX})
  file(WRITE ${EGGS_INDEX} "INDEX = {}\n")
endif()
include(${MICROPY_DIR}/py/usermod.cmake)
if(CONFIG_MULTIAPP_MICROPYTHON_GUI)
  # The binding brings LVGL's sources through lvgl_interface: this extension builds them below.
  list(FILTER MICROPY_SOURCE_USERMOD EXCLUDE REGEX "^${ZEPHYR_LVGL_MODULE_DIR}/src/")
endif()

set(MICROPY_SOURCE_QSTR
  ${MICROPY_SOURCE_PY}
  ${MICROPY_SOURCE_EXTMOD}
  ${MICROPY_SOURCE_USERMOD}
  ${MICROPY_SOURCE_SHARED}
  ${MICROPY_SOURCE_DRIVERS}
  ${MICROPY_SOURCE_LIB}
  ${MICROPY_SOURCE_PORT}
)

# Zephyr's lv_conf.h ahead of the binding's: Zephyr's glue includes it by name.
set(mp_includes
  ${mp_include_dirs}
  ${MICROPY_INC_CORE}
  ${MICROPY_PORT_DIR}
  ${MICROPY_INC_USERMOD}
  ${CMAKE_BINARY_DIR}
  ${mp_app}/src
  ${mp_ext}
)
set(mp_defines
  NDEBUG
  MP_CONFIGFILE=<mpconfigport_multiapp.h>
  MICROPY_HEAP_SIZE=512
  FFCONF_H=\"${MICROPY_OOFATFS_DIR}/ffconf.h\"
  MICROPY_VFS_FAT=0
  MICROPY_VFS_LFS1=0
  MICROPY_VFS_LFS2=0
  ${mp_definitions}
)

# What differs by chip: the S3's own USB disks and boot thread (boot.c, usb_disks.c), Xtensa assembly and
# its setjmp; the S31's thread, REPL stream and radio glue (ext/mp/mp_repl_s31.c, cmod/esdr_local.c).
set(mp_chip_sources)
set(mp_chip_options)
if(CONFIG_SOC_SERIES_ESP32S3)
  # The Scan app's FFT kernel and frame passes (cmod/esdr_scan_pie.c): assembly, so beside the C
  # modules, whose sources MicroPython's qstr pass reads.
  include(${apps_dir}/common/esp_dsp.cmake)
  esp_dsp_s3_fft(mp_esp_dsp)
  list(APPEND mp_chip_sources
    ${mp_app}/src/boot.c
    ${mp_app}/src/setjmp.S
    ${mp_app}/src/usb_disks.c
    ${mp_esp_dsp}
    ${mp_app}/cmod/esdr_scan_vec.S
    # esdr.shell() on a dummy backend of its own (the S31's goes to the host's shell: mp_repl_s31.c)
    ${ZEPHYR_BASE}/subsys/shell/backends/shell_dummy.c)
  list(APPEND mp_chip_options AREA CONSOLE USB 12 VERSION_SCRIPT ${mp_ext}/micropython.ver WRAP setjmp)
elseif(CONFIG_SOC_SERIES_ESP32S31)
  # Its own thread (the REPL needs more stack than the app's), the console mux's REPL, the local radio
  # (bursts through the sdr module; the C modules' list is cmod/micropython.cmake's) and the crystal
  # calibration requests it answers (the host has esdr_cal.c only inside osmosdr_hs: this is its own copy).
  # No esp-dsp FFT: the Scan app's spectra use the portable code (cmod/esdr_scan.c) here.
  list(APPEND mp_chip_sources
    ${mp_ext}/mp_repl_s31.c
    ${mp_app}/cmod/esdr_local.c
    ${mp_app}/cmod/esdr_mon.c
    ${apps_dir}/common/src/esdr_cal.c)
  list(APPEND mp_includes ${apps_dir}/osmosdr/src)
  # The host's USB device has the drive; this app brings no disks (profile 0x4, MSC): a console app.
  # WRAP: what reaches the flash is refused or sent to the worker (mp_repl_s31.c)
  list(APPEND mp_chip_options CONSOLE USB 0
       WRAP flash_area_read flash_area_write flash_area_erase disk_access_read disk_access_write sys_reboot)
else()
  message(FATAL_ERROR "multiapp: MicroPython is built for the ESP32-S3 and the ESP32-S31 only")
endif()

# Its stack is one the area holds, as the flash needs (src/boot.c): main() only starts that thread.
# The S3's main() only starts the thread (STACK 1536); the S31's, too (mp_repl_s31.c).
# USB 12: MULTIAPP_USB_MSC | MULTIAPP_USB_OWN_DISKS.
multiapp_extension(micropython APP ${mp_app} APP_CONF ${mp_ext}/app.conf STACK 1536 FILE_ONLY
  REQUIRES sdr ${mp_options} ${mp_chip_options}
  KCONFIG ${mp_kconfig}
  KCONFIG_OWN "MICROPY.*" "SENSOR.*" "CONSOLE_HANDLER" "UART_CONSOLE_DEBUG_SERVER_HOOKS"
              "SHELL_BACKEND_DUMMY" "SHELL_PROMPT_DUMMY" "SHELL_BACKEND_DUMMY_.*" "SHELL_DUMMY_.*"
              "WARN_DEPRECATED"
              # LVGL and the board's drivers, which the host has none of.
              "APP_LINK_WITH_LVGL" "LVGL.*" "LV_.*" "DISPLAY" "DISPLAY_.*" "ST7789V" "ST7789V_.*"
              "NV3007" "NV3007_.*"
              "MIPI_DBI" "MIPI_DBI_.*" "INPUT" "INPUT_.*" "SPI" "SPI_INIT_PRIORITY" "SPI_LOG_.*"
              "SPI_COMPLETION_TIMEOUT_TOLERANCE" "SPI_SDHC" "SPI_ASYNC" "ESP32_SPIM" "SPI_ESP32_.*"
              "I2C" "I2C_.*"
              "SDHC" "SDHC_.*" "SD_.*" "SDMMC_.*" "MMC_.*" "DISK_DRIVER_SDMMC"
  # Sensor drivers' settings, and no driver of them is built. The kernel heap and the early boot
  # log level are the host's, whatever the board's fragment sets for the standalone app.
  KCONFIG_DROP "ADE7978_.*" "HEAP_MEM_POOL_SIZE" "ESP_HAL_EARLY_LOG_LEVEL"
  PRELUDE ${mp_ext}/mp_prelude.h
  INCLUDES ${mp_includes}
  DEFINES ${mp_defines}
  SOURCES ${MICROPY_SOURCE_QSTR}
          ${MICROPY_PORT_DIR}/src/zephyr_getchar.c
          ${mp_ext}/mp_glue.c
          ${mp_chip_sources})
llext_compile_options(micropython_ext -std=gnu99 -fomit-frame-pointer)
if(CONFIG_SOC_SERIES_ESP32S31)
  # After the link: the extension's thread stacks are in PSRAM, so only ones that never touch the flash may be
  # there (MicroPython's own: its flash accesses run on the worker, ext/mp/mp_repl_s31.c); the libc arena holds the
  # worker's stack (taken at the first esdr.shell()) and what else takes the arena after MicroPython started
  # (measured: CONFIG_MULTIAPP_MICROPYTHON_ARENA_USE).
  set(mp_stack ${CONFIG_MULTIAPP_MICROPYTHON_WORKER_STACK})
  set(mp_stack_note "the flash worker's, CONFIG_MULTIAPP_MICROPYTHON_WORKER_STACK")
  if(CONFIG_MULTIAPP_MICROPYTHON_ARENA_USE GREATER 0)
    set(mp_others ${CONFIG_MULTIAPP_MICROPYTHON_ARENA_USE})
    set(mp_others_note "measured")
  else()
    set(mp_others 8192)
    set(mp_others_note "assumed, not measured")
  endif()
  math(EXPR mp_arena_min "${mp_stack} + ${mp_others}")
  add_custom_target(micropython_placement ALL
    COMMAND ${CMAKE_COMMAND} -E env READELF=${CMAKE_READELF}
            ${PYTHON_EXECUTABLE} ${apps_dir}/../scripts/tcm_groups.py
            --mp-ext ${ext_dir}/micropython.llext --arena-min ${mp_arena_min}
            --arena-note "worker stack ${mp_stack} B (${mp_stack_note}) + the rest of the arena ${mp_others} B (${mp_others_note})"
            ${PROJECT_BINARY_DIR}/zephyr/${KERNEL_ELF_NAME}
    COMMENT "MicroPython placement check"
    VERBATIM)
  add_dependencies(micropython_placement ${logical_target_for_zephyr_elf} micropython_ext)
endif()
if(CONFIG_MULTIAPP_MICROPYTHON_GUI)
  # As CONFIG_WARN_DEPRECATED=n gives the standalone build: the binding wraps LVGL's deprecated functions too.
  llext_compile_options(micropython_ext -Wno-deprecated-declarations)
  # LVGL's sources, and which of Zephyr's input glue, follow this extension's Kconfig pass.
  file(STRINGS ${ext_dir}/micropython_kconfig/.config mp_lv_config REGEX "^CONFIG_LV_Z_[A-Z0-9_]+=y$")
  foreach(line ${mp_lv_config})
    string(REPLACE "=y" "" option "${line}")
    set(${option} y)
  endforeach()
  include(${ZEPHYR_BASE}/modules/lvgl/sources.cmake)
  # Not used here, and its own ssize_t clashes with the one the prelude's headers bring.
  list(FILTER LVGL_SRC_FILES EXCLUDE REGEX "/libs/frogfs/")
  target_sources(${MICROPY_TARGET} PRIVATE ${LVGL_SRC_FILES} ${LVGL_GLUE_SOURCES})
  set_source_files_properties(${LVGL_SRC_FILES} ${LVGL_GLUE_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS _POSIX_C_SOURCE=200809L)
  set(mp_sections_ld ${mp_ext}/mp_sections.ld)
  llext_link_options(micropython_ext -Wl,-T,${mp_sections_ld})
  set_property(TARGET ${MICROPY_TARGET} APPEND PROPERTY LINK_DEPENDS ${mp_sections_ld})
  # The board's drivers, which the host has no use for, follow the pass too: a device whose driver is
  # missing here fails the link. The list comes from the difference between a standalone build's
  # compile commands and the host's.
  file(STRINGS ${ext_dir}/micropython_kconfig/.config mp_config REGEX "^CONFIG_[A-Z0-9_]+=y$")
  set(mp_hal ${ZEPHYR_HAL_ESPRESSIF_MODULE_DIR}/components)
  function(mp_driver option)
    if("CONFIG_${option}=y" IN_LIST mp_config)
      target_sources(${MICROPY_TARGET} PRIVATE ${ARGN})
    endif()
  endfunction()
  mp_driver(ESP32_SPIM ${ZEPHYR_BASE}/drivers/spi/spi_esp32_spim.c
    ${mp_hal}/esp_hal_gpspi/esp32s3/spi_periph.c
    ${mp_hal}/esp_hal_gpspi/spi_hal.c
    ${mp_hal}/esp_hal_gpspi/spi_hal_iram.c)
  mp_driver(I2C_ESP32 ${ZEPHYR_BASE}/drivers/i2c/i2c_common.c ${ZEPHYR_BASE}/drivers/i2c/i2c_esp32.c
    ${mp_hal}/esp_hal_i2c/i2c_hal.c
    ${mp_hal}/esp_hal_i2c/i2c_hal_iram.c)
  mp_driver(MIPI_DBI_SPI ${ZEPHYR_BASE}/drivers/mipi_dbi/mipi_dbi_spi.c)
  mp_driver(ST7789V ${ZEPHYR_BASE}/drivers/display/display_st7789v.c)
  mp_driver(NV3007 ${ZEPHYR_BASE}/drivers/display/display_nv3007.c)
  mp_driver(INPUT ${ZEPHYR_BASE}/subsys/input/input.c ${ZEPHYR_BASE}/subsys/input/input_utils.c)
  mp_driver(INPUT_KEYMAP ${ZEPHYR_BASE}/subsys/input/input_keymap.c)
  mp_driver(INPUT_GPIO_KEYS ${ZEPHYR_BASE}/drivers/input/input_gpio_keys.c)
  mp_driver(INPUT_GPIO_QDEC ${ZEPHYR_BASE}/drivers/input/input_gpio_qdec.c)
  mp_driver(INPUT_TCA8418 ${ZEPHYR_BASE}/drivers/input/input_tca8418.c)
  mp_driver(DISK_DRIVER_SDMMC ${ZEPHYR_BASE}/drivers/disk/sdmmc_subsys.c
    ${ZEPHYR_BASE}/drivers/sdhc/sdhc_spi.c
    ${ZEPHYR_BASE}/subsys/sd/sd.c
    ${ZEPHYR_BASE}/subsys/sd/sd_ops.c
    ${ZEPHYR_BASE}/subsys/sd/sdmmc.c)
endif()
# boot.c runs the port's main() in a thread of its own.
set_source_files_properties(${MICROPY_PORT_DIR}/main.c PROPERTIES COMPILE_DEFINITIONS main=real_main)
# Zephyr's ff.h, not the one the port's sources see: usb_disks.c refreshes a FatFs volume without
# remounting it.
set_source_files_properties(${mp_app}/src/usb_disks.c PROPERTIES
  INCLUDE_DIRECTORIES "$<TARGET_PROPERTY:ELMFAT,INTERFACE_INCLUDE_DIRECTORIES>"
  COMPILE_DEFINITIONS "$<TARGET_PROPERTY:ELMFAT,INTERFACE_COMPILE_DEFINITIONS>")

# The qstr scan preprocesses every source as it is compiled. mkrules.cmake reads the target's flags
# as plain lists, which an llext target's generator expressions are not.
set(zephyr_includes $<TARGET_PROPERTY:zephyr_interface,INTERFACE_INCLUDE_DIRECTORIES>)
set(zephyr_system_includes $<TARGET_PROPERTY:zephyr_interface,INTERFACE_SYSTEM_INCLUDE_DIRECTORIES>)
set(zephyr_definitions $<TARGET_PROPERTY:zephyr_interface,INTERFACE_COMPILE_DEFINITIONS>)
zephyr_get_compile_options_for_lang(C zephyr_options)
set(MICROPY_CPP_FLAGS
  "-I$<JOIN:${zephyr_includes},$<SEMICOLON>-I>"
  "$<$<BOOL:${zephyr_system_includes}>:-isystem$<JOIN:${zephyr_system_includes},$<SEMICOLON>-isystem>>"
  "-D$<JOIN:${zephyr_definitions},$<SEMICOLON>-D>"
  ${zephyr_options}
  -I${CMAKE_CURRENT_LIST_DIR}/include
  -I${apps_dir}/common/include
  -include ${ext_dir}/micropython_kconfig.h
  -include ${CMAKE_CURRENT_LIST_DIR}/include/multiapp/ext.h
  -include ${mp_ext}/mp_prelude.h
  -include ${ext_dir}/micropython_config.h
  -DLL_EXTENSION_BUILD
  -DMULTIAPP_NAME=micropython
  -DMICROPY_QSTR_EXTRA_POOL=mp_qstr_frozen_const_pool
  -DMICROPY_MODULE_FROZEN_MPY=\(1\)
)
foreach(dir ${mp_includes})
  list(APPEND MICROPY_CPP_FLAGS -I${dir})
endforeach()
foreach(def ${mp_defines})
  list(APPEND MICROPY_CPP_FLAGS -D${def})
endforeach()
include(${MICROPY_DIR}/py/mkrules.cmake)
add_dependencies(BUILD_VERSION_HEADER zephyr_generated_headers)

# What apps/micropython changes in the port's build: the same, on this target.
set(MPY_TARGET ${MICROPY_TARGET})
include(${mp_app}/mpy_module.cmake)
