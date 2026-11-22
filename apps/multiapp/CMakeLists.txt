# SPDX-License-Identifier: GPL-3.0-or-later
#
# multiapp (doc/apps/multiapp-internals.md): the other apps, built from their own sources as
# llext extensions, run on one host image.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_sdr_multiapp)

include(${CMAKE_CURRENT_LIST_DIR}/../common/common.cmake)

target_include_directories(app PRIVATE include src)
target_sources(app PRIVATE src/main.c src/apps.c src/drive.c src/runguard.c)
target_sources_ifdef(CONFIG_MULTIAPP_STAGES app PRIVATE src/stage.c src/stage_shell.c)
# The host's placement rules and its apps' (place.h, esp_sdr_place.h); each extension's scope is
# its name.
target_compile_definitions(app PRIVATE ESP_SDR_PLACE_SCOPE=multiapp)

# Zephyr drops -Os from Xtensa extensions (cmake/compiler/gcc/target_xtensa.cmake);
# unoptimized, they need several times the stack and code.
list(REMOVE_ITEM LLEXT_REMOVE_FLAGS -Os)

set(apps_dir ${CMAKE_CURRENT_LIST_DIR}/..)
set(ext_dir ${PROJECT_BINARY_DIR}/llext)
set(ext_names)
set(ext_modules)
set(ext_libs)
set(ext_files)
# The hot-swap kernels (multiapp_kernel()): files only, in none of the lists above that make the
# host's exports, build id and embedded table.
set(kernel_names)
# What the host exports, one name per line (exports.py --names): the kernels' imports are checked
# against it, and against their app's symbols.
set(host_names ${ext_dir}/multiapp_exports.txt)
# The build id every app file carries (buildid.py, generated below).
set(build_id_bin ${ext_dir}/multiapp_build_id.bin)
set(build_id_h ${ext_dir}/multiapp_build_id.h)

# A kernel's file: checked against what the host exports and its app defines (exports.py
# check-kernel), then the extension with its trailer (apptrail.py --kernel). A macro of
# multiapp_extension(): it sees that function's variables and sets the parent scope's lists.
macro(multiapp_kernel_file name file)
  if(NOT TARGET ${EXT_KERNEL_APP}_ext_llext_lib)
    message(FATAL_ERROR "multiapp: kernel ${name}: no app ${EXT_KERNEL_APP} (APP takes the name "
                        "of a multiapp_extension(), declared before the kernel)")
  endif()
  if(EXT_INTERNAL)
    set(kernel_place internal)
  else()
    set(kernel_place psram)
  endif()
  add_custom_command(
    OUTPUT ${file}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ext_dir}/files
    COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/exports.py check-kernel
            --kernel $<TARGET_FILE:${name}_ext_llext_lib>
            --app $<TARGET_FILE:${EXT_KERNEL_APP}_ext_llext_lib> --names ${host_names}
            --kernel-name "kernel ${name}" --app-name "app ${EXT_KERNEL_APP}"
    COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/apptrail.py
            --kernel ${kernel_place} --build-id ${build_id_bin} ${ext_dir}/${name}.llext ${file}
    DEPENDS ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/apptrail.py
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/exports.py ${ext_dir}/${name}.llext
            ${ext_dir}/${EXT_KERNEL_APP}.llext ${host_names} ${build_id_bin}
    VERBATIM
  )
  add_custom_target(${name}_kernel ALL DEPENDS ${file})
  set(kernel_names ${kernel_names} ${name} PARENT_SCOPE)
  if(CONFIG_MULTIAPP_KERNELS_IN_VOLUME)
    set(ext_files ${ext_files} ${file} PARENT_SCOPE)
  endif()
endmacro()

# multiapp_kernel(<name> STAGE <stage> APP <app> SOURCES <files> [INTERNAL] [INCLUDES <dirs>]
#                 [DEFINES <definitions>])
#
# A hot-swap kernel (include/multiapp/stage.h): one small extension that replaces the stage <stage>
# of the running app <app> (the name of its multiapp_extension()), loaded into it by "multiapp stage"
# without a reboot. Its source defines the stage's function and exports it with
# MULTIAPP_KERNEL_DEFINE(). Output: llext/files/<name>.llext (build target <name>_kernel), with a
# trailer that carries the host's build id and a CRC like an app's, and its placement. INTERNAL: its
# IRAM-placed code, data and bss in the module region heap (internal RAM) as an INTERNAL module's,
# the rest in PSRAM.
#
# A kernel is never embedded and joins no list the host's build id comes from: its imports are
# checked at build time to be what the host exports already or what the app defines (exports.py
# check-kernel), so that changing a kernel never invalidates a file. It is in the FAT volume image
# only with CONFIG_MULTIAPP_KERNELS_IN_VOLUME.
function(multiapp_kernel name)
  cmake_parse_arguments(KERN "INTERNAL" "STAGE;APP" "SOURCES;INCLUDES;DEFINES" ${ARGN})
  if(NOT KERN_STAGE OR NOT KERN_APP OR NOT KERN_SOURCES)
    message(FATAL_ERROR "multiapp_kernel(${name}): STAGE, APP and SOURCES are needed")
  endif()
  if(NOT CONFIG_MULTIAPP_STAGES)
    return()
  endif()
  if(NOT DEFINED stage_abi_${KERN_STAGE})
    message(FATAL_ERROR "multiapp_kernel(${name}): multiapp_stage_abi(${KERN_STAGE} HEADERS ...) "
                        "comes first, and the app is built with its hash too")
  endif()
  set(internal)
  if(KERN_INTERNAL)
    set(internal INTERNAL)
  endif()
  multiapp_extension(${name} KERNEL STAGE ${KERN_STAGE} KERNEL_APP ${KERN_APP} ${internal}
                     INCLUDES ${KERN_INCLUDES}
                     DEFINES ${KERN_DEFINES} MULTIAPP_STAGE_ABI_${KERN_STAGE}=${stage_abi_${KERN_STAGE}}
                     SOURCES ${KERN_SOURCES})
  set(kernel_names ${kernel_names} PARENT_SCOPE)
  set(ext_files ${ext_files} PARENT_SCOPE)
