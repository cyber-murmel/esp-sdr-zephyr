# SPDX-License-Identifier: GPL-3.0-or-later
"""include/esp_sdr/esdr_fpga_regs.h is generated from gateware/esdr_fpga/regs.py."""

import sys

from conftest import REPO


def test_header_is_current():
    sys.path.insert(0, str(REPO / "gateware"))
    try:
        from esdr_fpga.regs import header
    finally:
        sys.path.pop(0)
    path = REPO / "include/esp_sdr/esdr_fpga_regs.h"
    assert path.read_text() == header(), \
        "regenerate: cd gateware && python -m esdr_fpga.regs > ../include/esp_sdr/esdr_fpga_regs.h"
