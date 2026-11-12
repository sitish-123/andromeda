import importlib.util
import json
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("gen", HERE.parent / "gen.py")
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)

FIXTURE = HERE / "fixtures" / "sample.toml"

ACTION = """
[[action]]
name = "thing.do"
description = "Do it."
args = [{{ name = "value", type = "{arg_type}"{extra} }}]
result = "{result}"
permission = "{permission}"
reversible = true
sensitive = false
binds = "thing-do"
"""

CPP_MAIN = r"""
#include "out.h"
#include <cstdio>

namespace demo { class Ctx {}; }

namespace idl {
  std::expected<shape_list::Result, Error> handle(Context&, const shape_list::Args&) { return {}; }
  std::expected<shape_scale::Result, Error> handle(Context&, const shape_scale::Args& args) {
    Shape shape{.id = args.id, .points = {{.x = 1, .y = -2}}, .label = args.note};
    for (const auto& tag : args.tags.value_or(std::vector<std::string>{})) shape.label = tag;
    return shape;
  }
}

int main() {
  demo::Ctx ctx;
  for (const char* line : {
           R"({"id":"s1","factor":2,"tags":["a","b"]})",
           R"({"id":"s1","factor":2,"note":"n"})",
           R"({"id":"s1"})",
           R"({"id":"s1","factor":"x"})",
           R"({"id":"","factor":1})",
           R"({"id":"s1","factor":1,"bogus":1})",
       }) {
    auto out = idl::kActions[1].invoke(ctx, nlohmann::json::parse(line));
    std::puts(out ? out->dump().c_str() : idl::write(out.error()).dump().c_str());
  }
}
"""


def write_idl(directory: Path, name: str, text: str) -> Path:
    path = directory / name
    path.write_text(textwrap.dedent(text), encoding="utf-8")
    return path


class LoadTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def load_action(self, arg_type="u32", extra="", result="bool", permission="read"):
        path = write_idl(self.dir, "a.toml", ACTION.format(arg_type=arg_type, extra=extra, result=result,
                                                           permission=permission))
        return gen.load([path], "test")

    def assert_error(self, pattern, **kwargs):
        with self.assertRaisesRegex(gen.IdlError, pattern):
            self.load_action(**kwargs)

    def test_fixture_resolves_types_and_requiredness(self):
        manifest = gen.load([FIXTURE], "demo")
        scale = next(a for a in manifest["actions"] if a["name"] == "shape.scale")
        self.assertEqual({a["name"]: a["required"] for a in scale["args"]},
                         {"id": True, "factor": True, "tags": False, "note": False})
        self.assertEqual(scale["result"], "Shape")
        self.assertEqual(scale["example"], "andromeda act shape.scale --id s1 --factor 1.0")
        shape = next(t for t in manifest["types"] if t["name"] == "Shape")
        self.assertEqual([f["type"] for f in shape["fields"]], ["id", "list<Point>", "optional<string>"])

    def test_manifest_hash_follows_content(self):
        first = gen.manifest_hash(gen.load([FIXTURE], "demo"))
        self.assertEqual(first, gen.manifest_hash(gen.load([FIXTURE], "demo")))
        changed = write_idl(self.dir, "s.toml", FIXTURE.read_text().replace("Scale a shape.", "Resize a shape."))
        self.assertNotEqual(first, gen.manifest_hash(gen.load([changed], "demo")))

    def test_nested_builtin_types_are_accepted(self):
        manifest = self.load_action(arg_type="optional<list<id>>", result="list<optional<f64>>")
        self.assertFalse(manifest["actions"][0]["args"][0]["required"])

    def test_bad_type_name_is_an_error(self):
        self.assert_error(r"arg 'value': unknown type 'strng'", arg_type="strng")
        self.assert_error(r"result: unknown type 'Missing'", result="list<Missing>")
        self.assert_error(r"unknown type 'list<u32'", arg_type="list<u32")

    def test_contradictions_are_errors(self):
        self.assert_error(r"required arg cannot have an optional type", arg_type="optional<u32>",
                          extra=", required = true")
        self.assert_error(r"optional<optional", arg_type="optional<optional<u32>>")
        self.assert_error(r"permission 'root'", permission="root")
        self.assert_error(r"unknown key 'default'", extra=", default = 3")

    def test_duplicates_across_files_and_cycles_are_errors(self):
        one = write_idl(self.dir, "one.toml", ACTION.format(arg_type="u32", extra="", result="bool",
                                                            permission="read"))
        two = write_idl(self.dir, "two.toml", one.read_text())
        with self.assertRaisesRegex(gen.IdlError, "duplicate action"):
            gen.load([one, two], "test")
        cycle = write_idl(self.dir, "cycle.toml", """
            [[type]]
            name = "A"
            fields = [{ name = "b", type = "optional<B>" }]
            [[type]]
            name = "B"
            fields = [{ name = "a", type = "A" }]
        """)
        with self.assertRaisesRegex(gen.IdlError, "type cycle"):
            gen.load([cycle], "test")


class OutputTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.assertEqual(gen.main([
            "--module", "demo", "--cpp-namespace", "idl", "--cpp-context", "demo::Ctx",
            "--cpp", str(self.dir / "out.h"), "--rust", str(self.dir / "out.rs"),
            "--help-json", str(self.dir / "help.json"), str(FIXTURE),
        ]), 0)

    def tearDown(self):
        self.tmp.cleanup()

    def test_help_json_carries_hash_of_manifest(self):
        help_doc = json.loads((self.dir / "help.json").read_text())
        digest = help_doc.pop("manifest_hash")
        self.assertEqual(digest, gen.manifest_hash(help_doc))
        self.assertEqual([a["name"] for a in help_doc["actions"]], ["shape.list", "shape.scale"])

    @unittest.skipUnless(shutil.which("c++"), "no C++ compiler")
    def test_cpp_parses_args_and_writes_results(self):
        (self.dir / "main.cpp").write_text(CPP_MAIN)
        subprocess.run(["c++", "-std=c++23", "-o", str(self.dir / "demo"), str(self.dir / "main.cpp")],
                       cwd=self.dir, check=True)
        lines = subprocess.run([str(self.dir / "demo")], capture_output=True, text=True,
                               check=True).stdout.splitlines()
        replies = [json.loads(line) for line in lines]
        self.assertEqual(replies[0], {"id": "s1", "points": [{"x": 1, "y": -2}], "label": "b"})
        self.assertEqual(replies[1]["label"], "n")
        self.assertEqual(replies[2]["message"], "missing argument 'factor'")
        self.assertEqual(replies[3]["message"], "argument 'factor' must be f64")
        self.assertEqual(replies[4]["message"], "argument 'id' must be id")
        self.assertEqual(replies[5]["message"], "unknown argument 'bogus'")
        for reply in replies[2:]:
            self.assertEqual(reply["code"], "invalid_argument")
            self.assertEqual(reply["example"], "andromeda act shape.scale --id s1 --factor 1.0")

    @unittest.skipUnless(shutil.which("rustc"), "no rustc")
    def test_rust_module_compiles(self):
        subprocess.run(["rustc", "--edition", "2021", "--crate-type", "lib", "-o", str(self.dir / "out.rlib"),
                        str(self.dir / "out.rs")], cwd=self.dir, check=True)


if __name__ == "__main__":
    unittest.main()
