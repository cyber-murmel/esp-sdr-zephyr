# SPDX-License-Identifier: GPL-3.0-or-later
#
# Hardware integration test of the S31's dump ring (esp_sdr_dumpring.h) on a real board: the
# argument contract, a run's word count against its time, the turns' cost, refusals during a
# run, the receiver afterwards and, with two cores, what the other core keeps. See
# doc/dev/testing.md.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_sdr_dumpring_api)

target_sources(app PRIVATE src/main.c)
