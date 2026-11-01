#!/usr/bin/env python3
"""Usage: gen-design-tokens.py TOKENS.toml OUT.h -- writes namespace Tokens (see the header of TOKENS.toml)."""
import re
import sys
import tomllib

COLOR = re.compile(r"#[0-9a-fA-F]{8}")


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


def emit(table, path, out):
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
                out.append(f"  inline constexpr {name} {camel(sub + (child,))}{{{literals}}};")
                if all(isinstance(v, str) and COLOR.fullmatch(v) for v in values.values()):
                    pairs = ", ".join(f'{{"{k}", {scalar((), v)[1]}}}' for k, v in values.items())
                    out.append(f"  inline constexpr std::array<std::pair<std::string_view, std::uint32_t>, {len(values)}> "
                               f"{camel(sub + (child, 'entries'))}{{{{{pairs}}}}};")
        elif isinstance(value, dict):
            emit(value, sub, out)
        else:
            kind, literal = scalar(sub, value)
            out.append(f"  inline constexpr {kind} {camel(sub)} = {literal};")


def main(src, out_path):
    with open(src, "rb") as f:
        tokens = tomllib.load(f)
    out = ["// Generated from protocol/design-tokens.toml by protocol/gen-design-tokens.py. Do not edit.", "#pragma once",
           "", "#include <array>", "#include <cstdint>", "#include <string_view>", "#include <utility>", "", "namespace Tokens {", ""]
    emit(tokens, (), out)
    out += ["", "} // namespace Tokens", ""]
    with open(out_path, "w") as f:
        f.write("\n".join(out))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
