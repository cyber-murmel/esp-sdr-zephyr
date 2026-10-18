# SPDX-License-Identifier: GPL-3.0-or-later
"""The launcher: a menu of the apps on the board, packaged as in the badge.team hatchery. An
app is a folder whose __init__.py runs it when imported (tools/mkegg.py writes one that runs
the GRC export next to it) and whose metadata.json says what it is: name, description,
category (the menu's section), author, revision; "hidden": true keeps it out of the menu. The
apps come with the firmware (eggs/, frozen) or are folders in apps/ on /FLASH: or /SD:.

Each app runs in a fresh interpreter, as badge.team's system.start() starts one after a
reset: start() leaves the app's folder in esdr.next_app() and soft-resets, main.py runs it,
and when it ends a soft reset brings the menu back. Holding the knob ends an app and comes
back here (esdr.home() tells that Ctrl-C from the console's, which ends it in the REPL, as
without the launcher).

Without a display (gnuradio._gui.headless()) the menu is a list on the console and the board
stays in the REPL; run("datalink") starts an app by its folder or menu name, and its Qt GUI
runs on the console (gnuradio/_console.py)."""

import json
import os
import sys
import time

import esdr

# Where the apps are, in menu order, and what the menu calls the place.
PLACES = ((".frozen/eggs", "bundled"), ("/FLASH:/apps", "FLASH"), ("/SD:/apps", "SD card"))
# These sections come first; the others follow in alphabetical order, apps without one last.
FIRST = ("receive", "transmit")

W = 320
H = 240
HEADER = 20
FOOTER = 54
FG = 0xDCDCDC
DIM = 0x8C8C8C
FOCUS = 0x1F4E8C

_group = None
_handoff = False  # start() or home() is soft-resetting


def apps():
    """The apps, in menu order: their metadata, "path" (the folder) and "place"."""
    found = []
    for where, place in PLACES:
        for app, meta in _scan(where):
            if meta.get("hidden") is True:
                continue
            entry = {"name": app, "description": "", "category": "", "author": "", "revision": 0}
            entry.update(meta)
            entry["path"] = where + "/" + app
            entry["place"] = place
            found.append(entry)
    found.sort(key=_order)
    return found


def start(path):
    """Run the app in the folder path in a fresh interpreter (a soft reset)."""
    _reset(path)


def run(name):
    """Start the app with this folder or menu name (any case)."""
    for app in apps():
        if name.lower() in (app["path"].rsplit("/", 1)[1].lower(), str(app["name"]).lower()):
            start(app["path"])
    raise ValueError("no app {!r}: {}".format(name, ", ".join(
        a["path"].rsplit("/", 1)[1] for a in apps())))


def _headless():
    from gnuradio import _gui

    return _gui.headless()


def home(focus=""):
    """Back to the menu (a soft reset), the app in the folder focus selected."""
    _reset("?" + focus)


def _reset(next_app):
    global _handoff
    import machine

    esdr.next_app(next_app)
    _handoff = True
    machine.soft_reset()


def main():
    """main.py: the app start() asked for, otherwise the menu."""
    path = esdr.next_app()
    try:
        if path and path[0] != "?":
            _run(path)
        elif _headless():
            _list()
            return
        else:
            _menu(path[1:])
    except KeyboardInterrupt:
        if not esdr.home():
            _banner("Ctrl-C: the REPL is on USB. Hold the knob to restart.")
            raise
    home(path.lstrip("?"))


def _run(path):
    """Run the app in the folder path; return when it ends. The Ctrl-C that ended it, from the
    console or the knob, comes out as KeyboardInterrupt."""
    where, app = path.rsplit("/", 1)
    root, package = where.rsplit("/", 1)
    name = _metadata(where, app).get("name") or app
    # The app as a module of its place (apps.<app>, eggs.<app>), so that no folder takes the
    # name of a module; its own folder goes on the path for the modules of embedded Python
    # blocks.
    sys.path[:0] = [root, path]
    interrupted = False
    error = None
    try:
        __import__(package + "." + app)
    except KeyboardInterrupt:
        interrupted = True
        _sigint()
    except SystemExit:
        if _handoff:  # the app chose what comes next (start(), home())
            raise
    except Exception as e:
        sys.print_exception(e)
        error = e
    gui = sys.modules.get("gnuradio._gui")
    if interrupted or (gui is not None and gui.interrupted):
        raise KeyboardInterrupt
    if error is not None:
        _error(name, error)


