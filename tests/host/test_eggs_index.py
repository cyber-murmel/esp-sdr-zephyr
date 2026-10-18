# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/micropython/tools/eggs_index.py: the launcher's index of the bundled apps."""

import ast
import json

import pytest

from conftest import REPO, load

eggs_index = load("apps/micropython/tools/eggs_index.py")


def egg(root, name, meta, init=True):
    d = root / name
    d.mkdir(parents=True)
    if init:
        (d / "__init__.py").write_text("")
    if meta is not None:
        (d / "metadata.json").write_text(meta if isinstance(meta, str) else json.dumps(meta))
    return d


def read_index(path):
    tree = ast.parse(path.read_text())
    (assign,) = [n for n in tree.body if isinstance(n, ast.Assign)]
    return ast.literal_eval(assign.value)


def test_index_sorted_and_typed(tmp_path):
    egg(tmp_path, "zeta", {"name": "Zeta", "revision": 3})
    egg(tmp_path, "alpha", {"name": "Alpha", "hidden": True, "extra": [1, 2]})
    (tmp_path / "README").write_text("not an app")
    idx = eggs_index.index(tmp_path)
    assert list(idx) == ["alpha", "zeta"]
    assert idx["alpha"] == {"name": "Alpha", "hidden": True, "extra": [1, 2]}


@pytest.mark.parametrize("name,meta,init", [
    ("Bad-Name", {}, True),                 # not a module name
    ("noinit", {}, False),                  # no __init__.py
    ("nometa", None, True),                 # no metadata.json
    ("broken", "{", True),                  # not JSON
    ("listy", "[1]", True),                 # not an object
    ("typed", {"revision": "2"}, True),     # wrong type
    ("flag", {"hidden": 1}, True),          # int is not bool
])
def test_index_rejects(tmp_path, name, meta, init):
    egg(tmp_path, name, meta, init)
    with pytest.raises(SystemExit):
        eggs_index.index(tmp_path)


def test_main_writes_only_on_change(tmp_path, run_main):
    eggs = tmp_path / "eggs"
    egg(eggs, "tone", {"name": "Tone", "revision": 2})
    out = tmp_path / "gen" / "eggs" / "__init__.py"
    run_main(eggs_index, eggs, out)
    assert read_index(out) == {"tone": {"name": "Tone", "revision": 2}}
    stamp = out.stat().st_mtime_ns
    out.touch()
    before = out.stat().st_mtime_ns
    run_main(eggs_index, eggs, out)
    assert out.stat().st_mtime_ns == before >= stamp
    (eggs / "tone" / "metadata.json").write_text(json.dumps({"name": "Tone", "revision": 3}))
    run_main(eggs_index, eggs, out)
    assert read_index(out)["tone"]["revision"] == 3


def test_only(tmp_path, run_main):
    eggs = tmp_path / "eggs"
    egg(eggs, "tone", {"name": "Tone"})
    egg(eggs, "datalink", {"name": "Data link"})
    out = tmp_path / "gen" / "eggs" / "__init__.py"
    run_main(eggs_index, "--only", "datalink", eggs, out)
    assert read_index(out) == {"datalink": {"name": "Data link"}}
    with pytest.raises(SystemExit):
        eggs_index.index(eggs, ["missing"])


def test_bundled_apps_are_valid():
    idx = eggs_index.index(REPO / "apps/micropython/eggs")
    assert {"receive", "transmit"} <= {meta.get("category") for meta in idx.values()}
    for app, meta in idx.items():
        assert meta.get("name") and meta.get("description"), app
        assert (REPO / "apps/micropython/eggs" / app / f"{app}.grc").is_file(), app
