#pragma once

#include "config/config_types.h"

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class SettingsControl;

// Desktop Layouts (DESIGN.md "Desktop Layouts"): one pick sets the bar, dock, window arrangement, overview style and
// title bars together. The current layout is read back from those parts, so a later change to any part shows Custom.
namespace desktop_layout {

  inline constexpr std::array<std::string_view, 4> kPresets = {"andromeda", "macos", "windows", "tiling"};
  inline constexpr std::string_view kCustom = "custom";

  // A preset card drawing on a 64 x 40 grid: bar pills or a full-width bar or taskbar, windows, the dock.
  struct Shape {
    float x, y, w, h;
    bool faint = false;
  };
  [[nodiscard]] std::vector<Shape> drawing(std::string_view preset);

  // Settings paths: shell keys as in the config, compositor keys under the "andromeda" root.
  using Write = std::pair<std::vector<std::string>, ConfigOverrideValue>;

  // The writes that apply `preset` to `bars` (the first one moves edge and, for Windows, becomes the taskbar row);
  // empty for an unknown name.
  [[nodiscard]] std::vector<Write> writes(std::string_view preset, const std::vector<BarConfig>& bars);
  // The preset whose parts all match, else kCustom. Without the compositor's settings only the shell parts count.
  [[nodiscard]] std::string current(const Config& cfg, const SettingsControl* compositor);

} // namespace desktop_layout
