#pragma once

#include "wayland/settings_control.h"

#include <functional>
#include <string>
#include <string_view>

class Flex;

namespace settings {

  // What the "Add shortcut" row holds between rebuilds.
  struct ShortcutDraft {
    std::string action;
    std::string argument;
    std::string chord;
  };

  struct SettingsShortcutsContext {
    float scale = 1.0F;
    const SettingsControl* compositor = nullptr;
    ShortcutDraft& draft;
    // Chord of the row recording a replacement, or kDraftChordRow for the Add row; empty when none records.
    const std::string& recordingRow;
    std::function<void(std::string chord, std::string action)> bind;
    // Starts recording for `row`; the compositor reports the chord to `onCaptured`, empty when cancelled.
    std::function<void(std::string row, std::function<void(std::string)> onCaptured)> record;
    std::function<void()> cancelRecording;
    std::function<void()> requestRebuild;
  };

  inline constexpr std::string_view kDraftChordRow = "\n";

  void addSettingsShortcuts(Flex& content, const SettingsShortcutsContext& ctx);

} // namespace settings
