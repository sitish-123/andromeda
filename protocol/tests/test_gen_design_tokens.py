#!/usr/bin/env python3
"""Golden tests for gen-design-tokens.py: fixture-tokens.toml in, the GTK, Qt and fcitx5 files out.
Regenerate the goldens after an intended emitter change: UPDATE_GOLDEN=1 python3 test_gen_design_tokens.py"""
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
GENERATOR = HERE.parent / "gen-design-tokens.py"
FIXTURE = HERE / "fixture-tokens.toml"
OUTPUTS = ["a-gtk3-light.css", "a-gtk4-dark.css", "a-qt-dark.colors", "a-kde-light.colors", "fcitx5-light/theme.conf",
           "fcitx5-light/panel.svg", "fcitx5-light/highlight.svg"]


class GoldenTest(unittest.TestCase):
    def test_outputs_match_goldens(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp)
            (out / "fcitx5-light").mkdir()
            requested = [o for o in OUTPUTS if not o.endswith(".svg")]
            subprocess.run([sys.executable, GENERATOR, FIXTURE, *(out / o for o in requested)], check=True)
            for name in OUTPUTS:
                golden = HERE / "golden" / name
                produced = (out / name).read_text()
                if os.environ.get("UPDATE_GOLDEN"):
                    golden.parent.mkdir(parents=True, exist_ok=True)
                    golden.write_text(produced)
                with self.subTest(name=name):
                    self.assertEqual(produced, golden.read_text())


if __name__ == "__main__":
    unittest.main()
