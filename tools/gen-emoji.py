#!/usr/bin/env python3
"""Generate shell/assets/emoji.json from Unicode emoji-test.txt and CLDR English annotations.

Usage: gen-emoji.py EMOJI_TEST.txt ANNOTATIONS.json ANNOTATIONS_DERIVED.json FONT.ttf OUT.json

Sources: https://unicode.org/Public/emoji/latest/emoji-test.txt and unicode-org/cldr-json
(cldr-annotations-full, cldr-annotations-derived-full, en). Only fully-qualified emoji that FONT draws as one glyph
are kept, so the picker never shows a missing-glyph box. Skin-tone variants go into the base entry's "tones" list
(light, medium-light, medium, medium-dark, dark) instead of separate entries.
"""
import json
import re
import sys

from fontTools.ttLib import TTFont

GROUPS = {
    "Smileys & Emotion": "smileys",
    "People & Body": "people",
    "Animals & Nature": "animals",
    "Food & Drink": "food",
    "Travel & Places": "travel",
    "Activities": "activity",
    "Objects": "objects",
    "Symbols": "symbols",
    "Flags": "flags",
}
TONES = ["1F3FB", "1F3FC", "1F3FD", "1F3FE", "1F3FF"]
LINE = re.compile(r"^([0-9A-F ]+?)\s*;\s*fully-qualified\s*#\s*\S+\s+E([\d.]+)\s+(.+)$")


def font_draws(font, cmap, ligatures, codepoints):
    glyphs = [cmap.get(cp) for cp in codepoints if cp not in (0xFE0F, 0x200D)]
    if any(g is None for g in glyphs):
        return False
    if len(glyphs) == 1:
        return True
    # Noto builds sequences as GSUB ligatures over the glyphs without FE0F; ZWJ is a glyph too.
    full = [cmap.get(cp) for cp in codepoints if cp != 0xFE0F]
    for seq in (full, glyphs):
        first, rest = seq[0], tuple(seq[1:])
        if rest in ligatures.get(first, ()):
            return True
    return False


def load_ligatures(font):
    ligatures = {}
    if "GSUB" not in font:
        return ligatures
    for lookup in font["GSUB"].table.LookupList.Lookup:
        for sub in lookup.SubTable:
            if sub.LookupType == 7:
                sub = sub.ExtSubTable
            if getattr(sub, "LookupType", None) != 4:
                continue
            for first, ligs in sub.ligatures.items():
                ligatures.setdefault(first, set()).update(tuple(l.Component) for l in ligs)
    return ligatures


def main():
    test_path, ann_path, derived_path, font_path, out_path = sys.argv[1:6]
    ann = json.load(open(ann_path))["annotations"]["annotations"]
    ann.update(json.load(open(derived_path))["annotationsDerived"]["annotations"])
    font = TTFont(font_path, lazy=True)
    cmap = font.getBestCmap()
    ligatures = load_ligatures(font)

    entries, by_emoji = [], {}
    group = subgroup = None
    skipped = 0
    for line in open(test_path, encoding="utf-8"):
        if line.startswith("# group:"):
            group = GROUPS.get(line.split(":", 1)[1].strip())
            continue
        if line.startswith("# subgroup:"):
            subgroup = line.split(":", 1)[1].strip()
            continue
        m = LINE.match(line.strip())
        if not m or group is None:
            continue
        hexes = m.group(1).split()
        codepoints = [int(h, 16) for h in hexes]
        emoji = "".join(chr(c) for c in codepoints)
        if not font_draws(font, cmap, ligatures, codepoints):
            skipped += 1
            continue
        tone_hexes = [h for h in hexes if h in TONES]
        if len(tone_hexes) == 1:
            base = "".join(chr(int(h, 16)) for h in hexes if h not in TONES)
            parent = by_emoji.get(base) or by_emoji.get(base + "️")
            if parent is not None:
                parent.setdefault("tones", [None] * 5)[TONES.index(tone_hexes[0])] = emoji
                continue
        if tone_hexes:
            continue  # multi-person mixed tones: keep the toneless base only
        key = emoji.replace("️", "")
        a = ann.get(emoji) or ann.get(key) or {}
        name = (a.get("tts") or [m.group(3)])[0]
        keywords = [k for k in a.get("default", []) if k.lower() != name.lower()]
        entry = {"emoji": emoji, "name": name, "keywords": keywords, "category": group, "subgroup": subgroup,
                 "version": m.group(2)}
        entries.append(entry)
        by_emoji[emoji] = entry
    for e in entries:
        if "tones" in e and None in e["tones"]:
            del e["tones"]  # a partial tone set would show empty dots
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(entries, f, ensure_ascii=False, separators=(",", ":"))
        f.write("\n")
    toned = sum(1 for e in entries if "tones" in e)
    print(f"{len(entries)} emoji ({toned} with 5 skin tones), {skipped} skipped: not in {font_path}")


if __name__ == "__main__":
    main()
