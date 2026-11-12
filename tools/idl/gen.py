#!/usr/bin/env python3
"""Action IDL generator: one set of `<module>/actions/*.toml` files -> C++ header, Rust module, help.json.

IDL format (one file per domain; the files of one module merge into one manifest):

    [[action]]
    name = "workspace.switch"          # domain.verb: lowercase, digits, underscores
    description = "Show workspace N on the focused output."
    args = [{ name = "index", type = "u32", required = true, description = "1-based position", example = "2" }]
    result = "Workspace"               # a [[type]] name or a builtin type expression
    events = ["workspace.changed"]     # event names the action can cause (optional, default [])
    permission = "windows"             # read | windows | input | settings | files | apps | system
    reversible = true
    sensitive = false
    binds = "workspace-switch"         # the existing handler (IPC command or msg action) the provider calls

    [[type]]
    name = "Workspace"                 # CamelCase
    fields = [{ name = "id", type = "id" }, { name = "index", type = "u32" }]

Type expressions: bool, i64, u32, f64, string, id, list<T>, optional<T>, or a [[type]] name. `id` is a short
non-empty string such as `w3`, `ws2` or `out:DP-1`. An arg is required unless its type is optional<T> or it sets
`required = false`; a non-required arg of type T reaches the handler as std::optional<T>. `example` (a string) is the
value the error example shows; without it a value is made from the type. Unknown keys are errors.

Outputs (each optional):
    --cpp FILE    C++ header: per action a namespace `<domain>_<verb>` with kName, kBinds, kExample, `struct Args`,
                  `using Result`, `parse(json) -> std::expected<Args, Error>`; per type a struct with read()/write();
                  one `handle(Context&, const <ns>::Args&)` declaration per action and the `kActions` table that calls
                  them; kManifestHash and kHelpActions (the help.json action entries, as JSON text). The provider
                  defines the handlers: an IDL entry with no handler fails the link, a handler with
                  no IDL entry fails to compile (its Args type does not exist).
    --rust FILE   Rust module with registry data: SCHEMA_VERSION, MANIFEST_HASH, ACTIONS: &[ActionSpec], TYPES.
    --help-json FILE   the manifest plus its hash, for `help --json`.
The manifest hash is the sha256 of the canonical manifest JSON (sorted keys, no whitespace).

Usage: gen.py --module NAME [--cpp-namespace NS] [--cpp-context CLASS] [--cpp F] [--rust F] [--help-json F] FILE...
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import tomllib
from pathlib import Path

SCHEMA_VERSION = 1
BUILTINS = ("bool", "i64", "u32", "f64", "string", "id")
PERMISSIONS = ("read", "windows", "input", "settings", "files", "apps", "system")
ACTION_NAME = re.compile(r"^[a-z][a-z0-9_]*\.[a-z][a-z0-9_]*$")
FIELD_NAME = re.compile(r"^[a-z][a-z0-9_]*$")
TYPE_NAME = re.compile(r"^[A-Z][A-Za-z0-9]*$")
ACTION_KEYS = {"name", "description", "args", "result", "events", "permission", "reversible", "sensitive", "binds"}
ACTION_REQUIRED = ACTION_KEYS - {"args", "events"}
ARG_KEYS = {"name", "type", "required", "description", "example"}
FIELD_KEYS = {"name", "type", "description"}
EXAMPLE_VALUES = {"bool": "true", "i64": "0", "u32": "1", "f64": "1.0", "string": "text", "id": "w1"}


class IdlError(Exception):
    pass


def parse_type(text: str, where: str) -> tuple:
    """Returns ("builtin", name) | ("named", name) | ("list", inner) | ("optional", inner)."""
    text = text.strip()
    for wrapper in ("list", "optional"):
        if text.startswith(wrapper + "<") and text.endswith(">"):
            inner = parse_type(text[len(wrapper) + 1 : -1], where)
            if wrapper == "optional" and inner[0] == "optional":
                raise IdlError(f"{where}: optional<optional<...>> is not allowed")
            return (wrapper, inner)
    if text in BUILTINS:
        return ("builtin", text)
    if TYPE_NAME.match(text):
        return ("named", text)
    raise IdlError(f"{where}: unknown type '{text}'")


def type_text(t: tuple) -> str:
    return t[1] if t[0] in ("builtin", "named") else f"{t[0]}<{type_text(t[1])}>"


def named_in(t: tuple) -> list[str]:
    if t[0] == "named":
        return [t[1]]
    return named_in(t[1]) if t[0] in ("list", "optional") else []


def expect(table: dict, key: str, kind: type, where: str):
    value = table[key]
    if not isinstance(value, kind) or (kind is int and isinstance(value, bool)):
        raise IdlError(f"{where}: '{key}' must be {kind.__name__}")
    return value


def check_keys(table, allowed: set, required: set, where: str) -> None:
    if not isinstance(table, dict):
        raise IdlError(f"{where}: must be a table")
    unknown = sorted(set(table) - allowed)
    if unknown:
        raise IdlError(f"{where}: unknown key '{unknown[0]}'")
    missing = sorted(required - set(table))
    if missing:
        raise IdlError(f"{where}: missing key '{missing[0]}'")


def load(paths: list[Path], module: str) -> dict:
    actions: dict[str, dict] = {}
    types: dict[str, dict] = {}
    for path in paths:
        try:
            doc = tomllib.loads(path.read_text(encoding="utf-8"))
        except tomllib.TOMLDecodeError as error:
            raise IdlError(f"{path}: {error}") from error
        unknown = sorted(set(doc) - {"action", "type"})
        if unknown:
            raise IdlError(f"{path}: unknown top-level key '{unknown[0]}'")
        for raw in doc.get("type", []):
            where = f"{path}: type '{raw.get('name', '?')}'" if isinstance(raw, dict) else f"{path}: type"
            check_keys(raw, {"name", "fields"}, {"name", "fields"}, where)
            name = expect(raw, "name", str, where)
            if not TYPE_NAME.match(name):
                raise IdlError(f"{where}: type names are CamelCase")
            if name in types:
                raise IdlError(f"{where}: duplicate type")
            fields = []
            for field in expect(raw, "fields", list, where):
                fwhere = f"{where}: field '{field.get('name', '?') if isinstance(field, dict) else '?'}'"
                check_keys(field, FIELD_KEYS, {"name", "type"}, fwhere)
                fname = expect(field, "name", str, fwhere)
                if not FIELD_NAME.match(fname):
                    raise IdlError(f"{fwhere}: field names are lowercase")
                if any(f["name"] == fname for f in fields):
                    raise IdlError(f"{fwhere}: duplicate field")
                ftype = parse_type(expect(field, "type", str, fwhere), fwhere)
                fields.append({"name": fname, "type": type_text(ftype), "description": field.get("description", "")})
            types[name] = {"name": name, "fields": fields}
        for raw in doc.get("action", []):
            where = f"{path}: action '{raw.get('name', '?')}'" if isinstance(raw, dict) else f"{path}: action"
            check_keys(raw, ACTION_KEYS, ACTION_REQUIRED, where)
            name = expect(raw, "name", str, where)
            if not ACTION_NAME.match(name):
                raise IdlError(f"{where}: action names are domain.verb in lowercase")
            if name in actions:
                raise IdlError(f"{where}: duplicate action")
            permission = expect(raw, "permission", str, where)
            if permission not in PERMISSIONS:
                raise IdlError(f"{where}: permission '{permission}' is not one of {', '.join(PERMISSIONS)}")
            if not expect(raw, "description", str, where).strip():
                raise IdlError(f"{where}: empty description")
            if not expect(raw, "binds", str, where).strip():
                raise IdlError(f"{where}: empty binds")
            args = []
            for arg in raw.get("args", []):
                awhere = f"{where}: arg '{arg.get('name', '?') if isinstance(arg, dict) else '?'}'"
                check_keys(arg, ARG_KEYS, {"name", "type"}, awhere)
                aname = expect(arg, "name", str, awhere)
                if not FIELD_NAME.match(aname):
                    raise IdlError(f"{awhere}: arg names are lowercase")
                if any(a["name"] == aname for a in args):
                    raise IdlError(f"{awhere}: duplicate arg")
                atype = parse_type(expect(arg, "type", str, awhere), awhere)
                required = arg["required"] if "required" in arg else atype[0] != "optional"
                if not isinstance(required, bool):
                    raise IdlError(f"{awhere}: 'required' must be bool")
                if required and atype[0] == "optional":
                    raise IdlError(f"{awhere}: a required arg cannot have an optional type")
                example = arg.get("example")
                if example is not None and not isinstance(example, str):
                    raise IdlError(f"{awhere}: 'example' must be str")
                args.append({
                    "name": aname,
                    "type": type_text(atype),
                    "required": required,
                    "description": arg.get("description", ""),
                    "example": example if example is not None else example_for(atype),
                })
            events = raw.get("events", [])
            if not isinstance(events, list) or not all(isinstance(e, str) for e in events):
                raise IdlError(f"{where}: 'events' must be a list of strings")
            actions[name] = {
                "name": name,
                "description": raw["description"],
                "args": args,
                "result": type_text(parse_type(expect(raw, "result", str, where), where)),
                "events": events,
                "permission": permission,
                "reversible": expect(raw, "reversible", bool, where),
                "sensitive": expect(raw, "sensitive", bool, where),
                "binds": raw["binds"],
            }
    for type_ in types.values():
        for field in type_["fields"]:
            check_named(field["type"], types, f"type '{type_['name']}': field '{field['name']}'")
    for action in actions.values():
        check_named(action["result"], types, f"action '{action['name']}': result")
        for arg in action["args"]:
            check_named(arg["type"], types, f"action '{action['name']}': arg '{arg['name']}'")
    ordered_types(types)
    for action in actions.values():
        action["example"] = example_command(action)
    return {
        "module": module,
        "schema_version": SCHEMA_VERSION,
        "actions": [actions[k] for k in sorted(actions)],
        "types": [types[k] for k in sorted(types)],
    }


def check_named(text: str, types: dict, where: str) -> None:
    for name in named_in(parse_type(text, where)):
        if name not in types:
            raise IdlError(f"{where}: unknown type '{name}'")


def example_for(t: tuple) -> str:
    if t[0] == "builtin":
        return EXAMPLE_VALUES[t[1]]
    if t[0] == "optional":
        return example_for(t[1])
    if t[0] == "list":
        return example_for(t[1])
    return "{}"


def example_command(action: dict) -> str:
    parts = ["andromeda act", action["name"]]
    for arg in action["args"]:
        if arg["required"]:
            parts.append(f"--{arg['name']} {arg['example']}")
    return " ".join(parts)


def ordered_types(types: dict) -> list[dict]:
    """Types in dependency order; a cycle is an error (a struct cannot hold itself by value)."""
    done: list[str] = []
    state: dict[str, str] = {}

    def visit(name: str, chain: list[str]) -> None:
        if state.get(name) == "done":
            return
        if state.get(name) == "visiting":
            raise IdlError(f"type cycle: {' -> '.join(chain + [name])}")
        state[name] = "visiting"
        for field in types[name]["fields"]:
            for dep in named_in(parse_type(field["type"], name)):
                visit(dep, chain + [name])
        state[name] = "done"
        done.append(name)

    for name in sorted(types):
        visit(name, [])
    return [types[name] for name in done]


def manifest_hash(manifest: dict) -> str:
    canonical = json.dumps(manifest, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def cpp_string(text: str) -> str:
    return json.dumps(text, ensure_ascii=False)


CPP_SCALARS = {"bool": "bool", "i64": "int64_t", "u32": "uint32_t", "f64": "double", "string": "std::string",
               "id": "Id"}


def cpp_type(t: tuple) -> str:
    if t[0] == "builtin":
        return CPP_SCALARS[t[1]]
    if t[0] == "named":
        return t[1]
    if t[0] == "list":
        return f"std::vector<{cpp_type(t[1])}>"
    return f"std::optional<{cpp_type(t[1])}>"


def action_ns(name: str) -> str:
    return name.replace(".", "_")


CPP_PRELUDE = """\
enum class ErrorCode : uint8_t { InvalidArgument, NotFound, PermissionDenied, Unavailable, Conflict, Cancelled };