endfunction()

# multiapp_stage_abi(<stage> HEADERS <files>)
#
# The ABI hash of a stage (include/multiapp/stage.h): what the app and its kernels share besides the
# function's type, the headers that declare it (a state struct, a global). Defines
# stage_abi_<stage>, the hash of the headers' contents, which goes into the app's build and the
# kernels' as MULTIAPP_STAGE_ABI_<stage>, into the stage's object and the kernel's descriptor: the host
# refuses a kernel whose hash is not the app's. The headers are configure dependencies.
function(multiapp_stage_abi stage)
  cmake_parse_arguments(ABI "" "" "HEADERS" ${ARGN})
  set(text "${stage}")
  foreach(header ${ABI_HEADERS})
    file(SHA256 ${header} hash)
    string(APPEND text ":${hash}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${header})
  endforeach()
  string(SHA256 text_hash "${text}")
  string(SUBSTRING ${text_hash} 0 8 short)
  set(stage_abi_${stage} 0x${short}U PARENT_SCOPE)
endfunction()

# multiapp_extension(<name> SOURCES <files> [APP <dir> [APP_CONF <fragment>]] [STACK <bytes>]
#                    [USB <MULTIAPP_USB_* mask>] [AREA] [CONSOLE] [BANK2] [REQUIRES <modules>])
#
# The extension <name>, include/multiapp/ext.h in front of each source. APP:
# the app's directory, for its CONFIG_APP_* values (appconfig.py), APP_CONF on
# top of its own. AREA: it has buffers in the area (ESP_SDR_HIGH_BUF_DEFINE()).
# CONSOLE: it takes the CDC-ACM port from the shell for the boot, and its main()
# starts with USB up. BANK2: the boot gets SRAM dump bank 2 as a second internal heap, which a
# module that does not fit in module_heap loads into; the SDR then never uses that bank (rings
# on banks 0 and 1, no bursts or transmit). REQUIRES: the modules the host loads before it.
#
# multiapp_module(<name> SOURCES <files> [INTERNAL [HIGH_BSS]] [DEVICES]
#                 [REQUIRES <modules>] [LIBS <archives> [BLOBS_LD <script>]] [VERSION_SCRIPT <file>]
#                 [INCLUDES <dirs>] [DEFINES <definitions>] [FILE_ONLY] [PRELUDE <header>]
#                 [WRAP <symbols>]
#                 [KCONFIG <fragment> KCONFIG_OWN <regexes> [KCONFIG_DROP <regexes>]
#                  [KCONFIG_AFTER <header>]])
#
# Code and data other extensions call, loaded before them. It has no main().
# INTERNAL: placed as in a standalone image, its IRAM_ATTR code, data and bss in
# internal RAM, the rest of its code and its read-only data in PSRAM, where a
# standalone image has flash. HIGH_BSS: its bss in the high region, not below
# bank 0. DEVICES: it defines Zephyr devices, which the host initializes as it
# loads the module. LIBS: archives linked whole, placed by BLOBS_LD (default
# ext/module_blobs.ld). VERSION_SCRIPT: what it exports;
# the rest the linker drops. FILE_ONLY: too big for a built-in copy, it runs from its
# file or not at all. PRELUDE: a header in front of its sources, after ext.h. WRAP: its
# calls to these go to __wrap_<symbol>. KCONFIG: a second Kconfig pass with this fragment
# on top of the host's, whose differences it compiles with (extconfig.py); KCONFIG_AFTER
# is a header right after those values.
function(multiapp_extension name)
  cmake_parse_arguments(EXT "AREA;MODULE;INTERNAL;HIGH_BSS;DEVICES;FILE_ONLY;CONSOLE;BANK2;KERNEL"
                        "APP;APP_CONF;STACK;USB;VERSION_SCRIPT;PRELUDE;KCONFIG_AFTER;BLOBS_LD;STAGE;KERNEL_APP"
                        "SOURCES;REQUIRES;LIBS;INCLUDES;DEFINES;WRAP;KCONFIG;KCONFIG_OWN;KCONFIG_DROP"
                        ${ARGN})
  if(NOT EXT_STACK)
    set(EXT_STACK 2048)
  endif()
  if(NOT EXT_USB)
    set(EXT_USB 0)
  endif()
  set(info)
  if(NOT EXT_MODULE AND NOT EXT_KERNEL)
    set(info ${ext_dir}/${name}_info.c)
    file(WRITE ${info}.tmp
      "/* Generated by apps/multiapp/CMakeLists.txt. */\n"
      "#include <zephyr/llext/symbol.h>\n"
      "#include <multiapp/multiapp.h>\n\n"
      "int main(void);\n\n"
      "const struct multiapp_info multiapp_info = {\n"
      "\t.abi = MULTIAPP_ABI,\n"
      "\t.name = \"${name}\",\n"
      "\t.main = main,\n"
      "};\n"
      "EXPORT_SYMBOL(multiapp_info);\n")
    configure_file(${info}.tmp ${info} COPYONLY)
  endif()

  if(EXT_VERSION_SCRIPT)
    # A section per function, for the linker to drop what the exports do not reach.
    list(REMOVE_ITEM LLEXT_REMOVE_FLAGS -ffunction-sections -fdata-sections)
  endif()
  if(EXT_DEVICES)
    list(APPEND EXT_SOURCES ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ext/module_devices.c)
  endif()
  add_llext_target(${name}_ext
    OUTPUT  ${ext_dir}/${name}.llext
    SOURCES ${EXT_SOURCES} ${info}
  )
  if(CONFIG_RISCV)
    # A relocatable object: what it defines it resolves itself, and its sections are grouped for
    # llext (ext/llext_riscv.ld). The options below for shared objects do not apply.
    set(riscv_ld ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ext/llext_riscv.ld)
    llext_link_options(${name}_ext -Wl,-T,${riscv_ld})
    set_property(TARGET ${name}_ext_llext_lib APPEND PROPERTY LINK_DEPENDS ${riscv_ld})
    if(EXT_VERSION_SCRIPT OR EXT_DEVICES)
      message(FATAL_ERROR "multiapp: ${name}: VERSION_SCRIPT and DEVICES are not done on RISC-V")
    endif()
    if(EXT_LIBS)
      # Linked whole, then of the archives only what the extension reaches (ext/llext_riscv.ld).
      # The partial link wants a root symbol for that: an app's multiapp_info, a module's
      # multiapp_module_up(), which such a module must define.
      if(EXT_MODULE)
        set(root multiapp_module_up)
      else()
        set(root multiapp_info)
      endif()
      string(REPLACE ";" " " libs "${EXT_LIBS}")
      llext_link_options(${name}_ext "SHELL:-Wl,--whole-archive ${libs} -Wl,--no-whole-archive"
                         -Wl,--gc-sections -Wl,-u,${root})
      add_custom_target(${name}_libs DEPENDS ${EXT_LIBS})
      add_dependencies(${name}_ext_llext_lib ${name}_libs)
    endif()
  endif()
  if((EXT_MODULE OR EXT_KERNEL) AND NOT CONFIG_RISCV)
    # llext looks a global up in the host first: a module's own must stay its own.
    llext_link_options(${name}_ext -Wl,-Bsymbolic)
  endif()
  if(EXT_LIBS AND NOT CONFIG_RISCV)
    string(REPLACE ";" " " libs "${EXT_LIBS}")
    set(blobs_ld ${EXT_BLOBS_LD})
    if(NOT blobs_ld)
      set(blobs_ld ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ext/module_blobs.ld)
    endif()
    llext_link_options(${name}_ext "SHELL:-Wl,--whole-archive ${libs} -Wl,--no-whole-archive"
                       -Wl,-T,${blobs_ld})
    set_property(TARGET ${name}_ext_llext_lib APPEND PROPERTY LINK_DEPENDS ${blobs_ld})
    add_custom_target(${name}_libs DEPENDS ${EXT_LIBS})
    add_dependencies(${name}_ext_llext_lib ${name}_libs)
  endif()
  if(EXT_VERSION_SCRIPT AND NOT CONFIG_RISCV)
    llext_link_options(${name}_ext -Wl,--gc-sections -Wl,--version-script=${EXT_VERSION_SCRIPT})
    set_property(TARGET ${name}_ext_llext_lib APPEND PROPERTY LINK_DEPENDS ${EXT_VERSION_SCRIPT})
  endif()
  if(EXT_INTERNAL AND NOT CONFIG_RISCV)
    # No relaxation: it would turn calls between the two kinds of code into PC-relative ones.
    set(fast_ld ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ext/module_fast.ld)
    llext_link_options(${name}_ext -Wl,--no-relax -Wl,-T,${fast_ld})
    set_property(TARGET ${name}_ext_llext_lib APPEND PROPERTY LINK_DEPENDS ${fast_ld})
  endif()
  if(EXT_DEVICES AND NOT CONFIG_RISCV)
    set(devices_ld ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ext/module_devices.ld)
    llext_link_options(${name}_ext -Wl,-T,${devices_ld})
    set_property(TARGET ${name}_ext_llext_lib APPEND PROPERTY LINK_DEPENDS ${devices_ld})
  endif()
  if(EXT_INCLUDES)
    llext_include_directories(${name}_ext ${EXT_INCLUDES})
  endif()
  if(EXT_DEFINES)
    llext_compile_definitions(${name}_ext ${EXT_DEFINES})
  endif()
  foreach(symbol ${EXT_WRAP})
    llext_link_options(${name}_ext -Wl,--wrap=${symbol})
  endforeach()
  if(EXT_KCONFIG)
    set(pass_dir ${ext_dir}/${name}_kconfig)
    file(MAKE_DIRECTORY ${pass_dir})
    # As Zephyr's own pass on a new configuration: the fragment overrides the host's.
    set(pass_flags --handwritten-input-configs)
    if(DEFINED FORCED_CONF_FILE)
      list(APPEND pass_flags --forced-input-configs)
    endif()
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E env ${COMMON_KCONFIG_ENV_SETTINGS}
              KCONFIG_CONFIG=${pass_dir}/.config
              ${PYTHON_EXECUTABLE} ${ZEPHYR_BASE}/scripts/kconfig/kconfig.py
              --zephyr-base=${ZEPHYR_BASE} ${pass_flags} ${KCONFIG_ROOT} ${pass_dir}/.config
              ${pass_dir}/autoconf.h ${pass_dir}/sources.txt
              ${merge_config_files} ${EXT_KCONFIG} ${FORCED_CONF_FILE}
      RESULT_VARIABLE ret
    )
    if(ret)
      message(FATAL_ERROR "multiapp: ${name}: Kconfig with ${EXT_KCONFIG} failed")
    endif()
    set(pass_args)
    foreach(regex ${EXT_KCONFIG_OWN})
      list(APPEND pass_args --own ${regex})
    endforeach()
    foreach(regex ${EXT_KCONFIG_DROP})
      list(APPEND pass_args --drop ${regex})
    endforeach()
    set(pass_h ${ext_dir}/${name}_kconfig.h)
    execute_process(
      COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/extconfig.py
              --host ${DOTCONFIG} --ext ${pass_dir}/.config ${pass_args} --out ${pass_h}
      RESULT_VARIABLE ret
    )
    if(ret)
      message(FATAL_ERROR "multiapp: ${name}: extconfig.py failed")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
      ${EXT_KCONFIG} ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/extconfig.py)
    # First, as autoconf.h comes first: hal headers ext.h includes read these. extconfig.py keeps
    # out what could change what host and extension agree on.
    llext_compile_options(${name}_ext "$<$<COMPILE_LANGUAGE:C>:SHELL:-include ${pass_h}>")
    if(EXT_KCONFIG_AFTER)
      llext_compile_options(${name}_ext "$<$<COMPILE_LANGUAGE:C>:SHELL:-include ${EXT_KCONFIG_AFTER}>")
    endif()
  endif()
  # C only: an app may have assembly sources (link's esp-dsp FFT kernel).
  llext_compile_options(${name}_ext
    "$<$<COMPILE_LANGUAGE:C>:SHELL:-include ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/include/multiapp/ext.h>")
  if(EXT_PRELUDE)
    llext_compile_options(${name}_ext "$<$<COMPILE_LANGUAGE:C>:SHELL:-include ${EXT_PRELUDE}>")
  endif()
  if(EXT_APP)
    set(app_config ${ext_dir}/${name}_config.h)
    set(app_confs ${EXT_APP}/Kconfig ${EXT_APP}/prj.conf)
    set(conf_args --conf ${EXT_APP}/prj.conf)
    # The app's SoC fragment, then its board fragment (named with or without the SoC), as
    # Zephyr's own lookup applies them. No APP extension has a socs/ .conf yet; the lookup
    # is there for one that gets it.
    string(REGEX REPLACE "^/" "" soc_conf "${BOARD_QUALIFIERS}")
    string(REPLACE "/" "_" soc_conf "${soc_conf}")
    if(EXISTS ${EXT_APP}/socs/${soc_conf}.conf)
      list(APPEND app_confs ${EXT_APP}/socs/${soc_conf}.conf)
      list(APPEND conf_args --conf ${EXT_APP}/socs/${soc_conf}.conf)
    endif()
    string(REGEX REPLACE ".*/" "" cpu_cluster "${BOARD_QUALIFIERS}")
    foreach(board_conf ${NORMALIZED_BOARD_TARGET} ${BOARD}_${cpu_cluster})
      if(EXISTS ${EXT_APP}/boards/${board_conf}.conf)
        list(APPEND app_confs ${EXT_APP}/boards/${board_conf}.conf)
        list(APPEND conf_args --conf ${EXT_APP}/boards/${board_conf}.conf)
        break()
      endif()
    endforeach()
    if(EXT_APP_CONF)
      list(APPEND app_confs ${EXT_APP_CONF})
      list(APPEND conf_args --conf ${EXT_APP_CONF})
    endif()
    execute_process(
      COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/appconfig.py
              --kconfig ${EXT_APP}/Kconfig --host-config ${DOTCONFIG} ${conf_args}
              --out ${app_config}
      RESULT_VARIABLE ret
    )
    if(ret)
      message(FATAL_ERROR "multiapp: ${name}: appconfig.py failed")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
      ${app_confs} ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/appconfig.py)
    llext_compile_options(${name}_ext "$<$<COMPILE_LANGUAGE:C>:SHELL:-include ${app_config}>")
  endif()
  # Each extension's own placement rules (esp_sdr_place.h, place.h includes the apps').
  llext_compile_definitions(${name}_ext MULTIAPP_NAME=${name} ESP_SDR_PLACE_SCOPE=${name})
  llext_include_directories(${name}_ext
    ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/include
    ${apps_dir}/common/include
  )
  # The file the host loads: the extension and its trailer (apptrail.py).
  set(file ${ext_dir}/files/${name}.llext)
  if(EXT_KERNEL)
    multiapp_kernel_file(${name} ${file})
    return()
  endif()
  add_custom_command(
    OUTPUT ${file}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ext_dir}/files
    COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/apptrail.py
            --stack ${EXT_STACK} --build-id ${build_id_bin} ${ext_dir}/${name}.llext ${file}
    DEPENDS ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/apptrail.py ${ext_dir}/${name}.llext
            ${build_id_bin}
  )
  if(EXT_FILE_ONLY)
    set(ext_file_only_${name} true PARENT_SCOPE)
  else()
    set(ext_file_only_${name} false PARENT_SCOPE)
    generate_inc_file_for_target(app ${file}
      ${ZEPHYR_BINARY_DIR}/include/generated/multiapp_${name}.inc)
  endif()

  set(ext_names ${ext_names} ${name} PARENT_SCOPE)
  set(ext_usb_${name} ${EXT_USB} PARENT_SCOPE)
  if(EXT_AREA)
    set(ext_area_${name} true PARENT_SCOPE)
  else()
    set(ext_area_${name} false PARENT_SCOPE)
  endif()
  if(EXT_MODULE)
    set(ext_module_${name} true PARENT_SCOPE)
    set(ext_modules ${ext_modules} ${name} PARENT_SCOPE)
  else()
    set(ext_module_${name} false PARENT_SCOPE)
  endif()
  if(EXT_INTERNAL AND EXT_HIGH_BSS)
    set(ext_place_${name} MULTIAPP_PLACE_INTERNAL_HIGH_BSS PARENT_SCOPE)
  elseif(EXT_INTERNAL)
    set(ext_place_${name} MULTIAPP_PLACE_INTERNAL PARENT_SCOPE)
  else()
    set(ext_place_${name} MULTIAPP_PLACE_PSRAM PARENT_SCOPE)
  endif()
  if(EXT_DEVICES)
    set(ext_devices_${name} true PARENT_SCOPE)
  else()
    set(ext_devices_${name} false PARENT_SCOPE)
  endif()
  if(EXT_CONSOLE)
    set(ext_console_${name} true PARENT_SCOPE)
  else()
    set(ext_console_${name} false PARENT_SCOPE)
  endif()
  if(EXT_BANK2)
    set(ext_bank2_${name} true PARENT_SCOPE)
  else()
    set(ext_bank2_${name} false PARENT_SCOPE)
  endif()
  set(ext_requires_${name} ${EXT_REQUIRES} PARENT_SCOPE)
  set(ext_libs ${ext_libs} $<TARGET_FILE:${name}_ext_llext_lib> PARENT_SCOPE)
  set(ext_files ${ext_files} ${file} PARENT_SCOPE)
