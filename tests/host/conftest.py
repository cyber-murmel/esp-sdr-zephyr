# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of the repository's Python tools (pytest, CPython): see doc/testing.md."""

import importlib.util
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]


def load(relpath, name=None):
    """Import a tool by its path (the scripts are not packages; some names have dashes)."""
    path = REPO / relpath
    name = name or path.stem.replace("-", "_")
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture
def run_main(monkeypatch):
    """Call a tool's main() with the given command line arguments."""
    def run(mod, *args):
        monkeypatch.setattr(sys, "argv", [mod.__name__, *map(str, args)])
        return mod.main()
    return run