struct Error {
  ErrorCode code = ErrorCode::InvalidArgument;
  std::string message;
  std::string hint;
  std::string example;
};

inline std::string_view errorCodeName(ErrorCode code) {
  switch (code) {
  case ErrorCode::InvalidArgument: return "invalid_argument";
  case ErrorCode::NotFound: return "not_found";
  case ErrorCode::PermissionDenied: return "permission_denied";
  case ErrorCode::Unavailable: return "unavailable";
  case ErrorCode::Conflict: return "conflict";
  case ErrorCode::Cancelled: return "cancelled";
  }
  return "invalid_argument";
}

inline nlohmann::json write(const Error& error) {
  return {{"code", errorCodeName(error.code)}, {"message", error.message}, {"hint", error.hint},
          {"example", error.example}};
}

struct Id {
  std::string value;
  bool operator==(const Id&) const = default;
};

template <class T> bool read(const nlohmann::json& in, std::optional<T>& out);
template <class T> bool read(const nlohmann::json& in, std::vector<T>& out);
template <class T> nlohmann::json write(const std::optional<T>& value);
template <class T> nlohmann::json write(const std::vector<T>& values);

inline bool read(const nlohmann::json& in, bool& out) {
  if (!in.is_boolean()) return false;
  out = in.get<bool>();
  return true;
}
inline bool read(const nlohmann::json& in, int64_t& out) {
  if (!in.is_number_integer()) return false;
  out = in.get<int64_t>();
  return true;
}
inline bool read(const nlohmann::json& in, uint32_t& out) {
  if (!in.is_number_unsigned() || in.get<uint64_t>() > UINT32_MAX) return false;
  out = static_cast<uint32_t>(in.get<uint64_t>());
  return true;
}
inline bool read(const nlohmann::json& in, double& out) {
  if (!in.is_number()) return false;
  out = in.get<double>();
  return true;
}
inline bool read(const nlohmann::json& in, std::string& out) {
  if (!in.is_string()) return false;
  out = in.get<std::string>();
  return true;
}
inline bool read(const nlohmann::json& in, Id& out) {
  if (!in.is_string() || in.get_ref<const std::string&>().empty()) return false;
  out.value = in.get<std::string>();
  return true;
}
template <class T> bool read(const nlohmann::json& in, std::optional<T>& out) {
  if (in.is_null()) {
    out.reset();
    return true;
  }
  T value{};
  if (!read(in, value)) return false;
  out = std::move(value);
  return true;
}
template <class T> bool read(const nlohmann::json& in, std::vector<T>& out) {
  if (!in.is_array()) return false;
  out.clear();
  for (const auto& item : in) {
    T value{};
    if (!read(item, value)) return false;
    out.push_back(std::move(value));
  }
  return true;
}

