# SPDX-License-Identifier: GPL-3.0-or-later
#
# Hardware integration test of the esp_sdr DAC-engine replay (esp_sdr_replay.h) on a real
# board: argument checks, pass timing at both rates, the repeat count, restarts, and the
# return to receive. See doc/testing.md.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_sdr_replay_api)

target_sources(app PRIVATE src/main.c)