endfunction()

macro(multiapp_module name)
  multiapp_extension(${name} MODULE ${ARGN})
endmacro()

# A copy of a blob for a module, its .rodata* writable. Built without PIC, its pointer tables
# there need dynamic relocations, which the linker refuses in a read-only section; the module's
# data is in RAM anyway.
#
# multiapp_blob(<out> <archive> [NAME <copy name>] [DROP <members>])
# DROP: members taken out of the copy (ar d), for an archive two modules share; NAME tells the
# copies apart.
function(multiapp_blob out archive)
  cmake_parse_arguments(BLOB "" "NAME" "DROP" ${ARGN})
  if(CONFIG_RISCV)
    # A relocatable object keeps every relocation: the blob goes in as it is.
    set(${out} ${archive} PARENT_SCOPE)
    return()
  endif()
  if(BLOB_NAME)
    set(file ${BLOB_NAME})
  else()
    get_filename_component(file ${archive} NAME)
  endif()
  set(copy ${ext_dir}/blobs/${file})
  set(drop)
  if(BLOB_DROP)
    set(drop COMMAND ${CMAKE_AR} d ${copy} ${BLOB_DROP})
  endif()
  add_custom_command(
    OUTPUT ${copy}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ext_dir}/blobs
    COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/fastcheck.py
            --readelf ${CMAKE_READELF} ${archive}
    COMMAND ${CMAKE_OBJCOPY} --wildcard
            --set-section-flags ".rodata*=alloc,load,data,contents" ${archive} ${copy}
    ${drop}
    DEPENDS ${archive} ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/fastcheck.py
    VERBATIM
  )
  set(${out} ${copy} PARENT_SCOPE)
