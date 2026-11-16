# SPDX-License-Identifier: GPL-3.0-or-later

# The badge's bootloader wants a header in front of the image: zephyr.uf2 with it, in place of
# the one Zephyr made (support/rp2040_image.py).
set_property(GLOBAL APPEND PROPERTY extra_post_build_commands
  COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/support/rp2040_image.py
          ${CMAKE_BINARY_DIR}/zephyr/${KERNEL_BIN_NAME}
          ${CMAKE_BINARY_DIR}/zephyr/${KERNEL_UF2_NAME}
)
