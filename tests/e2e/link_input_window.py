#!/usr/bin/env python3
"""A fullscreen GTK 4 window that logs the input it receives as JSON lines: pointer motion, buttons, scrolls, key
names, and the text of its entry. Usage: link_input_window.py LOG."""
import json
import sys
import time

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gdk, Gtk  # noqa: E402

log = open(sys.argv[1], "a", buffering=1)


def record(event, **fields):
    log.write(json.dumps({"t": int(time.time() * 1000), "event": event, **fields}) + "\n")


def activate(app):
    window = Gtk.ApplicationWindow(application=app, title="link-input")
    entry = Gtk.Entry()
    entry.connect("changed", lambda e: record("text", text=e.get_text()))
    # Clicks land on the area, so a right click opens no entry menu that would take the keys.
    area = Gtk.DrawingArea(vexpand=True)
    box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
    box.append(entry)
    box.append(area)
    window.set_child(box)

    motion = Gtk.EventControllerMotion()
    motion.connect("motion", lambda _c, x, y: record("motion", x=round(x), y=round(y)))
    window.add_controller(motion)

    # Raw events: a click gesture loses the release to the entry's own gestures.
    buttons = Gtk.EventControllerLegacy()
    buttons.set_propagation_phase(Gtk.PropagationPhase.CAPTURE)

    def button(controller, _event):
        event = controller.get_current_event()  # PyGObject passes None for the signal's own argument
        kind = {Gdk.EventType.BUTTON_PRESS: "press", Gdk.EventType.BUTTON_RELEASE: "release"}.get(event.get_event_type())
        if kind:
            record(kind, button=event.get_button())
        return False

    buttons.connect("event", button)
    window.add_controller(buttons)

    scroll = Gtk.EventControllerScroll(flags=Gtk.EventControllerScrollFlags.BOTH_AXES)
    scroll.connect("scroll", lambda _c, dx, dy: record("scroll", dx=dx, dy=dy) or False)
    window.add_controller(scroll)

    keys = Gtk.EventControllerKey()
    keys.set_propagation_phase(Gtk.PropagationPhase.CAPTURE)
    keys.connect("key-pressed", lambda _c, keyval, _code, _state: record("key", name=Gdk.keyval_name(keyval)) or False)
    window.add_controller(keys)

    window.fullscreen()
    window.present()
    entry.grab_focus()
    record("ready")


app = Gtk.Application(application_id="org.andromeda.LinkInputProbe")
app.connect("activate", activate)
app.run([])
