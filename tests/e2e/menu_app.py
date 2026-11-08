#!/usr/bin/env python3
# A GTK 4 window that exports a menubar (org.gtk.Menus over gtk_shell1), for the macOS top-bar menu shots. The rows
# follow .agent/mockups/desktop-layouts.html section 7. Usage: menu_app.py <title>.
import sys

import gi

gi.require_version("Gtk", "4.0")
from gi.repository import Gio, Gtk  # noqa: E402

MENUS = [
    ("File", [[("New Window", "<Control>n"), ("New Folder", "<Shift><Control>n"), ("New Tab", "<Control>t")],
              [("Open", "<Control>o"), ("Get Info", "<Control>i")], [("Close Window", "<Control>w")]]),
    ("Edit", [[("Undo", "<Control>z"), ("Redo", "<Shift><Control>z")], [("Cut", "<Control>x"), ("Copy", "<Control>c"),
              ("Paste", "<Control>v")]]),
    ("View", [[("As Icons", "<Control>1"), ("As List", "<Control>2")]]),
    ("Go", [[("Back", "<Control>bracketleft"), ("Forward", "<Control>bracketright")]]),
    ("Window", [[("Minimize", None)]]),
    ("Help", [[("Files Help", None)]]),
]


def on_activate(app):
    bar = Gio.Menu()
    for name, sections in MENUS:
        menu = Gio.Menu()
        for rows in sections:
            section = Gio.Menu()
            for label, accel in rows:
                action = "app." + label.lower().replace(" ", "-")
                section.append(label, action)
                if accel:
                    app.set_accels_for_action(action, [accel])
            menu.append_section(None, section)
        bar.append_submenu(name, menu)
    for _, sections in MENUS:
        for rows in sections:
            for label, _ in rows:
                app.add_action(Gio.SimpleAction.new(label.lower().replace(" ", "-"), None))
    app.set_menubar(bar)
    window = Gtk.ApplicationWindow(application=app, title=sys.argv[1], default_width=640, default_height=400)
    window.set_show_menubar(False)
    window.present()


app = Gtk.Application(application_id="dev.andromeda.MenuTest", flags=Gio.ApplicationFlags.NON_UNIQUE)
app.connect("activate", on_activate)
app.run([sys.argv[0]])