endfunction()

# The stages' ABI hashes: the headers an app shares with its kernels (include/multiapp/stage.h).
multiapp_stage_abi(hello_op HEADERS ${CMAKE_CURRENT_LIST_DIR}/ext/hello_stage.h)
multiapp_stage_abi(ring_sink HEADERS ${apps_dir}/rfprobe/src/ring_stage.h
                   ${apps_dir}/../include/esp_sdr/esp_sdr_diag16.h)

if(CONFIG_SOC_SERIES_ESP32S3)
# The radio, as modules every app that uses it needs: the PHY's blobs and what the HAL builds
# around them (the ROM calls some of it with the cache off), the library, and the 802.15.4 driver
# for wpan alone. The host keeps their Kconfig values and the dump banks' linker snippet
# (CONFIG_ESP_SDR_AS_EXTENSION).
include(${apps_dir}/../lib/esp_sdr/sources.cmake)
include(${apps_dir}/../lib/ieee802154/sources.cmake)
# The WMAC interrupt (0) is reserved on core 0, where the wifi module pins its task (ext/wifi.conf).
# The host has no driver, so esp_cpu_intr.c would reserve it on core 1: the SDR's CPU in a coex boot.
zephyr_compile_definitions(CONFIG_ESP32_WIFI_TASK_PINNED_TO_CORE_0=1)
multiapp_blob(libphy ${ZEPHYR_HAL_ESPRESSIF_MODULE_DIR}/zephyr/blobs/lib/${CONFIG_SOC_SERIES}/libphy.a)
multiapp_blob(librftest ${apps_dir}/../zephyr/blobs/lib/${CONFIG_SOC_SERIES}/librftest.a)
# The radio arbiter (esp_sdr_arb.c) is in phy, which the Wi-Fi driver shares with the SDR: the real
# esp_phy_enable() and esp_phy_disable() are phy's, and its wraps (__wrap_esp_phy_enable(), exported)
# are what wifi's calls reach. phy's own PLL tracking (CONFIG_ESP_WIFI_ENABLED, phy_common.c) is
# filtered by the arbiter to Wi-Fi's own holds (__wrap_phy_param_track_tot()).
if(NOT CONFIG_ESP_SDR_ARB)
  message(FATAL_ERROR "multiapp: the S3's phy module holds the radio arbiter: CONFIG_ESP_SDR_ARB=y")
