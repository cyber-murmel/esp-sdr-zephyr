# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/micropython/tools/mkegg.py: a launcher app from a flowgraph, with a stand-in grcc."""

import json
import sys

import pytest

from conftest import load

mkegg = load("apps/micropython/tools/mkegg.py")

GRC = """\
options:
  parameters:
    id: {id}
    title: Test tone
    description: A tone for the tests
    author: esp-sdr
blocks: []
"""

# grcc -o DIR FLOWGRAPH: writes DIR/<id>.py; EXPORT changes what it writes, FAIL makes it fail.
GRCC = """\
#!{python}
import os, sys
out, grc = sys.argv[2], sys.argv[3]
if os.environ.get("FAIL"):
    print("grcc: broken flowgraph")
    sys.exit(1)
name = os.path.splitext(os.path.basename(grc))[0]
with open(os.path.join(out, name + ".py"), "w") as f:
    f.write("# export " + os.environ.get("EXPORT", "1") + "\\n")
"""


@pytest.fixture
def env(tmp_path, monkeypatch):
    grcc = tmp_path / "grcc"
    grcc.write_text(GRCC.format(python=sys.executable))
    grcc.chmod(0o755)
    monkeypatch.setenv("GRCC", str(grcc))
    monkeypatch.delenv("FAIL", raising=False)
    monkeypatch.delenv("EXPORT", raising=False)
    return tmp_path


def flowgraph(root, app="tone"):
    path = root / f"{app}.grc"
    path.write_text(GRC.format(id=app))
    return path


def meta(root, app="tone"):
    return json.loads((root / "eggs" / app / "metadata.json").read_text())


def test_new_app(env, run_main, capsys):
    grc = flowgraph(env)
    run_main(mkegg, grc, "-d", env / "eggs", "--category", "transmit")
    egg = env / "eggs" / "tone"
    assert (egg / "tone.grc").read_text() == grc.read_text()
    assert (egg / "tone.py").read_text() == "# export 1\n"
    assert "from .tone import main" in (egg / "__init__.py").read_text()
    assert meta(env) == {"name": "Test tone", "description": "A tone for the tests",
                         "category": "transmit", "author": "esp-sdr", "revision": 1}
    assert "revision 1" in capsys.readouterr().out


def test_revision_follows_export(env, run_main, monkeypatch):
    grc = flowgraph(env)
    run_main(mkegg, grc, "-d", env / "eggs", "--category", "transmit")
    # The app's own fields and the category stay; an unchanged export keeps the revision.
    path = env / "eggs" / "tone" / "metadata.json"
    path.write_text(json.dumps(dict(meta(env), hidden=True)))
    run_main(mkegg, grc, "-d", env / "eggs")
    assert meta(env)["revision"] == 1
    assert meta(env)["category"] == "transmit" and meta(env)["hidden"] is True
    monkeypatch.setenv("EXPORT", "2")
    run_main(mkegg, grc, "-d", env / "eggs")
    assert meta(env)["revision"] == 2
    assert (env / "eggs" / "tone" / "tone.py").read_text() == "# export 2\n"


def test_missing_revision_starts_one(env, run_main):
    grc = flowgraph(env)
    egg = env / "eggs" / "tone"
    egg.mkdir(parents=True)
    (egg / "metadata.json").write_text('{"category": "receive"}')
    (egg / "tone.py").write_text("# export 1\n")
    run_main(mkegg, grc, "-d", env / "eggs")
    assert meta(env)["revision"] == 1 and meta(env)["category"] == "receive"


def test_grcc_failure_leaves_nothing(env, run_main, monkeypatch):
    monkeypatch.setenv("FAIL", "1")
    with pytest.raises(SystemExit) as e:
        run_main(mkegg, flowgraph(env), "-d", env / "eggs")
    assert "broken flowgraph" in str(e.value.code)
    assert not (env / "eggs").exists()


def test_no_grcc(env, run_main):
    with pytest.raises(SystemExit) as e:
        run_main(mkegg, flowgraph(env), "-d", env / "eggs", "--grcc", env / "missing")
    assert "--grcc" in str(e.value.code)


def test_bad_id(env, run_main):
    with pytest.raises(SystemExit):
        run_main(mkegg, flowgraph(env, "Tone-2"), "-d", env / "eggs")