def _sigint():
    """The export's SIGINT handler, as the Ctrl-C would have run it on a desktop (it stops the
    flowgraph). The Qt GUI's loop runs it itself."""
    signal = sys.modules.get("signal")
    handler = signal.getsignal(signal.SIGINT) if signal else None
    if callable(handler):
        try:
            handler(signal.SIGINT, None)
        except SystemExit:
            pass
        except Exception as e:
            sys.print_exception(e)


def _bundled():
    """The bundled apps' metadata (eggs.INDEX, tools/eggs_index.py), from the frozen package
    whatever else sys.path holds."""
    path = sys.path[:]
    sys.path[:] = [".frozen"]
    try:
        from eggs import INDEX
    except ImportError:
        INDEX = {}
    finally:
        sys.path[:] = path
    return INDEX


def _metadata(where, app):
    if where.startswith(".frozen/"):
        return _bundled().get(app, {})
    path = where + "/" + app + "/metadata.json"
    try:
        with open(path) as f:
            meta = json.load(f)
    except OSError:  # none: the folder's name is all there is
        return {}
    except ValueError as e:
        print("{}: {}".format(path, e))
        return {}
    return meta if isinstance(meta, dict) else {}


def _scan(where):
    """(app, metadata) of every app folder in where: a module name with an __init__.py."""
    if where.startswith(".frozen/"):
        return sorted(_bundled().items())
    try:
        names = sorted(e[0] for e in os.ilistdir(where) if e[1] == 0x4000)
    except OSError:
        return []
    found = []
    for app in names:
        if app[0].isdigit() or not all(c.isalpha() or c.isdigit() or c == "_" for c in app):
            continue
        try:
            os.stat(where + "/" + app + "/__init__.py")
        except OSError:
            continue
        found.append((app, _metadata(where, app)))
    return found


def _order(app):
    section = _section(app)
    rank = FIRST.index(section) if section in FIRST else len(FIRST) + (section == "")
    return (rank, section, str(app["name"]).lower())


def _section(app):
    return app["category"].lower() if isinstance(app["category"], str) else ""


def _list():
    """The menu without a display: the apps on the console."""
    found = apps()
    print("Apps (launcher.run(NAME) starts one, Ctrl-C ends it):")
    for app in found:
        print("  {:12s} {} ({}, {})".format(app["path"].rsplit("/", 1)[1], app["name"],
                                          app["category"] or "other", app["place"]))
    if not found:
        print("  none: copy app folders to apps/ on the FLASH drive")


def _up():
    """LVGL on a clean black screen, the knob in a group of ours that ignores a press held from
    before (the hold that ended an app). Returns the lvgl module."""
    global _group
    import lvgl as lv

    if not lv.display_get_default():
        import ZDD

        ZDD.init()
    scr = lv.screen_active()
    scr.clean()
    scr.remove_flag(lv.obj.FLAG.SCROLLABLE)
    scr.set_style_bg_color(lv.color_hex(0), 0)
    scr.set_style_text_color(lv.color_hex(FG), 0)
    scr.set_style_pad_all(0, 0)
    if _group is None:
        _group = lv.group_create()
    _group.remove_all_objs()
    _group.set_default()
    # The Zephyr encoder is the only input device; a throw-away one finds it in the list.
    tmp = lv.indev_create()
    dev = tmp.get_next()
    tmp.delete()
    while dev:
        if dev.get_type() == lv.INDEV_TYPE.ENCODER:
            # Zephyr queues the knob's events across soft resets, also while an app without a
            # GUI ran: read them into the empty group, where they do nothing, and ignore a
            # press that lasts into the menu.
            dev.set_group(_group)
            dev.read()
            dev.wait_release()
        dev = dev.get_next()
    return lv


