# SPDX-License-Identifier: GPL-3.0-or-later

# The ROM's bootloader takes the image (west rp2040 flash; doc/index.rst). SWD on the test points
# TP9 and TP14, with any CMSIS-DAP probe.
board_runner_args(openocd --cmd-pre-init "source [find interface/cmsis-dap.cfg]")
board_runner_args(openocd --cmd-pre-init "transport select swd")
board_runner_args(openocd --cmd-pre-init "source [find target/rp2040.cfg]")
board_runner_args(openocd --cmd-pre-init "set_adapter_speed_if_not_set 2000")
board_runner_args(uf2 "--board-id=RPI-RP2")

board_set_debugger_ifnset(openocd)

include(${ZEPHYR_BASE}/boards/common/uf2.board.cmake)
include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
