#include "app/command_table.h"
#include "core/input/key_modifiers.h"

#include <print>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  int g_failures = 0;

  void expect(bool ok, const char* what) {
    if (!ok) {
      std::println(stderr, "command_table_test: {}", what);
      ++g_failures;
    }
  }
} // namespace

int main() {
  CommandTable table;
  int undo = 0;
  int redo = 0;
  bool canRedo = false;
  table.add({.id = "edit.undo", .label = "Undo", .shortcut = KeyChord{XKB_KEY_z, KeyMod::Ctrl}, .run = [&] { ++undo; }});
  table.add({
      .id = "edit.redo",
      .label = "Redo",
      .shortcut = KeyChord{XKB_KEY_Z, KeyMod::Ctrl | KeyMod::Shift},
      .enabled = [&] { return canRedo; },
      .run = [&] { ++redo; },
  });

  expect(table.handleKey(XKB_KEY_z, KeyMod::Ctrl) && undo == 1, "Ctrl+Z runs undo");
  expect(!table.handleKey(XKB_KEY_z, KeyMod::Alt), "Alt+Z matches nothing");
  // With Shift held the keysym arrives as "Z" or "z" depending on the layout path; both are the same chord.
  expect(!table.handleKey(XKB_KEY_Z, KeyMod::Ctrl | KeyMod::Shift) && redo == 0, "a disabled command takes no key");
  canRedo = true;
  expect(table.handleKey(XKB_KEY_z, KeyMod::Ctrl | KeyMod::Shift) && redo == 1, "Ctrl+Shift+z runs redo");
  expect(!table.handleKey(XKB_KEY_z, KeyMod::Ctrl | KeyMod::Shift | KeyMod::Alt), "extra modifiers do not match");

  table.add({.id = "file.zap", .label = "Zap", .shortcut = KeyChord{XKB_KEY_Z, KeyMod::Ctrl}, .run = [] {}});
  expect(table.find("file.zap") != nullptr && !table.find("file.zap")->shortcut.has_value(),
         "a chord already held is dropped from the later command");
  expect(table.handleKey(XKB_KEY_z, KeyMod::Ctrl) && undo == 2, "the first owner keeps the chord");

  table.add({.id = "edit.undo", .label = "Undo Typing", .shortcut = KeyChord{XKB_KEY_z, KeyMod::Ctrl}, .run = [&] { undo += 10; }});
  expect(table.find("edit.undo")->label == "Undo Typing", "same id replaces the command");
  expect(table.handleKey(XKB_KEY_z, KeyMod::Ctrl) && undo == 12, "the replacement owns its chord again");

  expect(!table.run("missing"), "an unknown id does not run");
  canRedo = false;
  expect(!table.run("edit.redo") && !table.isEnabled("edit.redo"), "run() honours enabled");

  return g_failures == 0 ? 0 : 1;
}
