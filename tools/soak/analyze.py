#!/usr/bin/env python3
"""Pull a soak run from the laptop and write .agent/soak/report-<start>.md.

Usage: python3 -I tools/soak/analyze.py <start> [host]
"""
import csv
import os
import statistics
import subprocess
import sys

WARMUP_S = 600
MEM_FLAG_MB_H = 2.0
IDLE_CPU_PCT = 1.0
IDLE_WAKE_S = 20.0

repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
start = sys.argv[1]
host = sys.argv[2] if len(sys.argv) > 2 else "192.168.101.8"
local = os.path.join(repo, ".agent", "soak", start)
os.makedirs(local, exist_ok=True)
for f in ("samples.tsv", "workload.log"):
    subprocess.run(["flock", "/tmp/laptop.lock", "scp", "-q", f"{host}:/var/tmp/andromeda-soak/{start}/{f}", local], check=f == "samples.tsv")

work = []
open_t = None
wl = os.path.join(local, "workload.log")
events = []
if os.path.exists(wl):
    for line in open(wl):
        parts = line.rstrip("\n").split("\t", 1)
        if len(parts) < 2:
            continue
        t, what = int(parts[0]), parts[1]
        if what == "phase work":
            open_t = t
        elif what == "phase idle" and open_t is not None:
            work.append((open_t, t))
            open_t = None
        elif what.startswith("event"):
            events.append(f"{t} {what[6:]}")
if open_t is not None:
    work.append((open_t, 1 << 62))


def in_work(t0, t1):
    return any(a < t1 and b > t0 for a, b in work)


def num(v):
    return float(v) if v not in ("", None) else None


def slope_per_h(pts):
    if len(pts) < 3:
        return None
    xs = [t / 3600 for t, _ in pts]
    ys = [v for _, v in pts]
    mx, my = statistics.fmean(xs), statistics.fmean(ys)
    den = sum((x - mx) ** 2 for x in xs)
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / den if den else None


def p95(v):
    return sorted(v)[min(len(v) - 1, int(0.95 * len(v)))] if v else 0.0


procs, machine, restarts = {}, [], []
with open(os.path.join(local, "samples.tsv")) as fh:
    for r in csv.DictReader(fh, delimiter="\t"):
        t = int(r["t"])
        if r["kind"] == "machine":
            machine.append((t, r))
        elif r["kind"] == "proc":
            key = (r["name"], r["pid"])
            procs.setdefault(key, []).append((t, r))
            if r["note"] == "restart":
                restarts.append(f"{t} {r['name']} new pid {r['pid']}")
        elif r["kind"] == "event":
            restarts.append(f"{t} {r['name']} pid {r['pid']} exited")

