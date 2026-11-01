#!/usr/bin/env python3
"""Usage: gen-design-tokens.py TOKENS.toml OUT... -- each OUT by extension: .h namespace Tokens (see the header of
TOKENS.toml), .css custom properties for the design reference (docs/design/reference.html), .glsl #defines of the
radius tokens for compositor shaders, *-gtk3-light.css / *-gtk3-dark.css / *-gtk4-light.css / *-gtk4-dark.css GTK
colors for one scheme (the shell's runtime file imports one and overrides the accent), *-qt-light.conf /
*-qt-dark.conf qtct palettes (.colors also accepted), */fcitx5-light/theme.conf and */fcitx5-dark/theme.conf fcitx5 classic-ui themes (with
panel.svg and highlight.svg beside them), *-kde-light.colors / *-kde-dark.colors KDE color schemes."""
import math
import re
import sys
import tomllib

COLOR = re.compile(r"#[0-9a-fA-F]{8}")
EXPR = re.compile(r"=\s*([a-z0-9_.]+)((?:\s*[-+]\s*[a-z0-9_.]+)*)")


def lookup(tokens, dotted):
    node = tokens
    for part in dotted.split("."):
        node = node[part]
    return node


def resolve(tokens, table=None, path=()):
    """Replaces every "= a.b - c.d" string with its value; a reference may itself be an expression."""
    table = tokens if table is None else table
    for key, value in table.items():
        if isinstance(value, dict):
            resolve(tokens, value, path + (key,))
        elif isinstance(value, str) and value.startswith("="):
            table[key] = evaluate(tokens, value, path + (key,), set())


def evaluate(tokens, expr, where, seen):
    m = EXPR.fullmatch(expr.strip())
    if not m or ".".join(where) in seen:
        sys.exit(f"{'.'.join(where)}: bad or circular expression {expr!r}")
    seen.add(".".join(where))
    terms = [("+", m.group(1))] + re.findall(r"([-+])\s*([a-z0-9_.]+)", m.group(2))
    total = 0.0
    for sign, ref in terms:
        value = lookup(tokens, ref)
        if isinstance(value, str) and value.startswith("="):
            value = evaluate(tokens, value, tuple(ref.split(".")), seen)
        if not isinstance(value, (int, float)) or isinstance(value, bool):
            sys.exit(f"{'.'.join(where)}: {ref} is not a number")
        total += value if sign == "+" else -value
    return float(total)


def camel(parts):
    words = [w for p in parts for w in p.split("_")]
    return words[0] + "".join(w[:1].upper() + w[1:] for w in words[1:])


def pascal(parts):
    name = camel(parts)
    return name[:1].upper() + name[1:]


def scalar(path, value):
    if isinstance(value, bool):
        return "bool", "true" if value else "false"
    if isinstance(value, int):
        return "int", str(value)
    if isinstance(value, float):
        return "float", f"{value!r}F"
    if isinstance(value, str) and COLOR.fullmatch(value):
        return "std::uint32_t", "0x" + value[1:].lower() + "U"
    if isinstance(value, str):
        return "std::string_view", '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'
    sys.exit(f"{'.'.join(path)}: unsupported value {value!r}")


def is_struct_group(table):
    children = list(table.values())
    if len(children) < 2 or not all(isinstance(c, dict) for c in children):
        return False
    keys = [list(c.keys()) for c in children]
    return all(k == keys[0] for k in keys) and all(not isinstance(v, dict) for c in children for v in c.values())


KIND = {"bool": "b", "int": "i", "float": "f", "std::uint32_t": "c", "std::string_view": "s"}


