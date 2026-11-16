# SPDX-License-Identifier: GPL-3.0-or-later
#
# Hardware integration test of the CPU-paced transmit (esp_sdr_paced.h) on a real board: the
# argument contract, tone and stream excluding each other, a locked stream keeping its rate,
# the TX DC register restored, and the receiver afterwards. See doc/testing.md.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_sdr_tx_paced_api)

target_sources(app PRIVATE src/main.c)
