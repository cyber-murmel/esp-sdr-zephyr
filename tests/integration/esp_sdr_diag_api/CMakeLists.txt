# SPDX-License-Identifier: GPL-3.0-or-later
#
# Hardware integration test of the esp_sdr diagnostic-bus ring (esp_sdr_diag.h) on a real
# board: argument checks, the ring's byte rate and integrity, the decimated stream. See
# doc/testing.md.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_sdr_diag_api)

target_sources(app PRIVATE src/main.c)