def _menu(focus):
    """The apps by section; the knob selects one and starts it. Never returns: start() soft-
    resets, the console's Ctrl-C comes out as KeyboardInterrupt."""
    lv = _up()
    scr = lv.screen_active()
    found = apps()

    head = lv.label(scr)
    head.set_text("Apps")
    head.set_pos(6, 2)
    count = lv.label(scr)
    count.set_text("{} app{}".format(len(found), "" if len(found) == 1 else "s"))
    count.set_style_text_color(lv.color_hex(DIM), 0)
    count.align(lv.ALIGN.TOP_RIGHT, -6, 2)

    lst = lv.list(scr)
    lst.set_pos(0, HEADER)
    lst.set_size(W, H - HEADER - FOOTER)
    lst.set_style_bg_color(lv.color_hex(0), 0)
    lst.set_style_border_width(0, 0)
    lst.set_style_radius(0, 0)
    lst.set_style_pad_all(0, 0)
    lst.set_style_pad_row(0, 0)
    lst.set_scrollbar_mode(lv.SCROLLBAR_MODE.OFF)

    # The selected app: what it does, where it is from.
    about = lv.label(scr)
    about.set_pos(6, H - FOOTER + 3)
    about.set_size(W - 12, 34)
    about.set_long_mode(lv.label.LONG_MODE.DOTS)
    origin = lv.label(scr)
    origin.set_pos(6, H - 18)
    origin.set_style_text_color(lv.color_hex(DIM), 0)

    def show(app):
        about.set_text(str(app["description"]))
        bits = [app["place"]]
        if app["revision"]:
            bits.append("revision {}".format(app["revision"]))
        if app["author"]:
            bits.append("by {}".format(app["author"]))
        origin.set_text(", ".join(bits))

    chosen = []
    section = None
    target = None
    for app in found:
        if _section(app) != section:
            section = _section(app)
            title = section or "other"
            t = lst.add_text(title[:1].upper() + title[1:])
            t.set_style_bg_color(lv.color_hex(0x1A1A1A), 0)
            t.set_style_text_color(lv.color_hex(DIM), 0)
        b = lst.add_button(None, str(app["name"]))
        b.set_style_bg_color(lv.color_hex(0), 0)
        b.set_style_text_color(lv.color_hex(FG), 0)
        b.set_style_border_width(0, 0)
        b.set_style_radius(0, 0)
        b.set_style_pad_left(14, 0)
        for state in (lv.STATE.FOCUSED, lv.STATE.FOCUS_KEY):
            b.set_style_bg_color(lv.color_hex(FOCUS), state)
            b.set_style_text_color(lv.color_hex(0xFFFFFF), state)
            b.set_style_outline_width(0, state)
        b.add_event_cb(lambda e, a=app: show(a), lv.EVENT.FOCUSED, None)
        b.add_event_cb(lambda e, a=app: chosen.append(a), lv.EVENT.SHORT_CLICKED, None)
        _group.add_obj(b)
        if app["path"] == focus:
            target = b
    if target is not None:
        lv.group_focus_obj(target)
        target.scroll_to_view(False)
    if not found:
        about.set_text("No apps: copy app folders to apps/ on the FLASH or SD drive.")

    while True:
        try:
            lv.task_handler()
            if chosen:
                _running(lv, str(chosen[0]["name"]))
                start(chosen[0]["path"])
            time.sleep_ms(10)
        except KeyboardInterrupt:
            if not esdr.home():
                raise


def _running(lv, name):
    """What the display keeps showing while an app without a GUI runs."""
    scr = lv.screen_active()
    scr.clean()
    t = lv.label(scr)
    t.set_text(name)
    t.align(lv.ALIGN.CENTER, 0, -12)
    hint = lv.label(scr)
    hint.set_text("Hold the knob to stop")
    hint.set_style_text_color(lv.color_hex(DIM), 0)
    hint.align(lv.ALIGN.CENTER, 0, 12)
    lv.refr_now(None)


def _error(name, e):
    """Why the app stopped, until the knob is pressed."""
    if _headless():
        print("{} stopped: {}: {}".format(name, type(e).__name__, e))
        return
    try:
        lv = _up()
        scr = lv.screen_active()
        head = lv.label(scr)
        head.set_text("{} stopped".format(name))
        head.set_style_text_color(lv.color_hex(0xFF6060), 0)
        head.set_pos(8, 8)
        msg = lv.label(scr)
        msg.set_pos(8, 34)
        msg.set_size(W - 16, H - 34 - 60)
        msg.set_long_mode(lv.label.LONG_MODE.DOTS)
        msg.set_text("{}: {}".format(type(e).__name__, e))
        btn = lv.button(scr)
        lv.label(btn).set_text("Back to the apps")
        btn.align(lv.ALIGN.BOTTOM_MID, 0, -12)
        done = []
        btn.add_event_cb(lambda ev: done.append(True), lv.EVENT.SHORT_CLICKED, None)
        _group.add_obj(btn)
    except Exception as e2:
        sys.print_exception(e2)
        return
    while not done:
        lv.task_handler()
        time.sleep_ms(10)


def _banner(text):
    """A note at the bottom of whatever the display shows (nothing without LVGL)."""
    try:
        import lvgl as lv

        if not lv.display_get_default():
            return
        b = lv.label(lv.screen_active())
        b.set_width(W)
        b.set_text(text)
        b.set_style_bg_color(lv.color_hex(0x5A3A00), 0)
        b.set_style_bg_opa(lv.OPA.COVER, 0)
        b.set_style_text_color(lv.color_hex(0xFFFFFF), 0)
        b.set_style_pad_all(4, 0)
        b.align(lv.ALIGN.BOTTOM_MID, 0, 0)
        lv.refr_now(None)
    except Exception:
        pass
