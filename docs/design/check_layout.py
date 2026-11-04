#!/usr/bin/env python3
"""Render an HTML mockup or specimen headless and check its layout against the rulebook tokens.

Usage: check_layout.py FILE.html [--width 2064] [--height 4000]
Exit 1 when any element breaks a rule; prints one line per violation.
"""
import argparse
import html
import json
import pathlib
import re
import subprocess
import sys
import tempfile
import tomllib

ROOT = pathlib.Path(__file__).resolve().parents[2]
T = tomllib.loads((ROOT / "protocol/design-tokens.toml").read_text())
C = T["component"]

# selector -> allowed heights (px); widths where fixed. Values come from the token file.
RULES = {
    ".side .srow:not(:has(.av))": {"h": [C["sidebar"]["row"]]},
    ".side": {"w": [C["sidebar"]["width"]]},
    ".lrow": {"h": [T["size"]["row"], T["size"]["row_two"]]},
    ".side .field, .field.s": {"h": [T["size"]["control_m"]]},
    ".tbar": {"h": [T["size"]["control_l"]]},
    ".tog": {"h": [C["toggle"]["height"]], "w": [C["toggle"]["width"]]},
    ".kc, .kc2": {"h": [20.0]},
    ".seg": {"h": [T["size"]["control_m"], T["size"]["control_s"]]},
    ".pop": {"h": [T["size"]["control_s"], T["size"]["control_m"]]},
    ".sheet": {"w": [C["sheet"]["width"]]},
    ".sheet .tile:not(.span2)": {"h": [C["tile"]["height"]], "w": [C["sheet"]["column"]]},
    ".sheet .tile.span2": {"h": [C["tile"]["height"]]},
    ".sheet .ri": {"h": [C["round_icon"]["size"]], "w": [C["round_icon"]["size"]]},
    ".sheet .pill": {"h": [C["slider"]["height"]]},
    ".sheet .media": {"h": [C["media"]["height"]]},
    ".sheet .media .art": {"h": [C["media"]["art"]], "w": [C["media"]["art"]]},
    ".island": {"h": [C["island"]["height"], 40.0, 56.0, 88.0]},  # rest 32; OSD, live activity, banner/media (9.2)
    ".dock": {"h": [C["dock"]["icon"] + 2 * C["dock"]["padding"]]},
    ".dock .app": {"h": [C["dock"]["icon"]], "w": [C["dock"]["icon"]]},
    ".ws > span": {"h": [24.0]},
}

