# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sphinx configuration of the project documentation: the guides in this
# directory (Markdown, through MyST), the README and the apps' READMEs.
# The API reference comes from Doxygen (Doxyfile); when it has been built
# first, it is copied into the HTML output as doxygen/.

import os
import re
import shutil
import sys
from pathlib import Path

project = "esp-sdr-zephyr"
author = "The esp-sdr-zephyr contributors"
copyright = author

extensions = ["myst_parser", "sphinx.ext.intersphinx", "sphinx_design", "sphinxcontrib.mermaid"]

# ::: fences for directives such as tab-set: GitHub shows their Markdown content
# (images, text) where it would show a ``` fence as code.
myst_enable_extensions = ["colon_fence"]
source_suffix = {".rst": "restructuredtext", ".md": "markdown"}
exclude_patterns = ["_build*", "_doxygen", "Thumbs.db", ".DS_Store"]

html_theme = "alabaster"

# The web flasher (doc/_extra/flasher), copied as it is to flasher/ in the site.
html_extra_path = ["_extra"]

intersphinx_mapping = {"zephyr": ("https://docs.zephyrproject.org/latest/", None)}

# Links to headings inside the Markdown guides (file.md#section).
myst_heading_anchors = 4

# ```mermaid fences (the guides' diagrams, the register pages' packet diagrams) render as
# diagrams here, as they do on GitHub. Packet diagrams need mermaid 11.
myst_fence_as_directive = ["mermaid"]
mermaid_version = "11.4.1"

DOC = Path(__file__).resolve().parent
REPO = DOC.parent
sys.path.insert(0, str(DOC))
import nav  # noqa: E402  (doc/nav.py: the Previous/Up/Next links)
sys.path.insert(0, str(DOC / "logo"))
import logo  # noqa: E402  (doc/logo/logo.py: the logo's formats from doc/logo/logo.svg)
DOXYGEN_HTML = DOC / "_build_doxygen" / "html"

VERSION = dict(re.findall(r"^(\w+) = (\d+)$", (REPO / "VERSION").read_text(), re.M))
version = release = "{VERSION_MAJOR}.{VERSION_MINOR}.{PATCHLEVEL}".format(**VERSION)
html_title = f"{project} {release}"
# The logo (README.rst shows it too) and, cut to the drawing, the favicon: made from
# doc/logo/logo.svg at every build. favicon.ico and apple-touch-icon.png go to the site's root,
# where browsers look for them.
LOGO_OUT = logo.build()
html_logo = str(LOGO_OUT / "logo.svg")
html_favicon = str(LOGO_OUT / "favicon.svg")
html_extra_path = [str(LOGO_OUT / "root")]

# Pages shown from outside doc/: overview.rst shows README.rst, doc/apps/<app>.rst
# shows apps/<app>/README.rst. Both are placeholders whose source this hook replaces.
#
# Every page writes its links as paths relative to its own file in the repository,
# so they work on GitHub. Here they become page links: README.rst is the overview,
# an app README its app page, a guide its page (MyST links Markdown guides to each
# other itself). Links to any other file of the repository go to GitHub. A link
# with an #anchor from an RST page goes to GitHub too: :doc: takes no anchor.
GITHUB = "https://github.com/cyber-murmel/esp-sdr-zephyr/blob/main/"
RST_LINK = re.compile(r"`([^`<]+?)\s*<([^>\s]+)>`__")
MD_LINK = re.compile(r"(?<!!)\[([^\]]+)\]\(([^)\s]+)\)")
MD_FENCE = re.compile(r"^(```|~~~).*?^\1[^\n]*$", re.M | re.S)
APP_README = re.compile(r"apps/(\w+)/README\.rst")
LOGO = re.compile(r"^\.\. image:: doc/logo/logo\.svg\n(?:   :.*\n)*\n", re.M)


def shown_from(docname):
    """The repository file a placeholder page shows, or None."""
    if docname == "overview":
        return REPO / "README.rst"
    if docname.startswith("apps/"):
        readme = REPO / "apps" / docname[len("apps/"):] / "README.rst"
        if readme.is_file():
            return readme
    return None


def page_of(path):
    """The page (as the source file under doc/) that shows a repository path, or None."""
    if path == "README.rst":
        return "doc/overview.rst"
    app = APP_README.fullmatch(path)
    if app and (DOC / "apps" / f"{app.group(1)}.rst").is_file():
        return f"doc/apps/{app.group(1)}.rst"
    if path.startswith("doc/") and path.endswith((".md", ".rst")) and (REPO / path).is_file():
        return path
    return None


def resolve(base, target):
    """A link target, relative to the repository directory base, as (path, anchor)."""
    path, _, anchor = target.partition("#")
    return os.path.normpath(os.path.join(base, path)), anchor


def github(path, anchor):
    return GITHUB + path + (f"#{anchor}" if anchor else "")


def rst_links(text, base):
    def link(m):
        label, target = m.group(1), m.group(2)
        if re.match(r"[a-z]+:", target):
            return m.group(0)
        path, anchor = resolve(base, target)
        page = page_of(path)
        if page and not anchor:
            return f":doc:`{label} </{page[len('doc/'):].rsplit('.', 1)[0]}>`"
        return f"`{label} <{github(path, anchor)}>`__"

    return RST_LINK.sub(link, text)


def md_links(text, base):
    def link(m):
        label, target = m.group(1), m.group(2)
        if re.match(r"[a-z]+:", target) or target.startswith("#"):
            return m.group(0)
        path, anchor = resolve(base, target)
        page = page_of(path)
        if page is None:
            return f"[{label}]({github(path, anchor)})"
        if page == path:
            return m.group(0)
        rel = os.path.relpath(REPO / page, REPO / base)
        return f"[{label}]({rel}{'#' + anchor if anchor else ''})"

    parts, last = [], 0
    for fence in MD_FENCE.finditer(text):
        parts += [MD_LINK.sub(link, text[last:fence.start()]), fence.group(0)]
        last = fence.end()
    return "".join(parts + [MD_LINK.sub(link, text[last:])])


def rewrite_source(app, docname, source):
    shown = shown_from(docname)
    if shown is not None:
        app.env.note_dependency(str(shown))
        source[0] = shown.read_text()
        file = shown
        if docname == "overview":
            # README.rst has no nav block (GitHub's front page); the overview page gets it here
            source[0] = nav.overview(source[0])
            # the sidebar shows the logo already (html_logo)
            source[0] = LOGO.sub("", source[0], count=1)
    else:
        file = Path(app.env.doc2path(docname))
    base = os.path.relpath(file.parent, REPO)
    if file.suffix == ".rst":
        source[0] = rst_links(source[0], base)
    elif file.suffix == ".md":
        source[0] = md_links(source[0], base)


def copy_doxygen(app, exception):
    if exception is None and app.builder.format == "html" and DOXYGEN_HTML.is_dir():
        shutil.copytree(DOXYGEN_HTML, Path(app.outdir) / "doxygen", dirs_exist_ok=True)


def setup(app):
    app.connect("source-read", rewrite_source)
    app.connect("build-finished", copy_doxygen)
