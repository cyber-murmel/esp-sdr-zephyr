# SPDX-License-Identifier: GPL-3.0-or-later

if(BOARD_QUALIFIERS MATCHES "esp32s31")
  # sysbuild/mcuboot.overlay gives MCUboot the S3 boards' flash layout; the ESP32-S31 has its own.
  set(mcuboot_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/sysbuild/mcuboot_s31.overlay
      CACHE INTERNAL "MCUboot's devicetree overlay")
  set(mcuboot_EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/sysbuild/mcuboot_s31.conf
      CACHE INTERNAL "MCUboot's extra Kconfig fragments")
endif()

# The project's own signing key (apps/common/sysbuild/Kconfig.signing, apps/multiapp/Kconfig.sysbuild).
include(${CMAKE_CURRENT_LIST_DIR}/../common/sysbuild/signing.cmake)

if(BOARD MATCHES "^esp32s31_function_coreboard")
  # The CoreBoard is a network host: Ethernet, addresses, a name (doc/dev/network-services.md). Its
  # devicetree part is #included by boards/esp32s31_function_coreboard_esp32s31_hpcore.overlay.
  # Fragments given with -Dmultiapp_EXTRA_CONF_FILE (hil.conf) stay.
  # mcumgr.conf: MCUmgr over UDP behind the arm gate (src/netsvc.c).
  # ssh.conf: the SSH server, public key login only (src/sshd.c, src/console_mux.c).
  foreach(frag net mcumgr ssh)
    if(NOT "${multiapp_EXTRA_CONF_FILE}" MATCHES "common/net/${frag}\\.conf")
      set(multiapp_EXTRA_CONF_FILE ${multiapp_EXTRA_CONF_FILE}
          ${CMAKE_CURRENT_LIST_DIR}/../common/net/${frag}.conf
          CACHE INTERNAL "multiapp's extra Kconfig fragments")
    endif()
  endforeach()
endif()
