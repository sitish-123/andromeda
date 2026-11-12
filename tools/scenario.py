#!/usr/bin/env python3
"""Run one scenario against a contained headless compositor.

Usage: tools/scenario.py <scenario.toml> [--binary <andromeda>]

A scenario file:
  outputs = 1                       # headless outputs (optional)
  config = '''...'''                # TOML appended to the harness default config (optional)
  [[step]]
  act = "workspace.switch"
  args = { index = 2 }
  [[expect]]
  state = "workspaces"              # any `andromeda <state> --json` query
  select = { focused = true }       # optional: pick the one list item whose fields equal these
  match = { index = 2 }             # subset of the state that must be equal

Prints one pass line, or the failing expectation with only the differing fields.
"""
import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_CONFIG = "[general]\nxwayland = false\nautostart = []\n"


class Instance:
    def __init__(self, binary, outputs, extra_config):
        self.binary = binary
        # sockaddr_un caps paths at 108 bytes; the compositor appends its socket name to XDG_RUNTIME_DIR.
        self.dir = tempfile.mkdtemp(prefix="scn.", dir="/tmp")
        self.log = os.path.join(self.dir, "compositor.log")
        config = os.path.join(self.dir, "config.toml")
        with open(config, "w") as f:
            f.write(DEFAULT_CONFIG + (extra_config or ""))
        env = {k: v for k, v in os.environ.items() if k not in ("WAYLAND_DISPLAY", "DISPLAY", "DBUS_SESSION_BUS_ADDRESS")}
        env.update(
            DBUS_SYSTEM_BUS_ADDRESS=f"unix:path={self.dir}/no-system-bus",
            XDG_RUNTIME_DIR=self.dir,
            XDG_DATA_HOME=f"{self.dir}/xdg-data",
            XDG_STATE_HOME=f"{self.dir}/xdg-state",
            XDG_CONFIG_HOME=f"{self.dir}/xdg-config",
            WLR_BACKENDS="headless",
            WLR_LIBINPUT_NO_DEVICES="1",
            WLR_HEADLESS_OUTPUTS=str(outputs),
        )
        with open(self.log, "wb") as log:
            self.proc = subprocess.Popen([binary, "-c", config], stdout=log, stderr=subprocess.STDOUT, env=env, start_new_session=True)
        self.env = dict(os.environ, ANDROMEDA_SOCKET=f"{self.dir}/andromeda-wayland-0.sock")
        wait = subprocess.run(
            [f"{ROOT}/tools/wait.sh", "--log", self.log, "--match", "IPC listening on", "--timeout", "10"],
            capture_output=True, text=True,
        )
        if wait.returncode != 0:
            tail = open(self.log, errors="replace").read()[-2000:]
            self.close()
            raise RuntimeError(f"compositor did not boot: {wait.stdout.strip()}\n{tail}")

    def cli(self, *args):
        r = subprocess.run([self.binary, *args], env=self.env, capture_output=True, text=True, timeout=20)
        if r.returncode != 0:
            raise RuntimeError(f"andromeda {' '.join(args)}: {(r.stderr or r.stdout).strip()}")
        return r.stdout

    def close(self, keep=False):
        if self.proc.poll() is None:
            os.killpg(self.proc.pid, signal.SIGTERM)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
                self.proc.wait()
        if not keep:
            shutil.rmtree(self.dir, ignore_errors=True)


def act(inst, name, args):
    """The one seam to the action layer: switch to `andromeda act` once the broker ships."""
    action = name.replace(".", "-")
    if args:
        action += ":" + "/".join(str(v) for v in args.values())
    inst.cli("msg", action)


def state(inst, name):
    return json.loads(inst.cli(name, "--json"))


def diff(expected, actual, path=""):
    """The differing leaves of `expected` (a subset) against `actual`, as (path, expected, actual)."""
    if isinstance(expected, dict) and isinstance(actual, dict):
        out = []
        for k, v in expected.items():
            sub = f"{path}.{k}" if path else k
            out += diff(v, actual[k], sub) if k in actual else [(sub, v, "<missing>")]
        return out
    if isinstance(expected, list) and isinstance(actual, list):
        if len(expected) != len(actual):
            return [(f"{path}.length", len(expected), len(actual))]
        return [d for i, (e, a) in enumerate(zip(expected, actual)) for d in diff(e, a, f"{path}[{i}]")]
    return [] if expected == actual else [(path or "value", expected, actual)]


def check(inst, i, exp):
    value = state(inst, exp["state"])
    where = exp.get("select")
    label = f"expect[{i}] {exp['state']}"
    if where is not None:
        label += " " + ",".join(f"{k}={json.dumps(v)}" for k, v in where.items())
        picked = [item for item in value if not diff(where, item)]
        if len(picked) != 1:
            return label, [("select", "1 item", f"{len(picked)} items: {json.dumps(value)}")]
        value = picked[0]
    return label, diff(exp.get("match", {}), value)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("scenario")
    p.add_argument("--binary", default=os.environ.get("ANDROMEDA_BIN", f"{ROOT}/compositor/build-debug/andromeda"))
    a = p.parse_args()
    with open(a.scenario, "rb") as f:
        scn = tomllib.load(f)
    name = os.path.relpath(a.scenario, ROOT) if os.path.abspath(a.scenario).startswith(ROOT + "/") else a.scenario
    try:
        inst = Instance(a.binary, scn.get("outputs", 1), scn.get("config"))
    except RuntimeError as e:
        print(f"scenario {name}: FAILED {e}")
        return 1
    failed = False
    try:
        for step in scn.get("step", []):
            act(inst, step["act"], step.get("args", {}))
        inst.cli("settle")
        for i, exp in enumerate(scn.get("expect", [])):
            label, bad = check(inst, i, exp)
            if bad:
                print(f"scenario {name}: FAILED {label}")
                for path, e, act_v in bad:
                    print(f"  {path}: expected {json.dumps(e)}, actual {json.dumps(act_v)}")
                print(f"  log: {inst.log}")
                failed = True
                return 1
    except RuntimeError as e:
        print(f"scenario {name}: FAILED {e}\n  log: {inst.log}")
        failed = True
        return 1
    finally:
        inst.close(keep=failed)
    print(f"scenario {name}: ok ({len(scn.get('step', []))} steps, {len(scn.get('expect', []))} expects)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
