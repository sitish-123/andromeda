"""Socket activation survives a compositor crash (docs/resilience.md R1), without the host user manager:
the test owns the listening socket like andromeda.socket, kills the compositor with SIGSEGV, starts it again on
the same socket, and a client that connected during the gap is served."""

import fcntl
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

HELPER = r'''#!PYTHON
import os, sys
from pathlib import Path
name = Path(sys.argv[0]).name
if name == "systemctl" and sys.argv[2:3] == ["import-environment"]:
    with (Path(os.environ["RESTART_ROOT"]) / "imported").open("a") as stream:
        stream.write(os.environ["WAYLAND_DISPLAY"] + "\n")
if name == "systemd-run":
    sys.exit(1)
'''

O_CLOEXEC = 0o2000000


def wl_sync(sock):
    """wl_display.sync(new_id 2); the reply is wl_callback(2).done."""
    sock.sendall(struct.pack("=III", 1, (12 << 16) | 0, 2))


def await_callback(sock, timeout):
    sock.settimeout(timeout)
    data = b""
    while True:
        chunk = sock.recv(4096)
        assert chunk, "compositor closed the client connection"
        data += chunk
        offset = 0
        while len(data) - offset >= 8:
            obj, word = struct.unpack_from("=II", data, offset)
            if obj == 2 and word & 0xFFFF == 0:
                return
            offset += word >> 16


