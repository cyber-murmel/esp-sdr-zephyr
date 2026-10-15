# SPDX-License-Identifier: GPL-3.0-or-later
#
# The development shell: the packages of default.nix's `dev`, the Zephyr SDK and the esdr
# SoapySDR module (SOAPY_SDR_PLUGIN_PATH).

{ ... }@args:
(import ./default.nix args).shell