t_first = min(t for s in procs.values() for t, _ in s)
t_last = max(t for s in procs.values() for t, _ in s)
rows, flags = [], []
for (name, pid), s in procs.items():
    idle_cpu, idle_wk, work_cpu, work_wk = [], [], [], []
    for (ta, _), (tb, r) in zip(s, s[1:]):
        dt = tb - ta
        c, w = num(r["cpu_ms"]), num(r["wakeups"])
        if c is None or dt <= 0:
            continue
        cpu, wk = c / (dt * 10.0), max(w or 0.0, 0.0) / dt
        (work_cpu if in_work(ta, tb) else idle_cpu).append(cpu)
        (work_wk if in_work(ta, tb) else idle_wk).append(wk)
    late = [(t, r) for t, r in s if t >= t_first + WARMUP_S]
    sl = {}
    for col, scale in (("pss_kb", 1024), ("pdirty_kb", 1024), ("gpu_kb", 1024), ("fds", 1), ("threads", 1)):
        pts = [(t, num(r[col]) / scale) for t, r in late if num(r[col]) is not None]
        sl[col] = (slope_per_h(pts), pts[0][1] if pts else None, pts[-1][1] if pts else None)
    last = s[-1][1]
    tag = f"{name}({pid})"
    for col in ("pss_kb", "pdirty_kb", "gpu_kb"):
        v = sl[col][0]
        if v is not None and v > MEM_FLAG_MB_H:
            flags.append(f"{tag}: {col[:-3]} +{v:.1f} MB/h ({sl[col][1]:.1f} -> {sl[col][2]:.1f} MB)")
    for col in ("fds", "threads"):
        v, a, b = sl[col]
        if v is not None and b is not None and b > a and v > 0:
            flags.append(f"{tag}: {col} {a:.0f} -> {b:.0f} ({v:+.1f}/h)")
    ic, iw = statistics.fmean(idle_cpu) if idle_cpu else 0.0, statistics.fmean(idle_wk) if idle_wk else 0.0
    if ic > IDLE_CPU_PCT:
        flags.append(f"{tag}: idle CPU {ic:.2f} %")
    if iw > IDLE_WAKE_S:
        flags.append(f"{tag}: idle wakeups {iw:.1f}/s")
    rows.append((
        statistics.fmean(idle_cpu + work_cpu) if idle_cpu + work_cpu else 0.0, tag,
        f"| {tag} | {ic:.2f}/{p95(idle_cpu):.2f} | {statistics.fmean(work_cpu) if work_cpu else 0:.2f}/{p95(work_cpu):.2f} "
        f"| {iw:.1f}/{statistics.fmean(work_wk) if work_wk else 0:.1f} | {num(last['pss_kb']) / 1024:.0f} "
        f"| {num(last['gpu_kb']) / 1024:.0f} | {last['fds']}/{last['threads']} | "
        + " | ".join("-" if sl[c][0] is None else f"{sl[c][0]:+.1f}" for c in ("pss_kb", "pdirty_kb", "gpu_kb")) + " |",
    ))

rows.sort(reverse=True)
out = [
    f"# Soak report {start}",
    "",
    f"Span {(t_last - t_first) / 60:.0f} min, {len(machine)} ticks, warm-up {WARMUP_S // 60} min excluded from slopes, "
    f"{len(work)} work phases. CPU in % of one core, wakeups per s, memory in MB, slopes in MB/h.",
    "",
    "## Flags",
    "",
]
out += [f"- {f}" for f in flags[:15]] or ["- none"]
if len(flags) > 15:
    out.append(f"- ... {len(flags) - 15} more")
out += [
    "",
    "## Processes (top 20 by CPU)",
    "",
    "| process | idle CPU mean/p95 | work CPU mean/p95 | wakeups idle/work | Pss | GPU | fds/thr | Pss/h | Dirty/h | GPU/h |",
    "|---|---|---|---|---|---|---|---|---|---|",
]
out += [r[2] for r in rows[:20]]
pw = [num(r["power_w"]) for _, r in machine if num(r["power_w"])]
ma = [(t, num(r["memavail_kb"]) / 1024) for t, r in machine if t >= t_first + WARMUP_S]
temps = [num(r["temp_c"]) for _, r in machine if num(r["temp_c"])]
ms = slope_per_h(ma)
out += [
    "",
    "## Machine",
    "",
    f"- battery power: {f'{statistics.fmean(pw):.2f} W mean, p95 {p95(pw):.2f} W' if pw else 'n/a (on AC)'}; "
    f"load1 mean {statistics.fmean(num(r['load1']) for _, r in machine):.2f}",
    f"- MemAvailable slope {'-' if ms is None else f'{ms:+.1f} MB/h'}; pkg temp mean {statistics.fmean(temps) if temps else 0:.0f} C",
    "",
    "## Restarts and workload events",
    "",
]
out += [f"- {e}" for e in (restarts + events)[:8]] or ["- none"]
path = os.path.join(repo, ".agent", "soak", f"report-{start}.md")
with open(path, "w") as fh:
    fh.write("\n".join(out[:60]) + "\n")
print(path)
