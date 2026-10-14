# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit tests of the Zigbee MAC layer (lib/zigbee: frame codec, MAC state machine against a mock
# radio, network formation) on native_sim, through the CONFIG_ZIGBEE_ZNP library.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zigbee_mac_unit)

target_sources(app PRIVATE src/main.c)
