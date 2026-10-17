# SPDX-License-Identifier: GPL-3.0-or-later
"""threading for GRC scripts: Event only (there is one Python thread)."""

import time


class Event:
    def __init__(self):
        self._flag = False

    def set(self):
        self._flag = True

    def clear(self):
        self._flag = False

    def is_set(self):
        return self._flag

    def wait(self, timeout=None):
        start = time.ticks_ms()
        while not self._flag:
            if timeout is not None and time.ticks_diff(time.ticks_ms(), start) >= timeout * 1000:
                return False
            time.sleep_ms(10)
        return True
