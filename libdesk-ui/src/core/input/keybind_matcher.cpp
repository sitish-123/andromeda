#include "core/input/keybind_matcher.h"

#include "core/input/key_chord.h"
#include "core/input/key_modifiers.h"

#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  std::array<std::optional<KeybindMatcher::Matcher>, 11> g_matchers{};

  // The standard keys, for processes that install no matchers from a config (apps; the shell sets its own).
  bool standard(KeybindAction action, std::uint32_t sym, std::uint32_t modifiers) {
    const bool plain = modifiers == 0;
    switch (action) {
    case KeybindAction::Validate:
      return plain && (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter);
    case KeybindAction::Cancel:
      return plain && sym == XKB_KEY_Escape;
    case KeybindAction::Left:
      return plain && sym == XKB_KEY_Left;
    case KeybindAction::Right:
      return plain && sym == XKB_KEY_Right;
    case KeybindAction::Up:
      return plain && sym == XKB_KEY_Up;
    case KeybindAction::Down:
      return plain && sym == XKB_KEY_Down;
    case KeybindAction::TabNext:
    case KeybindAction::TabPrevious:
      return false; // InputDispatcher cycles focus on these before the focused field sees Tab
    case KeybindAction::Delete:
      return plain && sym == XKB_KEY_Delete;
    case KeybindAction::Copy:
      return modifiers == KeyMod::Ctrl && (sym == XKB_KEY_c || sym == XKB_KEY_C);
    case KeybindAction::Save:
      return modifiers == KeyMod::Ctrl && (sym == XKB_KEY_s || sym == XKB_KEY_S);
    }
    return false;
  }

} // namespace

namespace KeybindMatcher {

  void setMatcher(KeybindAction action, Matcher matcher) {
    auto idx = static_cast<std::size_t>(action);
    if (idx < g_matchers.size()) {
      g_matchers[idx] = std::move(matcher);
    }
  }

  bool matches(KeybindAction action, std::uint32_t sym, std::uint32_t modifiers) {
    if (action != KeybindAction::Validate && isPrintableKey(sym) && modifiers == 0) {
      return false;
    }
    auto idx = static_cast<std::size_t>(action);
    if (idx < g_matchers.size() && g_matchers[idx]) {
      return (*g_matchers[idx])(sym, modifiers);
    }
    return standard(action, sym, modifiers);
  }

} // namespace KeybindMatcher
