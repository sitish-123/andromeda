#pragma once

#include "compositors/settings_control.h"

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

class Flex;
class Node;
struct DesktopEntry;

namespace settings {

  // Page state kept between rebuilds: the selected category, the search or press lookup, and the add form.
  struct ShortcutDraft {
    std::string category = "apps";
    bool pressMode = false;
    std::string query;
    std::string pressChord;
    bool adding = false;
    bool addCommand = false; // Custom: run a command instead of opening an app.
    std::string addQuery;
    std::string addAction; // Full compositor action, "spawn:<cmd>" or "<action>[:<arg>]".
    std::string addTitle;
    std::string addCommandText;
    std::string chord;
  };

  struct SettingsShortcutsContext {
    float scale = 1.0F;
    const SettingsControl* compositor = nullptr;
    ShortcutDraft& draft;
    // Chord of the row recording a replacement, or one of the k*Row ids below; empty when none records.
    const std::string& recordingRow;
    std::span<const DesktopEntry> entries;
    // An app icon of `size` px for the entry; null when it has none.
    std::function<std::unique_ptr<Node>(const DesktopEntry&, float size)> appIcon;
    std::function<void(std::string chord, std::string action)> bind;
    // Starts recording for `row`; the compositor reports the chord to `onCaptured`, empty when cancelled.
    std::function<void(std::string row, std::function<void(std::string)> onCaptured)> record;
    std::function<void()> cancelRecording;
    std::function<void()> requestRebuild;
  };

  inline constexpr std::string_view kDraftChordRow = "\n";
  inline constexpr std::string_view kPressChordRow = "\n?";

  void addSettingsShortcuts(Flex& content, const SettingsShortcutsContext& ctx);

} // namespace settings
