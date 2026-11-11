"""A log-out keeps the window list in session.json (docs/resilience.md R6). The compositor runs in a delegated cgroup
named andromeda.service, so session-quit takes the real path: it starts andromeda-shutdown.target (a stub here), the
user manager stops the apps first, and only then sends SIGTERM to the compositor."""

import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

HELPER = r'''#!PYTHON
import os, sys
from pathlib import Path
name = Path(sys.argv[0]).name
if name == "systemctl" and "andromeda-shutdown.target" in sys.argv:
    (Path(os.environ["LOGOUT_ROOT"]) / "shutdown").touch()
if name == "systemd-run":
    sys.exit(1)
'''

# Runs inside the delegated scope: move into a child cgroup that the unit parser reads as andromeda.service.
LAUNCH = ('cg=/sys/fs/cgroup$(cut -d: -f3 /proc/self/cgroup); mkdir "$cg/andromeda.service" && '
          'echo $$ > "$cg/andromeda.service/cgroup.procs" && export SYSTEMD_EXEC_PID=$$ && exec "$@"')


def wait_for(predicate, message, timeout=5):
    deadline = time.monotonic() + timeout
    while not predicate():
        assert time.monotonic() < deadline, message
        time.sleep(0.05)


def main(binary):
    with tempfile.TemporaryDirectory(prefix="um-logout.") as directory:
        root = Path(directory)
        (root / "bin").mkdir()
        helper = root / "helper"
        helper.write_text(HELPER.replace("#!PYTHON", "#!" + sys.executable, 1))
        helper.chmod(0o700)
        for name in ("systemctl", "dbus-update-activation-environment", "systemd-run"):
            (root / "bin" / name).symlink_to(helper)
        config = root / "config.toml"
        config.write_text('[general]\nxwayland = false\nautostart = []\n')

        environment = {k: v for k, v in os.environ.items()
                       if k not in ("WAYLAND_DISPLAY", "WAYLAND_SOCKET", "DISPLAY", "ANDROMEDA_SOCKET",
                                    "SYSTEMD_EXEC_PID", "LISTEN_PID", "LISTEN_FDS", "LISTEN_FDNAMES", "NOTIFY_SOCKET")}
        environment.update(
            PATH=str(root / "bin") + ":" + os.environ["PATH"],
            XDG_RUNTIME_DIR=str(root),
            XDG_CACHE_HOME=str(root / "cache"),
            XDG_STATE_HOME=str(root / "state"),
            DBUS_SESSION_BUS_ADDRESS="unix:path=" + str(root / "no-session-bus"),
            SYSTEMD_BUS_ADDRESS="unix:path=" + str(root / "no-manager-bus"),
            DBUS_SYSTEM_BUS_ADDRESS="unix:path=" + str(root / "no-system-bus"),
            LOGOUT_ROOT=str(root),
            WLR_BACKENDS="headless",
            WLR_HEADLESS_OUTPUTS="1",
            WLR_LIBINPUT_NO_DEVICES="1",
        )
        # systemd-run needs the real user manager (the harness hides its bus); env -i sets the test environment.
        command = ["systemd-run", "--user", "--scope", "-q", "--expand-environment=no", "-p", "Delegate=yes", "env", "-i",
                   *(f"{k}={v}" for k, v in environment.items()),
                   "/bin/sh", "-c", LAUNCH, "session-logout", str(binary), "-c", str(config)]
        ipc_env = dict(environment, ANDROMEDA_SOCKET=str(root / "andromeda-wayland-0.sock"))

        def ipc(*args):
            return subprocess.run([str(binary), *args], env=ipc_env, capture_output=True, text=True, timeout=5)

        def windows():
            return json.loads(ipc("windows", "--json").stdout or "[]")

        saved = root / "state" / "andromeda" / "session.json"

        def saved_ids():
            return sorted(w["app_id"] for w in json.loads(saved.read_text())["windows"]) if saved.exists() else None

        clients = {}
        with (root / "compositor.log").open("w+") as log:
            compositor = subprocess.Popen(command, stdout=log, stderr=log, start_new_session=True,
                                          env=dict(os.environ, XDG_RUNTIME_DIR=f"/run/user/{os.getuid()}"))
            try:
                wait_for(lambda: ipc("windows").returncode == 0, "the compositor never answered IPC", 10)
                window_env = dict(environment, WAYLAND_DISPLAY="wayland-0")
                for app_id in ("logout.a", "logout.b"):
                    out = (root / f"{app_id}.log").open("w")
                    clients[app_id] = subprocess.Popen([os.environ["ANDROMEDA_SEAT_LOG_CLIENT"], "key-window"],
                                                       env=dict(window_env, SEAT_LOG_APP_ID=app_id),
                                                       stdout=out, stderr=out)
                wait_for(lambda: saved_ids() == ["logout.a", "logout.b"], "the mapped windows never reached session.json")

                # The control: a window the user closes while the session runs leaves session.json.
                clients.pop("logout.a").kill()
                wait_for(lambda: saved_ids() == ["logout.b"], "a closed window stayed in session.json")

                ipc("msg", "session-quit:skip-confirmation")
                wait_for(lambda: (root / "shutdown").exists(), "session-quit never started andromeda-shutdown.target")
                # The user manager stops the app scopes before andromeda.service: the window unmaps first.
                clients.pop("logout.b").kill()
                wait_for(lambda: windows() == [], "the app window stayed mapped")
                # A save runs at most one second after a change; give it that second before SIGTERM.
                time.sleep(1.5)
                os.kill(compositor.pid, signal.SIGTERM)
                compositor.wait(timeout=10)
                assert saved_ids() == ["logout.b"], f"the log-out emptied session.json: {saved.read_text()}"
            except BaseException:
                log.seek(0)
                print(log.read()[-6000:], file=sys.stderr)
                raise
            finally:
                for c in clients.values():
                    c.kill()
                    c.wait(timeout=5)
                if compositor.poll() is None:
                    os.killpg(compositor.pid, signal.SIGKILL)
                    compositor.wait(timeout=5)
        print("a log-out keeps the window list in session.json; a close while the session runs updates it")


if __name__ == "__main__":
    main(Path(sys.argv[1]).resolve())
