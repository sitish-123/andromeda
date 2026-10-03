#!/usr/bin/env python3
"""The shell's two clipboard calls on org.umbriel.Link1, for E2E tests without a shell.

Usage: link_clipboard_dbus.py offer MIME TEXT      offers TEXT as the desktop clipboard (through a memfd)
       link_clipboard_dbus.py pull DEVICE ID MIME  pulls a device's clip through a pipe and prints it
"""
import os
import sys
import threading

from gi.repository import Gio, GLib


def call(method, signature, args, fds, reply):
    bus = Gio.bus_get_sync(Gio.BusType.SESSION)
    result, _ = bus.call_with_unix_fd_list_sync(
        "org.umbriel.Link1", "/org/umbriel/Link1", "org.umbriel.Link1", method, GLib.Variant(signature, args),
        GLib.VariantType(reply), Gio.DBusCallFlags.NONE, 20000, fds, None)
    return result.unpack()


def offer(mime, text):
    fd = os.memfd_create("clip")
    os.write(fd, text.encode())
    os.lseek(fd, 0, os.SEEK_SET)
    fds = Gio.UnixFDList()
    index = fds.append(fd)
    call("OfferClipboard", "(ash)", ([mime], index), fds, "()")


def pull(device, clip, mime):
    read, write = os.pipe()
    chunks = []
    reader = threading.Thread(target=lambda: chunks.append(os.fdopen(read, "rb").read()))
    reader.start()
    fds = Gio.UnixFDList()
    index = fds.append(write)
    os.close(write)
    call("PullClipboard", "(stsh)", (device, int(clip), mime, index), fds, "(t)")
    fds = None  # closes the list's copy of the write end, so the reader sees the end
    reader.join()
    sys.stdout.buffer.write(chunks[0])


if __name__ == "__main__":
    try:
        if sys.argv[1] == "offer":
            offer(sys.argv[2], sys.argv[3])
        else:
            pull(sys.argv[2], sys.argv[3], sys.argv[4])
    except GLib.Error as error:
        print(error.message, file=sys.stderr)
        sys.exit(1)