inline nlohmann::json write(bool value) { return value; }
inline nlohmann::json write(int64_t value) { return value; }
inline nlohmann::json write(uint32_t value) { return value; }
inline nlohmann::json write(double value) { return value; }
inline nlohmann::json write(const std::string& value) { return value; }
inline nlohmann::json write(const Id& value) { return value.value; }
template <class T> nlohmann::json write(const std::optional<T>& value) {
  return value.has_value() ? write(*value) : nlohmann::json(nullptr);
}
template <class T> nlohmann::json write(const std::vector<T>& values) {
  nlohmann::json out = nlohmann::json::array();
  for (const auto& value : values) out.push_back(write(value));
  return out;
}

inline Error badArgument(std::string message, std::string_view example) {
  return {ErrorCode::InvalidArgument, std::move(message), "run `andromeda help --json` for the argument types",
          std::string(example)};
}

template <size_t N>
std::optional<Error> rejectUnknownArgs(const nlohmann::json& in, const std::array<std::string_view, N>& names,
                                       std::string_view example) {
  if (!in.is_object()) return badArgument("args must be a JSON object", example);
  for (auto it = in.begin(); it != in.end(); ++it) {
    if (std::ranges::find(names, std::string_view(it.key())) == names.end()) {
      return badArgument("unknown argument '" + it.key() + "'", example);
    }
  }
  return std::nullopt;
}
"""


def gen_cpp(manifest: dict, namespace: str, context: str, digest: str) -> str:
    types = {t["name"]: t for t in manifest["types"]}
    context_ns, _, context_name = context.rpartition("::")
    guard = "IDL_PRELUDE_" + re.sub(r"\W", "_", namespace).upper()
    out = [
        "// Generated by tools/idl/gen.py from the action IDL. Do not edit.",
        "#pragma once",
        "",
        "#include <algorithm>",
        "#include <array>",
        "#include <cstdint>",
        "#include <expected>",
        "#include <nlohmann/json.hpp>",
        "#include <optional>",
        "#include <string>",
        "#include <string_view>",
        "#include <vector>",
        "",
    ]
    if context_ns:
        out += [f"namespace {context_ns} {{", f"  class {context_name};", "}", ""]
    else:
        out += [f"class {context_name};", ""]
    out += [f"namespace {namespace} {{", "", f"#ifndef {guard}", f"#define {guard}", CPP_PRELUDE, "#endif", ""]
    out += [
        f"inline constexpr int kSchemaVersion = {SCHEMA_VERSION};",
        f"inline constexpr std::string_view kModule = {cpp_string(manifest['module'])};",
        f"inline constexpr std::string_view kManifestHash = {cpp_string(digest)};",
        "// The help.json action entries, as the provider hello carries them.",
        f"inline constexpr std::string_view kHelpActions = R\"idl({help_actions(manifest)})idl\";",
        f"using Context = {context};",
        "using Outcome = std::expected<nlohmann::json, Error>;",
        "",
    ]
    for type_ in ordered_types(types):
        name = type_["name"]
        out.append(f"struct {name} {{")
        for field in type_["fields"]:
            out.append(f"  {cpp_type(parse_type(field['type'], name))} {field['name']}{{}};")
        out.append("};")
        out.append(f"inline nlohmann::json write(const {name}& value) {{")
        out.append("  nlohmann::json out = nlohmann::json::object();")
        for field in type_["fields"]:
            out.append(f"  out[{cpp_string(field['name'])}] = write(value.{field['name']});")
        out.append("  return out;")
        out.append("}")
        out.append(f"inline bool read(const nlohmann::json& in, {name}& out) {{")
        out.append("  if (!in.is_object()) return false;")
        for field in type_["fields"]:
            key = cpp_string(field["name"])
            if field["type"].startswith("optional<"):
                out.append(f"  if (in.contains({key}) && !read(in.at({key}), out.{field['name']})) return false;")
            else:
                out.append(f"  if (!in.contains({key}) || !read(in.at({key}), out.{field['name']})) return false;")
        out.append("  return true;")
        out.append("}")
        out.append("")
    for action in manifest["actions"]:
        ns = action_ns(action["name"])
        names = ", ".join(cpp_string(a["name"]) for a in action["args"])
        out += [
            f"namespace {ns} {{",
            f"  inline constexpr std::string_view kName = {cpp_string(action['name'])};",
            f"  inline constexpr std::string_view kBinds = {cpp_string(action['binds'])};",
            f"  inline constexpr std::string_view kExample = {cpp_string(action['example'])};",
            f"  inline constexpr std::array<std::string_view, {len(action['args'])}> kArgNames = {{{names}}};",
            "  struct Args {",
        ]
        for arg in action["args"]:
            atype = parse_type(arg["type"], ns)
            if not arg["required"] and atype[0] != "optional":
                atype = ("optional", atype)
            out.append(f"    {cpp_type(atype)} {arg['name']}{{}};")
        out += [
            "  };",
            f"  using Result = {cpp_type(parse_type(action['result'], ns))};",
            "  inline std::expected<Args, Error> parse(const nlohmann::json& in) {",
            "    if (auto error = rejectUnknownArgs(in, kArgNames, kExample)) return std::unexpected(std::move(*error));",
            "    Args out{};",
        ]
        for arg in action["args"]:
            key = cpp_string(arg["name"])
            shown = arg["type"]
            out.append(f"    if (auto it = in.find({key}); it != in.end() && !it->is_null()) {{")
            out.append(f"      if (!read(*it, out.{arg['name']})) {{")
            out.append(
                f"        return std::unexpected(badArgument(\"argument '{arg['name']}' must be {shown}\", kExample));"
            )
            out.append("      }")
            if arg["required"]:
                out.append("    } else {")
                out.append(
                    f"      return std::unexpected(badArgument(\"missing argument '{arg['name']}'\", kExample));"
                )
            out.append("    }")
        out += ["    return out;", "  }", "}", ""]
    out.append("// The provider defines one handler per action.")
    for action in manifest["actions"]:
        ns = action_ns(action["name"])
        out.append(f"std::expected<{ns}::Result, Error> handle(Context& context, const {ns}::Args& args);")
    out.append("")
    out.append("struct ActionEntry {")
    out.append("  std::string_view name;")
    out.append("  Outcome (*invoke)(Context& context, const nlohmann::json& args);")
    out.append("};")
    out.append("")
    out.append("template <auto Parse> Outcome invoke(Context& context, const nlohmann::json& args) {")
    out.append("  auto parsed = Parse(args);")
    out.append("  if (!parsed) return std::unexpected(std::move(parsed.error()));")
    out.append("  auto result = handle(context, *parsed);")
    out.append("  if (!result) return std::unexpected(std::move(result.error()));")
    out.append("  return write(*result);")
    out.append("}")
    out.append("")
    out.append(f"inline constexpr std::array<ActionEntry, {len(manifest['actions'])}> kActions = {{{{")
    for action in manifest["actions"]:
        ns = action_ns(action["name"])
        out.append(f"    {{{ns}::kName, &invoke<&{ns}::parse>}},")
    out.append("}};")
    out.append("")
    out.append(f"}} // namespace {namespace}")
    out.append("")
    return "\n".join(out)


def rust_string(text: str) -> str:
    return json.dumps(text, ensure_ascii=False)


def rust_type(t: tuple) -> str:
    if t[0] == "builtin":
        return f"TypeRef::{t[1].capitalize()}"
    if t[0] == "named":
        return f"TypeRef::Named({rust_string(t[1])})"
    return f"TypeRef::{t[0].capitalize()}(&{rust_type(t[1])})"


RUST_PRELUDE = """\
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TypeRef {
    Bool,
    I64,
    U32,
    F64,
    String,
    Id,
    List(&'static TypeRef),
    Optional(&'static TypeRef),
    Named(&'static str),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Permission {
    Read,
    Windows,
    Input,
    Settings,
    Files,
    Apps,
    System,
}

#[derive(Debug, Clone, Copy)]
pub struct ArgSpec {
    pub name: &'static str,
    pub ty: TypeRef,
    pub required: bool,
    pub description: &'static str,
    pub example: &'static str,
}

#[derive(Debug, Clone, Copy)]
pub struct ActionSpec {
    pub name: &'static str,
    pub description: &'static str,
    pub args: &'static [ArgSpec],
    pub result: TypeRef,
    pub events: &'static [&'static str],
    pub permission: Permission,
    pub reversible: bool,
    pub sensitive: bool,
    pub example: &'static str,
}

#[derive(Debug, Clone, Copy)]
pub struct FieldSpec {
    pub name: &'static str,
    pub ty: TypeRef,
}

#[derive(Debug, Clone, Copy)]
pub struct TypeSpec {
    pub name: &'static str,
    pub fields: &'static [FieldSpec],
}
"""


def gen_rust(manifest: dict, digest: str) -> str:
    out = [
        "// Generated by tools/idl/gen.py from the action IDL. Do not edit.",
        "",
        RUST_PRELUDE,
        f"pub const SCHEMA_VERSION: u32 = {SCHEMA_VERSION};",
        f"pub const MODULE: &str = {rust_string(manifest['module'])};",
        f"pub const MANIFEST_HASH: &str = {rust_string(digest)};",
        "",
        "pub const ACTIONS: &[ActionSpec] = &[",
    ]
    for action in manifest["actions"]:
        out.append("    ActionSpec {")
        out.append(f"        name: {rust_string(action['name'])},")
        out.append(f"        description: {rust_string(action['description'])},")
        out.append("        args: &[")
        for arg in action["args"]:
            out.append(
                f"            ArgSpec {{ name: {rust_string(arg['name'])}, ty: {rust_type(parse_type(arg['type'], ''))}, "
                f"required: {str(arg['required']).lower()}, description: {rust_string(arg['description'])}, "
                f"example: {rust_string(arg['example'])} }},"
            )
        out.append("        ],")
        out.append(f"        result: {rust_type(parse_type(action['result'], ''))},")
        out.append(f"        events: &[{', '.join(rust_string(e) for e in action['events'])}],")
        out.append(f"        permission: Permission::{action['permission'].capitalize()},")
        out.append(f"        reversible: {str(action['reversible']).lower()},")
        out.append(f"        sensitive: {str(action['sensitive']).lower()},")
        out.append(f"        example: {rust_string(action['example'])},")
        out.append("    },")
    out.append("];")
    out.append("")
    out.append("pub const TYPES: &[TypeSpec] = &[")
    for type_ in manifest["types"]:
        fields = ", ".join(
            f"FieldSpec {{ name: {rust_string(f['name'])}, ty: {rust_type(parse_type(f['type'], ''))} }}"
            for f in type_["fields"]
        )
        out.append(f"    TypeSpec {{ name: {rust_string(type_['name'])}, fields: &[{fields}] }},")
    out.append("];")
    out.append("")
    return "\n".join(out)


def help_actions(manifest: dict) -> str:
    text = json.dumps(manifest["actions"], sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    if ")idl\"" in text:
        raise IdlError("an action text contains ')idl\"', which ends the C++ raw string")
    return text


def gen_help(manifest: dict, digest: str) -> str:
    return json.dumps({**manifest, "manifest_hash": digest}, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Generate code from the action IDL.")
    parser.add_argument("--module", required=True)
    parser.add_argument("--cpp-namespace", default="idl")
    parser.add_argument("--cpp-context", default="ProviderContext")
    parser.add_argument("--cpp", type=Path)
    parser.add_argument("--rust", type=Path)
    parser.add_argument("--help-json", type=Path)
    parser.add_argument("files", nargs="+", type=Path)
    options = parser.parse_args(argv)
    try:
        manifest = load(options.files, options.module)
    except IdlError as error:
        print(f"idl: {error}", file=sys.stderr)
        return 1
    digest = manifest_hash(manifest)
    if options.cpp:
        options.cpp.write_text(gen_cpp(manifest, options.cpp_namespace, options.cpp_context, digest), encoding="utf-8")
    if options.rust:
        options.rust.write_text(gen_rust(manifest, digest), encoding="utf-8")
    if options.help_json:
        options.help_json.write_text(gen_help(manifest, digest), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