def emit(table, path, out, slots, refresh):
    for key, value in table.items():
        sub = path + (key,)
        if isinstance(value, dict) and is_struct_group(value):
            first = next(iter(value.values()))
            name = pascal(sub)
            fields = [(camel((k,)), scalar(sub + (k,), v)[0]) for k, v in first.items()]
            out.append(f"  struct {name} {{")
            out += [f"    {kind} {field};" for field, kind in fields]
            out.append("  };")
            for child, values in value.items():
                literals = ", ".join(scalar(sub + (child, k), v)[1] for k, v in values.items())
                out.append(f"  inline TOKENS_CONST {name} {camel(sub + (child,))}{{{literals}}};")
                for k, v in values.items():
                    slots.append((".".join(sub + (child, k)), KIND[scalar((), v)[0]], f"&{camel(sub + (child,))}.{camel((k,))}", v))
                if all(isinstance(v, str) and COLOR.fullmatch(v) for v in values.values()):
                    pairs = ", ".join(f'{{"{k}", {scalar((), v)[1]}}}' for k, v in values.items())
                    out.append(f"  inline TOKENS_CONST std::array<std::pair<std::string_view, std::uint32_t>, {len(values)}> "
                               f"{camel(sub + (child, 'entries'))}{{{{{pairs}}}}};")
                    live = ", ".join(f'{{"{k}", {camel(sub + (child,))}.{camel((k,))}}}' for k in values)
                    refresh.append(f"    {camel(sub + (child, 'entries'))} = {{{{{live}}}}};")
        elif isinstance(value, dict):
            emit(value, sub, out, slots, refresh)
        else:
            kind, literal = scalar(sub, value)
            out.append(f"  inline TOKENS_CONST {kind} {camel(sub)} = {literal};")
            slots.append((".".join(sub), KIND[kind], f"&{camel(sub)}", value))


def default_number(v):
    if isinstance(v, bool):
        return "1" if v else "0"
    if isinstance(v, str):
        return str(int(v[1:], 16)) if COLOR.fullmatch(v) else "0"
    return repr(float(v))


def default_text(v):
    return scalar((), v)[1] if isinstance(v, str) and not COLOR.fullmatch(v) else '""'


PX_GROUPS = ("radius", "space", "component")
UNITLESS = ("weight", "line_height", "font_weight", "smoothing")


def css_value(path, value):
    if isinstance(value, str) and COLOR.fullmatch(value):
        r, g, b, a = (int(value[i:i + 2], 16) for i in (1, 3, 5, 7))
        return f"rgba({r}, {g}, {b}, {round(a / 255, 3)})"
    if isinstance(value, str):
        return f'"{value}"'
    px = (path[0] in PX_GROUPS or path[-1] == "size") and path[-1] not in UNITLESS
    return f"{value}px" if px else str(value)


def css_vars(table, path, out):
    for key, value in table.items():
        sub = path + (key,)
        if isinstance(value, dict):
            css_vars(value, sub, out)
        else:
            out.append(f"  --{'-'.join(sub).replace('_', '-')}: {css_value(sub, value)};")


def emit_css(tokens, out_path):
    schemes = tokens.get("scheme", {})
    rest = {k: v for k, v in tokens.items() if k != "scheme"}
    out = ["/* Generated from protocol/design-tokens.toml by protocol/gen-design-tokens.py. Do not edit. */", ":root {"]
    css_vars(rest, (), out)
    for name, t in tokens.get("type", {}).items():
        out.append(f"  --type-{name}: {t['weight']} {t['size']}px/{t['line_height']} var(--font-family);")
    smoothing = tokens.get("radius", {}).get("smoothing", 0.0)
    out.append(f"  --corner-extent: {round(1 + 0.88 * smoothing, 4)};")
    out.append(f"  --corner-k: {round(math.log2(2 + 5 * smoothing), 4)};")
    css_vars(schemes.get("light", {}), (), out)
    out += ["}", ".dark {"]
    css_vars(schemes.get("dark", {}), (), out)
    out += ["}", ""]
    with open(out_path, "w") as f:
        f.write("\n".join(out))


def emit_glsl(tokens, out_path):
    out = ["// Generated from protocol/design-tokens.toml by protocol/gen-design-tokens.py. Do not edit."]

    def walk(table, path):
        for key, value in table.items():
            if isinstance(value, dict):
                walk(value, path + (key,))
            elif isinstance(value, (int, float)) and not isinstance(value, bool):
                out.append(f"#define TOKEN_{'_'.join(path + (key,)).upper()} {float(value)!r}")

    # Only the radius group: shaders.c joins sources into fixed-size buffers.
    walk({"radius": tokens.get("radius", {})}, ())
    with open(out_path, "w") as f:
        f.write("\n".join(out) + "\n")


