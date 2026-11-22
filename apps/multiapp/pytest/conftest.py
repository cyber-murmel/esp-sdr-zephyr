# SPDX-License-Identifier: GPL-3.0-or-later
"""The tests that switch the running app (MicroPython's, test_multiapp_s31.py) run after all others in a
module: an app switch is a reboot, and the board's default app comes back with the last of them."""


def pytest_configure(config):
    config.addinivalue_line("markers", "switches_app: reboots the board into another app; runs last")


def pytest_collection_modifyitems(config, items):
    plain = [i for i in items if i.get_closest_marker("switches_app") is None]
    last = [i for i in items if i.get_closest_marker("switches_app") is not None]
    items[:] = plain + last
