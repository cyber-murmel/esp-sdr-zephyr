#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Serial bridge for twister --device-serial-pty on the MCH2022 badge.

The ESP32's console is the RP2040's first interface (if00). Its DTR/RTS reset is unreliable
after esptool's download reset, so this opens the port with both lines low, boots the
application through the RP2040's WebUSB reset (request 0x23, wValue 0: IO0 released), then
forwards the console to stdout and stdin to the console.

usage: mch2022_pty.py <if00 port>
"""

import os
import select
import sys

import serial
import usb.core

BADGE_VID, BADGE_PID = 0x16D0, 0x0F9A
# Class request to the RP2040's WebUSB interface for the ESP32 (interface 4).
RESET_REQUEST, RESET_INTERFACE, RESET_APP = 0x23, 4, 0


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip().splitlines()[-1])
    s = serial.Serial()
    s.port = sys.argv[1]
    s.baudrate = 115200
    s.timeout = 0
    s.dtr = False
    s.rts = False
    s.open()
    dev = usb.core.find(idVendor=BADGE_VID, idProduct=BADGE_PID)
    if dev is None:
        sys.exit("no MCH2022 badge on USB")
    dev.ctrl_transfer(0x21, RESET_REQUEST, RESET_APP, RESET_INTERFACE, None)
    out = sys.stdout.buffer
    while True:
        ready, _, _ = select.select([s.fileno(), sys.stdin.fileno()], [], [], 0.1)
        if s.fileno() in ready:
            out.write(s.read(4096))
            out.flush()
        if sys.stdin.fileno() in ready:
            data = os.read(sys.stdin.fileno(), 1024)
            if not data:
                break
            s.write(data)


if __name__ == "__main__":
    main()