def await_notify(notify, want, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        notify.settimeout(max(0.01, deadline - time.monotonic()))
        try:
            if notify.recv(128) == want:
                return
        except socket.timeout:
            break
    raise AssertionError(f"no {want!r} within {timeout} s")


def await_line(path, prefix, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists() and any(line.startswith(prefix) for line in path.read_text().splitlines()):
            return
        time.sleep(0.02)
    raise AssertionError(f"no '{prefix}' in {path.name} within {timeout} s: {path.read_text() if path.exists() else ''}")


def main(binary, mode):
    with tempfile.TemporaryDirectory(prefix="um-restart.") as directory:
        root = Path(directory)
        (root / "bin").mkdir()
        helper = root / "helper"
        helper.write_text(HELPER.replace("#!PYTHON", "#!" + sys.executable, 1))
        helper.chmod(0o700)
        for name in ("systemctl", "dbus-update-activation-environment", "systemd-run"):
            (root / "bin" / name).symlink_to(helper)
        config = root / "config.toml"
        config.write_text('[general]\nxwayland = false\nautostart = []\n')

        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(str(root / "wayland-andromeda"))
        listener.listen(16)
        notify = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        notify.bind(str(root / "notify"))

        environment = {k: v for k, v in os.environ.items()
                       if k not in ("WAYLAND_DISPLAY", "WAYLAND_SOCKET", "DISPLAY", "ANDROMEDA_SOCKET",
                                    "SYSTEMD_EXEC_PID", "LISTEN_PID", "LISTEN_FDS", "LISTEN_FDNAMES")}
        environment.update(
            PATH=str(root / "bin") + ":" + os.environ["PATH"],
            XDG_RUNTIME_DIR=str(root),
            XDG_CACHE_HOME=str(root / "cache"),
            DBUS_SESSION_BUS_ADDRESS="unix:path=" + str(root / "no-session-bus"),
            SYSTEMD_BUS_ADDRESS="unix:path=" + str(root / "no-manager-bus"),
            DBUS_SYSTEM_BUS_ADDRESS="unix:path=" + str(root / "no-system-bus"),
            NOTIFY_SOCKET=str(root / "notify"),
            RESTART_ROOT=str(root),
            LISTEN_FDS="1",
            WATCHDOG_USEC="400000",
            ANDROMEDA_LOCK_ESCAPE_MS="1000",
            WLR_BACKENDS="headless",
            WLR_HEADLESS_OUTPUTS="1",
            WLR_LIBINPUT_NO_DEVICES="1",
        )
        # systemd sets these per exec, so the shell sets them to the compositor's own PID.
        # Above 3, so the shell redirect "3<&fd fd<&-" never closes the socket it just moved.
        fd = fcntl.fcntl(listener.fileno(), fcntl.F_DUPFD_CLOEXEC, 10)
        command = ["/bin/sh", "-c",
                   f'export SYSTEMD_EXEC_PID=$$ LISTEN_PID=$$ WATCHDOG_PID=$$; exec "$@" 3<&{fd} {fd}<&-',
                   "session-restart", str(binary), "-c", str(config)]

        def start(log):
            return subprocess.Popen(command, env=environment, stdout=log, stderr=log, start_new_session=True,
                                    pass_fds=(fd,))

        processes = []
        clients = []
        with (root / "compositor.log").open("w+") as log:
            try:
                processes.append(start(log))
                await_notify(notify, b"READY=1", 10)
                if mode == "socket":
                    # Two pings inside one WATCHDOG_USEC (0.4 s): the timer re-arms at half the interval.
                    await_notify(notify, b"WATCHDOG=1", 1)
                    await_notify(notify, b"WATCHDOG=1", 0.4)
                    first = processes[0].pid
                    flags = int(next(line.split()[1] for line in open(f"/proc/{first}/fdinfo/3")
                                     if line.startswith("flags:")), 8)
                    assert flags & O_CLOEXEC, "the listening socket leaks into spawned apps (no FD_CLOEXEC)"
                    assert not (root / "wayland-0").exists(), "the compositor made its own socket"
                    assert (root / "imported").read_text().split() == ["wayland-andromeda"], "WAYLAND_DISPLAY not kept"

                    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                        client.connect(str(root / "wayland-andromeda"))
                        wl_sync(client)
                        await_callback(client, 5)

                    os.kill(first, signal.SIGSEGV)
                    processes[0].wait(timeout=10)
                    assert processes[0].returncode != 0, "SIGSEGV must fail the unit so Restart=on-failure applies"

                    # A client in the gap waits in the listen backlog and is served by the next compositor.
                    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                        client.connect(str(root / "wayland-andromeda"))
                        wl_sync(client)
                        processes.append(start(log))
                        await_notify(notify, b"READY=1", 10)
                        await_callback(client, 5)
                    assert (root / "imported").read_text().split() == ["wayland-andromeda"] * 2

                else:
                    # R3: a crash while locked restarts into the lock; only a lock client's unlock ends it.
                    client_env = {k: v for k, v in environment.items() if not k.startswith(("LISTEN_", "WATCHDOG_"))}
                    client_env.update(WAYLAND_DISPLAY="wayland-andromeda", LOG_LOCK_INPUT="1")
                    lock_file = root / "andromeda" / "locked"

                    def client(argv, name, stdin=None):
                        out = (root / name).open("w")
                        clients.append(subprocess.Popen(argv, env=client_env, stdin=stdin, stdout=out, stderr=out))
                        return root / name

                    lock_log = client([os.environ["ANDROMEDA_LOCK_CLIENT"]], "lock-1.log", subprocess.PIPE)
                    await_line(lock_log, "locked", 5)
                    assert lock_file.exists(), "a confirmed lock wrote no lock file"
                    os.kill(processes[0].pid, signal.SIGSEGV)
                    processes[0].wait(timeout=10)
                    processes.append(start(log))
                    await_notify(notify, b"READY=1", 10)
                    shell_log = client([os.environ["ANDROMEDA_DESKTOP_CLIENT"], "shell-events"], "shell.log", subprocess.PIPE)
                    await_line(shell_log, "relock", 5)
                    # A headless seat has no keyboard until a virtual one arrives; without it no window gets
                    # keyboard-enter, and the focus checks below would pass for nothing.
                    keyboard = client([os.environ["ANDROMEDA_POINTER_CLIENT"], "1", "1", "keyboard-only", "mod", "none",
                                       "mark", "ready", "pause", "86400000"], "keyboard.log")
                    await_line(keyboard, "ready", 5)
                    window_log = client([os.environ["ANDROMEDA_SEAT_LOG_CLIENT"], "key-window"], "window.log")
                    capture_env = dict(client_env, CAPTURE_PIXELS="1")
                    capture_out = (root / "capture.log").open("w")
                    clients.append(subprocess.Popen([os.environ["ANDROMEDA_CAPTURE_CLIENT"]], env=capture_env,
                                                    stdout=capture_out, stderr=capture_out))
                    capture_log = root / "capture.log"
                    await_line(capture_log, "pixels", 5)
                    time.sleep(0.5)
                    assert "keyboard-enter" not in window_log.read_text(), "a window got focus in the restart lock"
                    frames = [line for line in capture_log.read_text().splitlines() if line.startswith("pixels")]
                    assert all("uniform=1" in line for line in frames), f"the restart lock showed content: {frames}"
                    relock = client([os.environ["ANDROMEDA_LOCK_CLIENT"]], "lock-2.log", subprocess.PIPE)
                    await_line(relock, "locked", 5)
                    await_line(shell_log, "recovered", 5)
                    clients[-1].stdin.write(b"unlock\n")
                    clients[-1].stdin.flush()
                    await_line(relock, "unlocked", 5)
                    await_line(window_log, "keyboard-enter", 5)
                    assert not lock_file.exists(), "the unlock left the lock file"
                    # The control: after the unlock the same capture sees the window, so uniform=1 above meant blank.
                    deadline = time.monotonic() + 5
                    while not capture_log.read_text().splitlines()[-1:] or "uniform=0" not in capture_log.read_text():
                        assert time.monotonic() < deadline, "the capture never showed the window after the unlock"
                        time.sleep(0.05)
                    # The escape must not fire once a lock client took over.
                    time.sleep(1.5)
                    assert processes[1].poll() is None, "the restart-lock escape ended an unlocked session"

                    # Escape: with no lock client after a locked crash the session ends in about 10 s, and unlocked.
                    for c in clients:
                        c.kill()
                    lock_file.touch()
                    os.kill(processes[1].pid, signal.SIGSEGV)
                    processes[1].wait(timeout=10)
                    processes.append(start(log))
                    await_notify(notify, b"READY=1", 10)
                    assert processes[2].wait(timeout=6) == 0, "the restart lock did not end the session cleanly"
                    assert not lock_file.exists(), "the escape left the lock file"
            except BaseException:
                log.seek(0)
                print(log.read()[-6000:], file=sys.stderr)
                raise
            finally:
                for c in clients:
                    if c.poll() is None:
                        c.kill()
                        c.wait(timeout=5)
                for process in processes:
                    if process.poll() is None:
                        os.killpg(process.pid, signal.SIGTERM)
                        try:
                            process.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            os.killpg(process.pid, signal.SIGKILL)
                            process.wait(timeout=5)
        print({"socket": "a crashed compositor restarts on the same socket; a client from the gap is served",
               "lock": "a crash while locked restarts into a blank lock that only a lock client ends"}[mode])


if __name__ == "__main__":
    main(Path(sys.argv[1]).resolve(), sys.argv[2])