endif()
multiapp_module(phy INTERNAL
  LIBS ${libphy} ${librftest}
  VERSION_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/ext/phy.ver
  WRAP esp_phy_enable esp_phy_disable phy_param_track_tot
  DEFINES ${ESP_SDR_PHY_DEFINES} CONFIG_ESP_WIFI_ENABLED
  INCLUDES ${ESP_SDR_INCLUDES}
  SOURCES ${ESP_SDR_PHY_SOURCES} ${ESP_SDR_ARB_SOURCES} ${CMAKE_CURRENT_LIST_DIR}/ext/phy_down.c)
multiapp_module(sdr INTERNAL HIGH_BSS REQUIRES phy
  INCLUDES ${ESP_SDR_INCLUDES}
  SOURCES ${ESP_SDR_SOURCES} ${CMAKE_CURRENT_LIST_DIR}/ext/sdr_up.c)
multiapp_module(ieee154 INTERNAL DEVICES REQUIRES sdr SOURCES ${IEEE154_SOURCES})

set(hello_requires REQUIRES sdr)
if(CONFIG_MULTIAPP_MODULE_TESTS)
  multiapp_module(hellofast INTERNAL SOURCES ${CMAKE_CURRENT_LIST_DIR}/ext/hellofast.c)
  multiapp_module(hellolib DEVICES REQUIRES hellofast
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/ext/hellolib.c)
  list(APPEND hello_requires hellolib)
