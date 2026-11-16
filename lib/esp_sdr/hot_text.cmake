# SPDX-License-Identifier: GPL-3.0-or-later
#
# The hot flash block (CONFIG_ESP_SDR_HOT_TEXT, place.cmake): its wrapper
# ${ZEPHYR_BINARY_DIR}/esp_sdr_hot_text.ld from hot_text.ld.in, which esp_sdr_hot_text() makes
# again with the app's list, and the check of the SoC linker script's hook.

include_guard(GLOBAL)

# The SoC linker script must take TEXT_SECTIONS snippets, ahead of the flash text's catch-all.
function(esp_sdr_hot_text_hook_check script)
  file(READ ${script} text)
  string(FIND "${text}" "snippets-text-sections.ld" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "CONFIG_ESP_SDR_HOT_TEXT: ${script} has no TEXT_SECTIONS hook "
      "(#include <snippets-text-sections.ld> ahead of the flash text's catch-all, Zephyr commit "
      "\"soc: espressif: let applications place code in esp32s3 flash text\")")
  endif()
endfunction()

# The wrapper, with <include> where the app's list goes.
function(esp_sdr_hot_text_configure include)
  set(ESP_SDR_HOT_TEXT_INCLUDE "${include}")
  configure_file(${CMAKE_CURRENT_FUNCTION_LIST_DIR}/hot_text.ld.in
                 ${ZEPHYR_BINARY_DIR}/esp_sdr_hot_text.ld @ONLY)
endfunction()

# esp_sdr_hot_text(<list file>): the app's hot flash functions, statements of input sections in
# the order the block takes them, ahead of what the rules place there. Without
# CONFIG_ESP_SDR_HOT_TEXT it does nothing.
function(esp_sdr_hot_text list)
  if(NOT CONFIG_ESP_SDR_HOT_TEXT)
    return()
  endif()
  get_filename_component(list ${list} ABSOLUTE)
  if(NOT EXISTS ${list})
    message(FATAL_ERROR "esp_sdr_hot_text(): no ${list}")
  endif()
  esp_sdr_hot_text_configure("#include \"${list}\"")
endfunction()
