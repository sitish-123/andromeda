#pragma once

// The one action taxonomy: `andromeda msg --help` and docs/actions.md group actions by it.

#include "config/keybind_parse.h"

#include <span>

namespace andromeda {

  enum class Group : int {
    Apps = 0,
    Screencasting,
    Focus,
    MoveSize,
    Windows,
    Scratchpad,
    Workspaces,
    Overview,
    System,
  };

  [[nodiscard]] const char* groupTitle(Group group);
  [[nodiscard]] Group groupForAction(KeybindAction action);
  [[nodiscard]] std::span<const Group> fixedGroupOrder();

} // namespace andromeda