# GTK color name -> (scheme role, fallback role for its foreground). Each bg gets a matching fg.
GTK_SURFACES = {
    "window": "window", "view": "view", "headerbar": "header", "popover": "popover", "card": "card",
    "dialog": "window", "overview": "sidebar", "sidebar": "sidebar", "secondary_sidebar": "view",
}


def css_color(value):
    r, g, b, a = (int(value[i:i + 2], 16) for i in (1, 3, 5, 7))
    return f"#{value[1:7].lower()}" if a == 255 else f"rgba({r}, {g}, {b}, {round(a / 255, 3)})"


def emit_gtk(tokens, out_path, scheme, gtk4):
    t = tokens["scheme"][scheme]
    colors = {
        "accent_color": t["accent"], "accent_bg_color": t["accent"], "accent_fg_color": t["on_accent"],
        "destructive_bg_color": t["danger"], "destructive_fg_color": t["on_accent"], "destructive_color": t["danger"],
        "error_bg_color": t["danger"], "error_fg_color": t["on_accent"], "error_color": t["danger"],
    }
    for name, role in GTK_SURFACES.items():
        colors[f"{name}_bg_color"] = t[role]
        colors[f"{name}_fg_color"] = t["label_1"]
    colors["borders"] = t["separator"]
    colors["sidebar_border_color"] = t["separator"]
    out = [f"/* Generated from protocol/design-tokens.toml by protocol/gen-design-tokens.py ({scheme}). Do not edit. */"]
    out += [f"@define-color {name} {css_color(value)};" for name, value in colors.items()]
    out += [
        "@define-color theme_bg_color @window_bg_color;", "@define-color theme_fg_color @window_fg_color;",
        "@define-color theme_base_color @view_bg_color;", "@define-color theme_text_color @view_fg_color;",
        "@define-color theme_selected_bg_color @accent_bg_color;",
        "@define-color theme_selected_fg_color @accent_fg_color;",
        "@define-color headerbar_backdrop_color @window_bg_color;",
        "@define-color sidebar_backdrop_color @window_bg_color;",
    ]
    if gtk4:
        out.append(":root {")
        out += [f"  --{name.replace('_', '-')}: {css_color(value)};" for name, value in colors.items() if name != "borders"]
        out.append(f"  --border-color: {css_color(t['separator'])};")
        out.append(f"  --window-radius: {tokens['radius']['md']}px;")
        out.append("}")
    with open(out_path, "w") as f:
        f.write("\n".join(out) + "\n")


def over(fg, bg):
    """fg (#rrggbbaa) composited over an opaque bg, as #rrggbbff."""
    f = [int(fg[i:i + 2], 16) for i in (1, 3, 5, 7)]
    b = [int(bg[i:i + 2], 16) for i in (1, 3, 5)]
    a = f[3] / 255
    return "#" + "".join(f"{round(f[i] * a + b[i] * (1 - a)):02x}" for i in range(3)) + "ff"


def argb(value):
    return "#" + value[7:9] + value[1:7]


def emit_qt(tokens, out_path, scheme):
    t = tokens["scheme"][scheme]
    w = t["window"]

    def palette(label):
        roles = [  # QPalette order used by qt5ct/qt6ct
            over(label, w), t["card"], t["view"], t["solid_thick"], t["solid_thin"], over(t["separator"], w),
            over(label, w), t["on_accent"], over(label, w), t["view"], w, t["shadow"], t["accent"], t["on_accent"],
            t["accent"], t["accent"], t["sidebar"], w, t["popover"], over(label, t["popover"]), over(t["label_3"], w),
            t["accent"],
        ]
        return ", ".join(argb(c) for c in roles)

    out = [f"# Generated from protocol/design-tokens.toml by protocol/gen-design-tokens.py ({scheme}). Do not edit.",
           "[ColorScheme]", f"active_colors={palette(t['label_1'])}", f"inactive_colors={palette(t['label_1'])}",
           f"disabled_colors={palette(t['label_3'])}"]
    with open(out_path, "w") as f:
        f.write("\n".join(out) + "\n")


