# SPDX-License-Identifier: GPL-3.0-or-later

# The ESP32-C5 image (no display): _boot.py, main.py and the launcher, the gnuradio shim and the
# GRC script support, the bench's RF switch (modules_c5), the apps that run headless here
# (CONFIG_APP_EGGS) and their index (mpy_port.cmake), from micropython-lib what GRC scripts
# import.
freeze("modules")
freeze("modules_c5")
freeze(".", "eggs/datalink")
module("eggs/__init__.py", base_path="$(EGGS_INDEX_DIR)")
require("argparse")
require("upysh")
