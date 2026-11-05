#pragma once

#include "core/input/key_chord.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// One table of app commands. The window keys, the menu, the launcher's action panel and the a11y tree all read
// this table, so a command has one label, one shortcut and one enabled rule everywhere.
struct AppCommand {
  std::string id;                  // stable, dotted: "file.copy", "view.toggle-sidebar"
  std::string label;               // translated, shown in menus
  std::string group;               // menu section: "File", "Edit", "View", "Go", "Window"
  std::optional<KeyChord> shortcut;
  std::function<bool()> enabled;   // empty = always enabled
  std::function<void()> run;
};

class CommandTable {
public:
  // Replaces a command with the same id. A shortcut that another command already holds is dropped from the new
  // command (logged): the first registration owns the chord.
  void add(AppCommand command);
  void remove(std::string_view id);

  [[nodiscard]] const AppCommand* find(std::string_view id) const;
  [[nodiscard]] bool isEnabled(std::string_view id) const;
  // Runs an enabled command. False when the id is unknown or the command is disabled.
  bool run(std::string_view id) const;
  // The enabled command whose shortcut matches the key, or nullptr.
  [[nodiscard]] const AppCommand* match(std::uint32_t sym, std::uint32_t modifiers) const;
  // Matches and runs; true when a command took the key.
  bool handleKey(std::uint32_t sym, std::uint32_t modifiers) const;

  [[nodiscard]] const std::vector<AppCommand>& commands() const noexcept { return m_commands; }

private:
  std::vector<AppCommand> m_commands;
};
