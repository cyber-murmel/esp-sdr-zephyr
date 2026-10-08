# SPDX-License-Identifier: GPL-3.0-or-later
from concurrent.futures import ThreadPoolExecutor

import pytest
from twister_harness import DeviceAdapter, Shell
from twister_harness.fixtures import determine_scope, get_ready_shell


@pytest.fixture(scope=determine_scope)
def shells(duts: list[DeviceAdapter]) -> list[Shell]:
    """twister_harness's shells, after clearing each shell's newline state.

    The harness presses Enter with a bare LF. A Zephyr shell swallows an LF that follows a CR
    until some other character arrives (process_nl() in subsys/shell/shell.c). `west dfu`
    ends its lines with CR LF and is the last to talk to the S3 before the harness opens
    its port, so the S3 never answers and the prompt is not found. Ctrl-C is a character
    that resets the state and does nothing at an empty prompt.
    """
    for dut in duts:
        dut.write(b"\x03")
    with ThreadPoolExecutor(max_workers=len(duts)) as executor:
        return list(executor.map(get_ready_shell, duts))