AUDIT = r"""
<script>
addEventListener('load', () => {
  const rules = %RULES%;
  const out = [];
  const name = el => (el.className && typeof el.className === 'string' ? '.' + el.className.trim().split(/\s+/).join('.') : el.tagName.toLowerCase()) + (el.textContent ? ' "' + el.textContent.trim().slice(0, 24) + '"' : '');
  for (const [sel, r] of Object.entries(rules)) {
    for (const el of document.querySelectorAll(sel)) {
      const b = {width: el.offsetWidth, height: el.offsetHeight};
      if (!b.width || !b.height) continue;
      if (r.h && !r.h.some(v => Math.abs(b.height - v) < 0.6)) out.push(`${sel}: ${name(el)} height ${b.height.toFixed(1)}, want ${r.h.join('|')}`);
      if (r.w && !r.w.some(v => Math.abs(b.width - v) < 0.6)) out.push(`${sel}: ${name(el)} width ${b.width.toFixed(1)}, want ${r.w.join('|')}`);
    }
  }
  // Shrunk boxes: a flex child drawn smaller than its own content.
  for (const el of document.querySelectorAll('.side *, .content *')) {
    const cs = getComputedStyle(el);
    if (cs.display === 'none' || el.closest('svg')) continue;
    if (el.scrollHeight > el.clientHeight + 1 && cs.overflow === 'visible' && el.clientHeight > 0) out.push(`shrunk: ${name(el)} content ${el.scrollHeight} in ${el.clientHeight}`);
  }
  // Inset symmetry: a key cap or chip at the end of a field keeps the same gap to the top, bottom and end.
  for (const el of document.querySelectorAll('.field .kc')) {
    const f = el.parentElement, k = getComputedStyle(f);
    const top = el.offsetTop - f.offsetTop, bottom = f.offsetHeight - top - el.offsetHeight, end = f.offsetWidth - (el.offsetLeft - f.offsetLeft) - el.offsetWidth;
    if (Math.max(top, bottom, end) - Math.min(top, bottom, end) > 1) out.push(`inset: ${name(el)} top ${top.toFixed(1)} bottom ${bottom.toFixed(1)} end ${end.toFixed(1)}`);
  }
  const px = v => parseFloat(v) || 0;
  const scope = [...document.querySelectorAll('.win, .stage .sheet, .island, .dock')];
  const inScope = el => scope.some(w => w.contains(el));
  const rad = el => { const r = px(getComputedStyle(el).borderTopLeftRadius); return r >= el.offsetHeight / 2 - 0.5 ? -1 : r; }; // -1 = capsule/circle
  const abs = el => { let x = 0, y = 0, e = el; while (e) { x += e.offsetLeft; y += e.offsetTop; e = e.offsetParent; } return {x, y}; };
  // R7 concentric radius: a rounded box inside a rounded box, near its corner, has radius = outer - inset (min 4).
  for (const el of document.querySelectorAll('.win *, .sheet *, .island *, .dock *')) {
    if (el.closest('svg') || !el.offsetHeight) continue;
    const r = rad(el); if (r <= 0 || el.matches('.app, .app *')) continue; // app icons keep their own shape
    let o = el.parentElement;
    while (o && !(rad(o) > 0)) o = o.parentElement;
    if (!o || !inScope(o)) continue;
    const a = abs(el), b = abs(o), dx = a.x - b.x, dy = a.y - b.y, R = rad(o);
    if (dx >= R || dy >= R) continue;
    const want = Math.max(4, R - Math.max(dx, dy));
    if (Math.abs(r - want) > 0.6) out.push(`R7: ${name(el)} radius ${r} at inset ${Math.max(dx, dy)} in radius ${R}, want ${want}`);
  }
  // R1 grid: boxes inside windows sit on the 2 px grid (size and offset to the window).
  for (const w of document.querySelectorAll('.win, .sheet, .island, .dock')) {
    const W = abs(w);
    for (const el of w.querySelectorAll('div, span')) {
      if (el.closest('svg') || !el.offsetHeight || getComputedStyle(el).display === 'inline') continue;
      if (el.children.length === 0 && el.textContent.trim() && !el.className) continue;
      if (el.offsetHeight <= 8) continue; // dots and marks centre on whole pixels
      const a = abs(el), vals = [el.offsetHeight, a.y - W.y]; // widths and x follow text and flex shares
      if (vals.some(v => Math.abs(v / 2 - Math.round(v / 2)) > 0.01)) out.push(`R1: ${name(el)} box ${vals.join(',')} (h,y) off the 2 grid`);
    }
  }
  // R6 one edge: in a card, row labels and the section header above it start on one line.
  for (const c of document.querySelectorAll('.gcard')) {
    const xs = [...c.querySelectorAll(':scope > .lrow > .grow:first-child')].map(e => Math.round(abs(e).x));
    const h = c.previousElementSibling && c.previousElementSibling.classList.contains('shead') ? Math.round(abs(c.previousElementSibling).x + px(getComputedStyle(c.previousElementSibling).paddingLeft)) : null;
    const all = h === null ? xs : xs.concat([h]);
    if (new Set(all).size > 1) out.push(`R6: card "${c.textContent.trim().slice(0, 24)}" text edges ${[...new Set(all)].join(',')}`);
  }
  const pre = document.createElement('pre'); pre.id = '__audit'; pre.textContent = JSON.stringify(out);
  document.body.appendChild(pre);
});
</script>
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--width", type=int, default=2064)
    ap.add_argument("--height", type=int, default=4000)
    a = ap.parse_args()
    src = pathlib.Path(a.file).resolve()
    page = src.read_text().replace("</body>", AUDIT.replace("%RULES%", json.dumps(RULES)) + "</body>")
    with tempfile.NamedTemporaryFile("w", suffix=".html", dir=src.parent, delete=False) as tmp:
        tmp.write(page)
    try:
        dom = subprocess.run(
            ["google-chrome-stable", "--headless=new", "--disable-gpu", f"--window-size={a.width},{a.height}",
             "--virtual-time-budget=3000", "--dump-dom", f"file://{tmp.name}"],
            capture_output=True, text=True, timeout=60).stdout
    finally:
        pathlib.Path(tmp.name).unlink()
    m = re.search(r'<pre id="__audit">(.*?)</pre>', dom, re.S)
    if not m:
        print("audit did not run", file=sys.stderr)
        return 2
    found = json.loads(html.unescape(m.group(1)))
    for line in sorted(set(found)):
        print(line)
    print(f"{len(set(found))} violations", file=sys.stderr)
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
