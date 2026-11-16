# SPDX-License-Identifier: GPL-3.0-or-later

if(SB_CONFIG_ESDR_C6_APPS)
  set(c6apps ${APP_DIR}/c6apps)
  set(mcuboot_EXTRA_CONF_FILE ${c6apps}/mcuboot.conf CACHE INTERNAL "")
  set(mcuboot_EXTRA_DTC_OVERLAY_FILE ${c6apps}/mcuboot.overlay CACHE INTERNAL "")
  set(${DEFAULT_IMAGE}_EXTRA_DTC_OVERLAY_FILE ${c6apps}/slot0.overlay CACHE INTERNAL "")
  # In the order of boot_select.c's slots and of C6_APPS in west dfu.
  set(slot 1)
  foreach(app capture rfprobe wpan zstack)
    set(${app}_EXTRA_DTC_OVERLAY_FILE ${c6apps}/slot${slot}.overlay CACHE INTERNAL "")
    ExternalZephyrProject_Add(APPLICATION ${app} SOURCE_DIR ${APP_DIR}/../${app})
    # Booted by MCUboot as the main image is. Only one image can have the type: it owns menuconfig.
    set_property(TARGET ${app} APPEND PROPERTY IMAGE_CONF_SCRIPT
                 ${CMAKE_SOURCE_DIR}/image_configurations/MAIN_image_default.cmake)
    math(EXPR slot "${slot} + 1")
  endforeach()
endif()
