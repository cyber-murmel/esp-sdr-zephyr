# SPDX-License-Identifier: GPL-3.0-or-later

# _boot.py (mounts /FLASH: and /SD:), main.py and the launcher, the gnuradio shim and the GRC
# script support (signal, threading), from micropython-lib what GRC scripts import, and LVGL's
# event loop helper.
freeze("modules")
# The bundled apps (eggs/<app>/, run from .frozen/eggs) and their index (mpy_port.cmake).
freeze(".", "eggs")
module("eggs/__init__.py", base_path="$(EGGS_INDEX_DIR)")
require("argparse")
require("upysh")
module("lv_utils.py", base_path="$(LV_BINDING_MICROPYTHON_DIR)/lib/")
