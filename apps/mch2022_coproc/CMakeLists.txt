# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_sdr_mch2022_coproc)

target_include_directories(app PRIVATE src ../../include)
target_sources(app PRIVATE
  src/main.c
  src/bridge.c
  src/esp32_ctl.c
  src/fwlog.c
  src/i2c_regs.c
  src/periph.c
  src/pio_out.c
  src/qspi_select.c
  src/regmap.c
  src/reset_detect.c
  src/sysctl.c
  src/uart_dma.c
  src/usb_dev.c
  src/usb_vendor.c)
