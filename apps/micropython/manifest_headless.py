# SPDX-License-Identifier: GPL-3.0-or-later

# A board without a display (the ESP32-P4-Function-EV-Board's P4): _boot.py, main.py and the
# launcher, the gnuradio shim and the GRC script support, the apps that run headless
# (CONFIG_APP_EGGS) and their index (mpy_module.cmake), from micropython-lib what GRC scripts
# import.
freeze("modules")
freeze(".", "eggs/datalink")
module("eggs/__init__.py", base_path="$(EGGS_INDEX_DIR)")
require("argparse")
require("upysh")
