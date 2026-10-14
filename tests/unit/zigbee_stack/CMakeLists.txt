# SPDX-License-Identifier: GPL-3.0-or-later
#
# The whole Zigbee stack behind the ZNP on native_sim with a mock radio: host sessions that
# replay what zigpy-znp and zigbee-herdsman do (formation, restore, ZDO, scans, association).

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zigbee_stack_unit)

target_sources(app PRIVATE src/main.c)
