# SPDX-License-Identifier: GPL-3.0-or-later
#
# Two-board hardware regression test of the gapless receive and transmit streams
# (pytest/test_gapless.py, see doc/testing.md). The DUT image is the apps/osmosdr_serial
# application, unmodified: the test's job is to regress what that application ships.
# src/main.c is built from there by path, APPLICATION_CONFIG_DIR points Zephyr's board-aware
# config lookup at that directory (prj.conf, boards/, diag.conf for EXTRA_CONF_FILE), and
# KCONFIG_ROOT its Kconfig, as for ieee802154_link.

cmake_minimum_required(VERSION 3.20.0)

set(OSMO_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../apps/osmosdr_serial)
set(APPLICATION_CONFIG_DIR ${OSMO_DIR})
set(KCONFIG_ROOT ${OSMO_DIR}/Kconfig)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(stream_gapless_regression)

# The protocol header is the osmosdr app's, as in the app's own CMakeLists.
target_include_directories(app PRIVATE ${OSMO_DIR}/../osmosdr/src)
target_sources(app PRIVATE ${OSMO_DIR}/src/main.c)
