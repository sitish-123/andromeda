#include "app/command_table.h"

#include "core/log.h"

#include <algorithm>
#include <xkbcommon/xkbcommon.h>

namespace {
  constexpr Logger kLog("app-commands");

  // Shift turns "z" into "Z"; a chord written as Ctrl+Shift+Z must match either keysym.
  bool chordMatches(const KeyChord& chord, std::uint32_t sym, std::uint32_t modifiers) {
    return chord.modifiers == modifiers && xkb_keysym_to_lower(chord.sym) == xkb_keysym_to_lower(sym);
  }
} // namespace

void CommandTable::add(AppCommand command) {
  remove(command.id);
  if (command.shortcut.has_value()) {
    const KeyChord chord = *command.shortcut;
    const auto owner = std::ranges::find_if(m_commands, [&](const AppCommand& c) {
      return c.shortcut.has_value() && chordMatches(*c.shortcut, chord.sym, chord.modifiers);
    });
    if (owner != m_commands.end()) {
      kLog.warn("shortcut {} of {} is held by {}; dropped", keyChordToString(chord), command.id, owner->id);
      command.shortcut.reset();
    }
  }
  m_commands.push_back(std::move(command));
}

void CommandTable::remove(std::string_view id) {
  std::erase_if(m_commands, [&](const AppCommand& c) { return c.id == id; });
}

const AppCommand* CommandTable::find(std::string_view id) const {
  const auto it = std::ranges::find_if(m_commands, [&](const AppCommand& c) { return c.id == id; });
  return it == m_commands.end() ? nullptr : &*it;
}

bool CommandTable::isEnabled(std::string_view id) const {
  const AppCommand* command = find(id);
  return command != nullptr && (!command->enabled || command->enabled());
}

bool CommandTable::run(std::string_view id) const {
  const AppCommand* command = find(id);
  if (command == nullptr || !command->run || (command->enabled && !command->enabled())) {
    return false;
  }
  // Copy: the command may replace the table entry while it runs.
  const auto run = command->run;
  run();
  return true;
}

const AppCommand* CommandTable::match(std::uint32_t sym, std::uint32_t modifiers) const {
  for (const AppCommand& command : m_commands) {
    if (command.shortcut.has_value() && chordMatches(*command.shortcut, sym, modifiers)
        && (!command.enabled || command.enabled())) {
      return &command;
    }
  }
  return nullptr;
}

bool CommandTable::handleKey(std::uint32_t sym, std::uint32_t modifiers) const {
  const AppCommand* command = match(sym, modifiers);
  if (command == nullptr || !command->run) {
    return false;
  }
  // Copy: the command may replace the table entry while it runs.
  const auto run = command->run;
  run();
  return true;
}
