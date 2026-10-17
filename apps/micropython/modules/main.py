# SPDX-License-Identifier: GPL-3.0-or-later
"""Runs at every start but the raw REPL's (mpremote, pyboard.py), after boot.py: main.py on
/FLASH: (the working directory) when there is one, as MicroPython would, otherwise the app
launcher (launcher.py). A frozen main.py takes precedence over the file system's, hence this."""

import os

try:
    os.stat("main.py")
except OSError:
    import launcher

    launcher.main()
else:
    with open("main.py") as f:
        _code = compile(f.read(), "main.py", "exec")
    exec(_code, {"__name__": "__main__"})
