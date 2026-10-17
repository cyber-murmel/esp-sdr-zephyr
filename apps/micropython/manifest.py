# SPDX-License-Identifier: GPL-3.0-or-later

# _boot.py (mounts /FLASH: and /SD:), the gnuradio shim and the GRC script support (signal,
# threading), from micropython-lib what GRC scripts import, and LVGL's event loop helper.
freeze("modules")
require("argparse")
require("upysh")
module("lv_utils.py", base_path="$(LV_BINDING_MICROPYTHON_DIR)/lib/")