endif()

multiapp_extension(hello ${hello_requires} DEFINES MULTIAPP_STAGE_ABI_hello_op=${stage_abi_hello_op}
  SOURCES ${CMAKE_CURRENT_LIST_DIR}/ext/hello.c)
multiapp_extension(capture STACK 4096 REQUIRES sdr SOURCES ${apps_dir}/capture/src/main.c)
# probe i2s (the S3's I2S bit clock bench) is in its own source.
multiapp_extension(rfprobe STACK 2048 REQUIRES sdr
  SOURCES ${apps_dir}/rfprobe/src/main.c ${apps_dir}/rfprobe/src/i2sbench.c)
multiapp_extension(wpan STACK 1536 REQUIRES ieee154 SOURCES ${apps_dir}/wpan/src/main.c)
set(stream ${apps_dir}/stream/src)
multiapp_extension(stream APP ${apps_dir}/stream STACK 2048 USB 1 REQUIRES sdr # MULTIAPP_USB_NCM
  SOURCES ${stream}/main.c ${stream}/vrt_rx.c ${stream}/iq_pack.c
          ${stream}/shell.c ${stream}/vrt_tx.c ${stream}/transport_udp.c)
set_source_files_properties(${stream}/iq_pack.c PROPERTIES COMPILE_OPTIONS -O3)

# osmosdr: its vendor USB class is in the host (src/osmosdr.c), the rest an
# extension whose USB blocks and ring and USB thread stacks are in the area.
set(osmosdr ${apps_dir}/osmosdr/src)
multiapp_extension(osmosdr APP ${apps_dir}/osmosdr STACK 1536 USB 2 AREA REQUIRES sdr # MULTIAPP_USB_VENDOR
  SOURCES ${osmosdr}/main.c ${osmosdr}/sdr.c ${osmosdr}/shell.c ${apps_dir}/common/src/esdr_cal.c)
set_source_files_properties(${osmosdr}/sdr.c PROPERTIES COMPILE_OPTIONS "-O3;-ffp-contract=fast")
set_source_files_properties(${apps_dir}/common/src/esdr_cal.c PROPERTIES INCLUDE_DIRECTORIES
  ${osmosdr})
target_sources(app PRIVATE src/osmosdr.c ${osmosdr}/usb_sdr.c)
set_source_files_properties(src/osmosdr.c ${osmosdr}/usb_sdr.c PROPERTIES
  INCLUDE_DIRECTORIES ${osmosdr})

# link, with the sources and flags of its own build (apps/link/sources.cmake) and esp-dsp's FFT.
include(${apps_dir}/link/sources.cmake)
esp_dsp_s3_fft(esp_dsp_fft)
multiapp_extension(link APP ${apps_dir}/link STACK 2048 REQUIRES sdr
  SOURCES ${LINK_SOURCES} ${esp_dsp_fft})
llext_compile_definitions(link_ext QAM_SPS=CONFIG_APP_SPS)

