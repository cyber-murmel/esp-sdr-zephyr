# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sphinx configuration of the project documentation: the guides in this
# directory (Markdown, through MyST), the README and the apps' READMEs.
# The API reference comes from Doxygen (Doxyfile); when it has been built
# first, it is copied into the HTML output as doxygen/.

import re
import shutil
from pathlib import Path

from pygments.lexers.special import TextLexer
from sphinx.highlighting import lexers

project = "esp-sdr-zephyr"
author = "The esp-sdr-zephyr contributors"
copyright = author

extensions = ["myst_parser", "sphinx.ext.intersphinx"]
source_suffix = {".rst": "restructuredtext", ".md": "markdown"}
exclude_patterns = ["_build*", "_doxygen", "Thumbs.db", ".DS_Store"]

html_theme = "alabaster"
html_title = project

intersphinx_mapping = {"zephyr": ("https://docs.zephyrproject.org/latest/", None)}

# Links to headings inside the Markdown guides (file.md#section).
myst_heading_anchors = 3

# The diagram sources in the guides (GitHub renders them, Pygments cannot).
lexers["mermaid"] = TextLexer()

DOC = Path(__file__).resolve().parent
REPO = DOC.parent
DOXYGEN_HTML = DOC / "_build_doxygen" / "html"

# overview.md shows the README. Its links are relative to the repository root:
# guides and app READMEs become links to their pages here, other files go to
# GitHub.
GITHUB = "https://github.com/cyber-murmel/esp-sdr-zephyr/blob/main/"
LINK = re.compile(r"\]\(([^)\s]+)\)")


def readme_link(m):
    target = m.group(1)
    if re.match(r"[a-z]+:", target) or target.startswith("#"):
        return m.group(0)
    path, _, anchor = target.partition("#")
    app = re.fullmatch(r"apps/(\w+)/README\.rst", path)
    if path.startswith("doc/") and (REPO / path).is_file() and path.endswith(".md"):
        path = path[len("doc/"):]
    elif app and (DOC / "apps" / f"{app.group(1)}.rst").is_file():
        path = f"apps/{app.group(1)}.rst"
    else:
        path = GITHUB + path
    return f"]({path}{'#' + anchor if anchor else ''})"


def readme_source(app, docname, source):
    if docname == "overview":
        source[0] = LINK.sub(readme_link, (REPO / "README.md").read_text())


def copy_doxygen(app, exception):
    if exception is None and app.builder.format == "html" and DOXYGEN_HTML.is_dir():
        shutil.copytree(DOXYGEN_HTML, Path(app.outdir) / "doxygen", dirs_exist_ok=True)


def setup(app):
    app.connect("source-read", readme_source)
    app.connect("build-finished", copy_doxygen)
