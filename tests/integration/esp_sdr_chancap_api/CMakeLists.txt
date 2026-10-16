# SPDX-License-Identifier: GPL-3.0-or-later
#
# Hardware integration test of esp_sdr_chancap on the XIAO ESP32S3: the start contract, the
# channelizer's timing on synthetic data (both engines), and a short live run. Deployed with
# `west dfu` like the apps (USB shell, see Kconfig). See doc/testing.md.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_sdr_chancap_api)

include(${CMAKE_CURRENT_LIST_DIR}/../../../apps/common/common.cmake)

target_sources(app PRIVATE src/main.c)