def emit_kde(tokens, out_path, scheme):
    t = tokens["scheme"][scheme]
    w = t["window"]

    def rgb(value):
        return ",".join(str(int(value[i:i + 2], 16)) for i in (1, 3, 5))

    def group(bg, alt):
        return {
            "BackgroundNormal": rgb(bg), "BackgroundAlternate": rgb(alt),
            "ForegroundNormal": rgb(over(t["label_1"], bg)), "ForegroundInactive": rgb(over(t["label_2"], bg)),
            "ForegroundActive": rgb(t["accent"]), "ForegroundLink": rgb(t["accent"]),
            "ForegroundVisited": rgb(t["accent"]), "ForegroundNegative": rgb(t["danger"]),
            "ForegroundNeutral": rgb(over(t["label_2"], bg)), "ForegroundPositive": rgb(t["accent"]),
            "DecorationFocus": rgb(t["accent"]), "DecorationHover": rgb(t["accent"]),
        }

    groups = {
        "Window": group(w, t["sidebar"]), "View": group(t["view"], t["window"]), "Button": group(t["card"], w),
        "Tooltip": group(t["popover"], t["popover"]), "Header": group(t["header"], w),
        "Complementary": group(t["popover"], t["popover"]),
    }
    selection = group(t["accent"], t["accent"])
    selection["ForegroundNormal"] = selection["ForegroundActive"] = rgb(t["on_accent"])
    groups["Selection"] = selection
    name = f"Andromeda{scheme.title()}"
    out = [f"# Generated from protocol/design-tokens.toml by protocol/gen-design-tokens.py ({scheme}). Do not edit."]
    for group_name, keys in groups.items():
        out.append(f"[Colors:{group_name}]")
        out += [f"{k}={v}" for k, v in keys.items()]
        out.append("")
    out += ["[General]", f"ColorScheme={name}", f"Name=Andromeda {scheme.title()}", "shadeSortColumn=true", "",
            "[KDE]", "contrast=4", "", "[WM]", f"activeBackground={rgb(t['header'])}",
            f"activeForeground={rgb(over(t['label_1'], t['header']))}", f"inactiveBackground={rgb(w)}",
            f"inactiveForeground={rgb(over(t['label_2'], w))}"]
    with open(out_path, "w") as f:
        f.write("\n".join(out) + "\n")


def emit_fcitx5(tokens, out_path, scheme):
    import os
    t = tokens["scheme"][scheme]
    r = tokens["radius"]
    pad = tokens["space"]["s2"]
    directory = os.path.dirname(out_path)

    def svg(name, fill, radius):
        # A 9-slice source: fcitx5 stretches the middle, so corners stay at `radius`.
        size = int(radius * 2 + 2)
        rgb, alpha = f"#{fill[1:7]}", round(int(fill[7:9], 16) / 255, 3)
        with open(os.path.join(directory, name), "w") as f:
            f.write(f'<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}">'
                    f'<rect width="{size}" height="{size}" rx="{radius}" fill="{rgb}" fill-opacity="{alpha}"/></svg>\n')
        return int(radius)

    panel = svg("panel.svg", t["popover"], r["md"])
    highlight = svg("highlight.svg", t["accent"], r["xs"])
    label1, label2 = over(t["label_1"], t["popover"]), over(t["label_2"], t["popover"])
    out = [
        f"# Generated from protocol/design-tokens.toml by protocol/gen-design-tokens.py ({scheme}). Do not edit.",
        "[Metadata]", f"Name=Andromeda {scheme.title()}", "Version=1", "Author=Andromeda",
        "Description=Andromeda design tokens", "ScaleWithDPI=True",
        "", "[InputPanel]", f"Font={tokens['font']['family']} {tokens['type']['body']['size']}",
        f"NormalColor={label1[:7]}", f"HighlightCandidateColor={t['on_accent'][:7]}",
        f"HighlightColor={t['on_accent'][:7]}", f"HighlightBackgroundColor={t['accent'][:7]}", f"Spacing={int(pad)}",
        "", "[InputPanel/TextMargin]", *(f"{k}={int(pad // 2)}" for k in ("Left", "Right", "Top", "Bottom")),
        "", "[InputPanel/Background]", "Image=panel.svg",
        "", "[InputPanel/Background/Margin]", *(f"{k}={panel}" for k in ("Left", "Right", "Top", "Bottom")),
        "", "[InputPanel/Background/OverlayClipMargin]", *(f"{k}=0" for k in ("Left", "Right", "Top", "Bottom")),
        "", "[InputPanel/Highlight]", "Image=highlight.svg",
        "", "[InputPanel/Highlight/Margin]", *(f"{k}={highlight}" for k in ("Left", "Right", "Top", "Bottom")),
        "", "[InputPanel/ContentMargin]", *(f"{k}={int(pad)}" for k in ("Left", "Right", "Top", "Bottom")),
        "", "[InputPanel/PrevPage]", f"Color={label2[:7]}", "", "[InputPanel/NextPage]", f"Color={label2[:7]}",
    ]
    with open(out_path, "w") as f:
        f.write("\n".join(out) + "\n")


