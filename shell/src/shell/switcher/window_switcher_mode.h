#pragma once

#include <cstdint>

// Pointer: opened without a held shortcut. Shortcut: releasing the shortcut modifier confirms the selection.
// Closing: the Return runs; the surface goes when it settles.
enum class WindowSwitcherMode : std::uint8_t { Hidden, Pointer, Shortcut, Closing };

// Hidden and Closing open as Pointer, an open mode takes the shortcut or starts Closing, and every mode can hide.
[[nodiscard]] constexpr bool windowSwitcherModeAllowed(WindowSwitcherMode from, WindowSwitcherMode to) noexcept {
  using enum WindowSwitcherMode;
  const bool open = from == Pointer || from == Shortcut;
  switch (to) {
  case Hidden:
    return true;
  case Pointer:
    return from == Hidden || from == Closing;
  case Shortcut:
  case Closing:
    return open;
  }
  return false;
}