# Buffers of the apps that never run the receive ring, in its bank 0.
zephyr_linker_sources(DATA_SECTIONS multiapp_bank0.ld)
elseif(CONFIG_SOC_SERIES_ESP32S31)
# The radio as one module: the PHY's blobs and what the HAL builds around them, and the library.
# On this chip the PHY library's calls into two ROM functions go to wrappers in the library
# (esp_sdr_s31.c), which only the module holding both can link. The ROM's PHY functions it calls
# are the host's (esp32s31.rom.phy.ld).
include(${apps_dir}/../lib/esp_sdr/sources.cmake)
multiapp_blob(libphy ${ZEPHYR_HAL_ESPRESSIF_MODULE_DIR}/zephyr/blobs/lib/${CONFIG_SOC_SERIES}/libphy.a)
multiapp_blob(librftest ${apps_dir}/../zephyr/blobs/lib/${CONFIG_SOC_SERIES}/librftest.a)
# The hal's PHY sources, a section per function: kept only where reached (ext/llext_riscv.ld).
set_source_files_properties(${ESP_SDR_PHY_SOURCES} PROPERTIES
  COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
multiapp_module(sdr INTERNAL
  LIBS ${libphy} ${librftest}
  WRAP phy_set_channel_rfpll_freq phy_write_gain_mem
  DEFINES ${ESP_SDR_PHY_DEFINES}
  INCLUDES ${ESP_SDR_INCLUDES}
  SOURCES ${ESP_SDR_PHY_SOURCES} ${ESP_SDR_SOURCES} ${CMAKE_CURRENT_LIST_DIR}/ext/sdr_up.c)
zephyr_link_libraries(
  -T${ZEPHYR_HAL_ESPRESSIF_MODULE_DIR}/components/esp_rom/${CONFIG_SOC_SERIES}/ld/${CONFIG_SOC_SERIES}.rom.phy.ld)

multiapp_extension(hello REQUIRES sdr DEFINES MULTIAPP_STAGE_ABI_hello_op=${stage_abi_hello_op}
  SOURCES ${CMAKE_CURRENT_LIST_DIR}/ext/hello.c)
multiapp_extension(capture STACK 4096 REQUIRES sdr SOURCES ${apps_dir}/capture/src/main.c)
multiapp_extension(rfprobe STACK 2048 REQUIRES sdr
  DEFINES MULTIAPP_STAGE_ABI_ring_sink=${stage_abi_ring_sink}
  SOURCES ${apps_dir}/rfprobe/src/main.c)

# osmosdr_hs: its vendor USB class, osmosdr's, is in the host (src/osmosdr.c), and the host's USB
# device stands in for the one it brings up standalone (ext/usbd_prelude.h). Internal RAM for its
# USB batches, which the stream copies at 40 MB/s, and its main()'s stack.
set(osmosdr ${apps_dir}/osmosdr/src)
target_sources(app PRIVATE src/osmosdr.c ${osmosdr}/usb_sdr.c)
set_source_files_properties(src/osmosdr.c ${osmosdr}/usb_sdr.c PROPERTIES
  INCLUDE_DIRECTORIES ${osmosdr})
set(hs_conf)
if(CONFIG_MULTIAPP_OSMOSDR_HS_LOAD_LOG)
  set(hs_conf APP_CONF ${apps_dir}/osmosdr_hs/loadlog.conf)
endif()
multiapp_extension(osmosdr_hs APP ${apps_dir}/osmosdr_hs ${hs_conf} STACK 4096 USB 2 INTERNAL REQUIRES sdr # MULTIAPP_USB_VENDOR
  PRELUDE ${CMAKE_CURRENT_LIST_DIR}/ext/usbd_prelude.h
  INCLUDES ${osmosdr} ${apps_dir}/common/include ${ZEPHYR_BASE}/samples/subsys/usb/common
  SOURCES ${apps_dir}/osmosdr_hs/src/main.c ${apps_dir}/common/src/esdr_cal.c)
endif()

# Hot-swap kernels (include/multiapp/stage.h, "Hot-swapping a stage" in doc/apps/multiapp-internals.md):
# hello's stage on both chips, one in PSRAM and one INTERNAL, and rfprobe's ring sink where it has
# the diagnostic-bus ring.
multiapp_kernel(hello_x2 STAGE hello_op APP hello INCLUDES ${CMAKE_CURRENT_LIST_DIR}/ext
  SOURCES ${CMAKE_CURRENT_LIST_DIR}/kernels/hello_x2.c)
multiapp_kernel(hello_x4 STAGE hello_op APP hello INTERNAL INCLUDES ${CMAKE_CURRENT_LIST_DIR}/ext
  SOURCES ${CMAKE_CURRENT_LIST_DIR}/kernels/hello_x4.c)
if(CONFIG_SOC_SERIES_ESP32S31)
  multiapp_kernel(sink_scan16 STAGE ring_sink APP rfprobe INTERNAL
    INCLUDES ${apps_dir}/rfprobe/src
    SOURCES ${apps_dir}/rfprobe/kernels/sink_scan16.c)
endif()

# The stock Wi-Fi driver, and a small app to try it. The boot's interface is the host's.
if(CONFIG_MULTIAPP_WIFI)
  target_sources(app PRIVATE src/wifi_netif.c)
  include(${CMAKE_CURRENT_LIST_DIR}/wifi.cmake)
  multiapp_extension(wifitest STACK 2048 REQUIRES wifi
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/ext/wifitest.c)
  # ESP-NOW without joining an access point: the module and its shell, nothing else.
  multiapp_extension(espnowtest STACK 2048 REQUIRES espnow
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/ext/espnowtest.c)
  if(CONFIG_SOC_SERIES_ESP32S3 AND CONFIG_ESP_SDR_RING)
    # Wi-Fi and the SDR in windows of its sleep (doc/radio/coexistence.md). No AREA: Wi-Fi's heap
    # is the area, and the ring's banks are the host's. ieee154 takes the area when no AREA app
    # is loaded, so it must not load with wifi.
    multiapp_extension(coex STACK 2048 BANK2 REQUIRES wifi sdr espnow
      SOURCES ${CMAKE_CURRENT_LIST_DIR}/ext/wifitest.c ${CMAKE_CURRENT_LIST_DIR}/ext/coex.c)
  endif()
endif()

# MicroPython: the port, its C modules and frozen Python as an app.
if(CONFIG_MULTIAPP_MICROPYTHON)
  include(${CMAKE_CURRENT_LIST_DIR}/micropython.cmake)
endif()

# The FAT volume with the extensions' files (src/drive.c), for west dfu --storage.
dt_nodelabel(fat_node NODELABEL fat_partition)
dt_reg_size(fat_size PATH ${fat_node})
set(fat_img ${PROJECT_BINARY_DIR}/multiapp_fat.img)
add_custom_command(
  OUTPUT ${fat_img}
  COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/mkfat.py --size ${fat_size}
          --out ${fat_img} ${ext_files}
  DEPENDS ${CMAKE_CURRENT_LIST_DIR}/mkfat.py ${ext_files}
  COMMENT "FAT volume of the multiapp extensions"
)
add_custom_target(multiapp_fat ALL DEPENDS ${fat_img})

# The Wi-Fi driver's heap is the whole area, and ieee154 takes the area when no AREA app is loaded:
# neither may load with wifi.
function(ext_closure out name)
  set(todo ${ext_requires_${name}})
  set(seen)
  while(todo)
    list(POP_FRONT todo dep)
    if(NOT dep IN_LIST seen)
      list(APPEND seen ${dep})
      list(APPEND todo ${ext_requires_${dep}})
    endif()
  endwhile()
  set(${out} ${seen} PARENT_SCOPE)
endfunction()
if(CONFIG_MULTIAPP_WIFI)
  foreach(name ${ext_names})
    ext_closure(needs ${name})
    if(wifi IN_LIST needs)
      if(ext_area_${name} OR ieee154 IN_LIST needs)
        message(FATAL_ERROR "multiapp: ${name} needs wifi and the area (an AREA app or ieee154): "
                            "Wi-Fi's heap is the area")
      endif()
    endif()
  endforeach()
endif()

# The embedded extensions, and what the host exports to them.
set(embedded ${ext_dir}/embedded.c)
set(embedded_text "/* Generated by apps/multiapp/CMakeLists.txt. */\n\n#include \"apps.h\"\n#include <multiapp/multiapp.h>\n\n")
# Without CONFIG_MULTIAPP_EMBEDDED the table keeps only what a boot needs
# before it loads the app's file (name, USB profile, area).
if(CONFIG_MULTIAPP_EMBEDDED)
  foreach(name ${ext_names})
    if(NOT ext_file_only_${name})
      string(APPEND embedded_text
        "static const uint8_t ${name}_llext[] __aligned(4) = {\n#include \"multiapp_${name}.inc\"\n};\n")
    endif()
  endforeach()
endif()
string(APPEND embedded_text "\n")
foreach(name ${ext_names})
  set(deps)
  foreach(dep ${ext_requires_${name}})
    string(APPEND deps "\"${dep}\", ")
  endforeach()
  string(APPEND embedded_text "static const char *const ${name}_requires[] = {${deps}NULL};\n")
endforeach()
string(APPEND embedded_text "\nconst struct multiapp_embedded multiapp_embedded[] = {\n")
foreach(name ${ext_names})
  if(CONFIG_MULTIAPP_EMBEDDED AND NOT ext_file_only_${name})
    set(elf "${name}_llext, sizeof(${name}_llext)")
  else()
    set(elf "NULL, 0")
  endif()
  string(APPEND embedded_text
    "\t{\"${name}\", ${elf}, ${ext_usb_${name}}, ${ext_area_${name}}, "
    "${ext_module_${name}}, ${ext_place_${name}}, ${ext_devices_${name}}, ${ext_console_${name}}, "
    "${ext_bank2_${name}}, "
    "${name}_requires},\n")
endforeach()
list(LENGTH ext_names ext_count)
string(APPEND embedded_text "};\n\nconst size_t multiapp_embedded_count = ${ext_count};\n")
file(WRITE ${embedded}.tmp "${embedded_text}")
configure_file(${embedded}.tmp ${embedded} COPYONLY)

set(exports ${ext_dir}/exports.c)
set(logs ${ext_dir}/logs.c)
set(objects ${ext_dir}/objects.c)
set(module_args)
foreach(name ${ext_modules})
  list(APPEND module_args --module ${name})
endforeach()
# The area has one owner per boot: an AREA app, or the wifi module (its heap). Nothing else may
# have buffers there (exports.py), and the loader refuses a second owner (src/apps.c).
foreach(name ${ext_names})
  if(ext_area_${name})
    list(APPEND module_args --area-owner ${name})
  endif()
endforeach()
if(CONFIG_SOC_SERIES_ESP32S31)
  # Its EXT_RAM_BSS_ATTR places nothing in PSRAM (esp_sdr_place.h).
  list(APPEND module_args --psram-section .ext_ram.bss)
endif()
add_custom_command(
  OUTPUT ${exports} ${logs} ${objects} ${host_names}
  COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/exports.py
          --exports ${exports} --logs ${logs} --objects ${objects} --names ${host_names}
          --area-size ${CONFIG_MULTIAPP_AREA_SIZE} ${module_args} ${ext_libs}
  DEPENDS ${CMAKE_CURRENT_LIST_DIR}/exports.py ${ext_libs}
  COMMENT "Exporting what the multiapp extensions import"
)
target_sources(app PRIVATE ${embedded} ${exports} ${logs} ${objects})

# The build id (buildid.py): a hash of what host and extensions must agree on.
set(id_dirs ${ZEPHYR_BASE}/include ${CMAKE_CURRENT_LIST_DIR}/include ${apps_dir}/common/include
            ${apps_dir}/../include)
set(id_headers)
set(id_args)
foreach(dir ${id_dirs})
  file(GLOB_RECURSE headers ${dir}/*.h)
  list(APPEND id_headers ${headers})
  list(APPEND id_args --dir ${dir})
endforeach()
add_custom_command(
  OUTPUT ${build_id_h} ${build_id_bin}
  COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/buildid.py
          --out-h ${build_id_h} --out-bin ${build_id_bin}
          --compiler "${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}"
          --file ${AUTOCONF_H} --file ${DEVICETREE_GENERATED_H} --file ${exports} ${id_args}
  DEPENDS ${CMAKE_CURRENT_LIST_DIR}/buildid.py ${AUTOCONF_H} ${DEVICETREE_GENERATED_H}
          ${exports} ${id_headers}
  COMMENT "multiapp build id"
)
target_sources(app PRIVATE ${build_id_h})
target_include_directories(app PRIVATE ${ext_dir})