def main(src, *out_paths):
    with open(src, "rb") as f:
        tokens = tomllib.load(f)
    resolve(tokens)
    for path in out_paths:
        if m := re.search(r"-kde-(light|dark)\.colors$|Andromeda(Light|Dark)\.colors$", path):
            emit_kde(tokens, path, (m.group(1) or m.group(2)).lower())
        elif m := re.search(r"-qt-(light|dark)\.(colors|conf)$", path):
            emit_qt(tokens, path, m.group(1))
        elif m := re.search(r"fcitx5-(light|dark)/theme\.conf$", path):
            emit_fcitx5(tokens, path, m.group(1))
        elif m := re.search(r"-gtk([34])-(light|dark)\.css$", path):
            emit_gtk(tokens, path, m.group(2), m.group(1) == "4")
        elif path.endswith(".css"):
            emit_css(tokens, path)
        elif path.endswith(".glsl"):
            emit_glsl(tokens, path)
        else:
            emit_header(tokens, path)


def emit_header(tokens, out_path):
    out = ["// Generated from protocol/design-tokens.toml by protocol/gen-design-tokens.py. Do not edit.", "#pragma once",
           "", "#include <array>", "#include <cstdint>", "#include <string_view>", "#include <utility>", "",
           "// TOKENS_RUNTIME: values are writable (live reload, see slots()); otherwise constexpr.",
           "#ifdef TOKENS_RUNTIME", "#define TOKENS_CONST", "#else", "#define TOKENS_CONST constexpr", "#endif", "",
           "// A float derived from tokens that re-reads them on every use: constexpr without TOKENS_RUNTIME.",
           "#ifdef TOKENS_RUNTIME",
           "namespace Tokens { template <class F> struct Live { F fn; operator float() const { return fn(); } }; }",
           "#define TOKENS_LIVE(name, expr) const auto name = ::Tokens::Live{[] { return static_cast<float>(expr); }}",
           "#else", "#define TOKENS_LIVE(name, expr) constexpr float name = (expr)",
           "#endif", "",
           "namespace Tokens {", ""]
    slots, refresh = [], []
    emit(tokens, (), out, slots, refresh)
    out += ["", "#ifdef TOKENS_RUNTIME", "  struct Slot {", "    std::string_view path;",
            "    char kind;  // b bool, i int, f float, c packed color, s string_view", "    void* ptr;",
            "    double number;  // built-in value of a b, i, f or c slot", "    std::string_view text;  // built-in value of an s slot", "  };",
            f"  inline const std::array<Slot, {len(slots)}>& slots() {{",
            f"    static const std::array<Slot, {len(slots)}> table{{{{"]
    out += [f'        {{"{p}", \'{k}\', {ptr}, {default_number(v)}, {default_text(v)}}},' for p, k, ptr, v in slots]
    out += ["    }};", "    return table;", "  }", "  inline void refreshEntries() {", *refresh, "  }", "#endif", "",
            "} // namespace Tokens", ""]
    with open(out_path, "w") as f:
        f.write("\n".join(out))


if __name__ == "__main__":
    main(*sys.argv[1:])
