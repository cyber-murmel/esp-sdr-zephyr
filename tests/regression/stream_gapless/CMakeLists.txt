# SPDX-License-Identifier: GPL-3.0-or-later
#
# Two-board hardware regression test of the gapless receive and transmit streams
# (pytest/test_gapless.py, see doc/dev/testing.md). The DUT image is the apps/osmosdr_serial
# application, unmodified: the test's job is to regress what that application ships.
# Its sources come from there through its sources.cmake, APPLICATION_CONFIG_DIR points Zephyr's
# board-aware config lookup at that directory (prj.conf, boards/, diag.conf for
# EXTRA_CONF_FILE), and KCONFIG_ROOT its Kconfig, as for ieee802154_link.

cmake_minimum_required(VERSION 3.20.0)

set(OSMO_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../apps/osmosdr_serial)
set(APPLICATION_CONFIG_DIR ${OSMO_DIR})
set(KCONFIG_ROOT ${OSMO_DIR}/Kconfig)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(stream_gapless_regression)

include(${OSMO_DIR}/sources.cmake)
