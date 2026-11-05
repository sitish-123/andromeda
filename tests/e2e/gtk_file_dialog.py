#!/usr/bin/env python3
"""GTK 4 client for files_picker.sh: opens Gtk.FileDialog through the portal and prints the result.

Usage: gtk_file_dialog.py open|save <initial folder> [initial name]
Prints "RESULT <path>" or "CANCELLED <message>", then exits.
"""
import sys

import gi

gi.require_version("Gtk", "4.0")
from gi.repository import Gio, GLib, Gtk  # noqa: E402

mode, folder = sys.argv[1], sys.argv[2]
name = sys.argv[3] if len(sys.argv) > 3 else None
app = Gtk.Application(application_id="dev.andromeda.TestFileDialog", flags=Gio.ApplicationFlags.NON_UNIQUE)


def done(dialog, result, window):
    try:
        file = dialog.open_finish(result) if mode == "open" else dialog.save_finish(result)
        print("RESULT", file.get_path(), flush=True)
    except GLib.Error as e:
        print("CANCELLED", e.message, flush=True)
    window.close()


def activate(application):
    window = Gtk.ApplicationWindow(application=application, title="file dialog test")
    window.present()
    dialog = Gtk.FileDialog(initial_folder=Gio.File.new_for_path(folder))
    text = Gtk.FileFilter(name="Text")
    text.add_pattern("*.txt")
    everything = Gtk.FileFilter(name="All files")
    everything.add_pattern("*")
    filters = Gio.ListStore.new(Gtk.FileFilter)
    filters.append(text)
    filters.append(everything)
    dialog.set_filters(filters)
    if name:
        dialog.set_initial_name(name)
    # The window must be mapped first: GTK exports its xdg-foreign handle as the portal parent.
    GLib.timeout_add(300, lambda: (getattr(dialog, mode)(window, None, done, window), False)[1])


app.connect("activate", activate)
app.run([])
