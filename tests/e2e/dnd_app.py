#!/usr/bin/env python3
# A GTK 4 window for drag and drop tests: the left half drags <file> out as a file list (text/uri-list), the right
# half takes file drops and prints "dropped <uri>" per file. Usage: dnd_app.py <title> <file>.
import sys

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gdk, Gio, GObject, Gtk  # noqa: E402


def on_activate(app):
    window = Gtk.ApplicationWindow(application=app, title=sys.argv[1], default_width=480, default_height=240)
    box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, homogeneous=True)
    source = Gtk.Label(label="drag me")
    target = Gtk.Label(label="drop here")
    box.append(source)
    box.append(target)
    window.set_child(box)

    drag = Gtk.DragSource(actions=Gdk.DragAction.COPY)
    files = Gdk.FileList.new_from_list([Gio.File.new_for_path(sys.argv[2])])
    drag.connect("prepare", lambda *_: Gdk.ContentProvider.new_for_value(GObject.Value(Gdk.FileList, files)))
    drag.connect("drag-end", lambda _s, _d, deleted: print(f"drag ended deleted={deleted}", flush=True))
    source.add_controller(drag)

    drop = Gtk.DropTarget.new(Gdk.FileList, Gdk.DragAction.COPY | Gdk.DragAction.MOVE)

    def on_drop(_target, value, _x, _y):
        for file in value.get_files():
            print(f"dropped {file.get_uri()}", flush=True)
        return True

    drop.connect("drop", on_drop)
    target.add_controller(drop)
    window.present()
    print("ready", flush=True)


app = Gtk.Application(application_id="dev.andromeda.DndTest", flags=Gio.ApplicationFlags.NON_UNIQUE)
app.connect("activate", on_activate)
app.run([])
