#include "shell/desktop_layout/desktop_layout.h"

#include "compositors/settings_control.h"

#include <algorithm>
#include <span>

namespace desktop_layout {

  namespace {

    struct Preset {
      bool islands;
      std::string_view barPosition;
      bool dock;
      std::string_view arrangement; // compositor layout.mode
      std::string_view overview;    // compositor overview.style
      std::string_view titleBar;    // compositor appearance.title_bar.style
      std::string_view buttons;     // compositor appearance.title_bar.button_position
      std::string_view radius;      // compositor appearance.corner_radius; empty = the default
    };

    // macOS and Windows use a full-width bar until their own top bar (DL5) and taskbar (DL2) exist.
    const Preset* find(std::string_view name) {
      static const Preset andromeda{true, "top", true, "scrolling", "spaces", "andromeda", "right", ""};
      static const Preset macos{false, "top", true, "floating", "spaces", "macos", "left", ""};
      static const Preset windows{false, "bottom", false, "floating", "taskview", "windows", "right", "8"};
      static const Preset tiling{true, "top", false, "scrolling", "grid", "andromeda", "right", ""};
      if (name == "andromeda") {
        return &andromeda;
      }
      if (name == "macos") {
        return &macos;
      }
      if (name == "windows") {
        return &windows;
      }
      if (name == "tiling") {
        return &tiling;
      }
      return nullptr;
    }

    // Windows (prototype .tbar2): a 48 px row with Start and the window buttons centred; tray, clock and Show Desktop
    // trail.
    constexpr std::array<std::string_view, 2> kTaskbarCenter = {"start", "taskbar"};
    // The row's own geometry: flush to the screen edge and both ends, square, with Show Desktop at the very end.
    struct RowInt {
      std::string_view key;
      std::int32_t BarConfig::* member;
      std::int64_t taskbar;
    };
    constexpr std::array<RowInt, 8> kTaskbarGeometry{{
        {"thickness", &BarConfig::thickness, 48},
        {"margin_ends", &BarConfig::marginEnds, 0},
        {"margin_edge", &BarConfig::marginEdge, 0},
        {"padding", &BarConfig::padding, 0},
        {"radius_top_left", &BarConfig::radiusTopLeft, 0},
        {"radius_top_right", &BarConfig::radiusTopRight, 0},
        {"radius_bottom_left", &BarConfig::radiusBottomLeft, 0},
        {"radius_bottom_right", &BarConfig::radiusBottomRight, 0},
    }};
    constexpr std::array<std::string_view, 3> kTaskbarEnd = {"tray", "clock", "show_desktop"};

    bool isTaskbarRow(const BarConfig& bar) {
      return bar.startWidgets.empty() && std::ranges::equal(bar.centerWidgets, kTaskbarCenter)
          && std::ranges::equal(bar.endWidgets, kTaskbarEnd);
    }

    std::vector<std::pair<std::string, std::string_view>> compositorParts(const Preset& p) {
      return {
          {"layout.mode", p.arrangement},
          {"overview.style", p.overview},
          {"appearance.title_bar.style", p.titleBar},
          {"appearance.title_bar.button_position", p.buttons},
      };
    }

    std::vector<std::string> compositorPath(std::string_view key) {
      std::vector<std::string> path{"andromeda"};
      size_t start = 0;
      while (start <= key.size()) {
        const size_t end = std::min(key.find('.', start), key.size());
        path.emplace_back(key.substr(start, end - start));
        start = end + 1;
      }
      return path;
    }

    bool matches(std::string_view name, const Preset& p, const Config& cfg, const SettingsControl* compositor) {
      if (!cfg.bars.empty() && isTaskbarRow(cfg.bars.front()) != (name == "windows")) {
        return false;
      }
      const bool bars = std::ranges::all_of(cfg.bars, [&](const BarConfig& bar) { return bar.islands == p.islands; })
          && (cfg.bars.empty() || cfg.bars.front().position == p.barPosition);
      if (!bars || cfg.dock.enabled != p.dock) {
        return false;
      }
      if (compositor == nullptr || !compositor->ready()) {
        return true;
      }
      return std::ranges::all_of(compositorParts(p), [&](const auto& part) {
        return compositor->value(part.first) == part.second;
      });
    }

  } // namespace

  std::vector<Shape> drawing(std::string_view preset) {
    if (preset == "andromeda") {
      return {{4, 2, 10, 3}, {24, 2, 16, 3}, {50, 2, 10, 3}, {-8, 8, 12, 23, true}, {8, 8, 24, 23}, {36, 8, 24, 23},
              {20, 34, 24, 4}};
    }
    if (preset == "macos") {
      return {{0, 0, 64, 4}, {8, 9, 30, 20}, {26, 14, 30, 17}, {16, 34, 32, 4}};
    }
    if (preset == "windows") {
      return {{8, 5, 30, 21}, {24, 10, 32, 20}, {0, 35, 64, 5}, {2, 36, 5, 3, true}};
    }
    if (preset == "tiling") {
      return {{4, 2, 10, 3}, {24, 2, 16, 3}, {50, 2, 10, 3}, {4, 8, 26, 29}, {33, 8, 26, 29}, {62, 8, 12, 29, true}};
    }
    return {};
  }

  std::vector<Write> writes(std::string_view preset, const std::vector<BarConfig>& bars) {
    const Preset* p = find(preset);
    if (p == nullptr) {
      return {};
    }
    std::vector<Write> out;
    for (const BarConfig& bar : bars) {
      out.emplace_back(std::vector<std::string>{"bar", bar.name, "islands"}, p->islands);
    }
    if (!bars.empty()) {
      const BarConfig& first = bars.front();
      out.emplace_back(std::vector<std::string>{"bar", first.name, "position"}, std::string(p->barPosition));
      // The taskbar row is set by Windows and put back to the defaults only when it is still the taskbar row, so
      // other picks keep the user's own widgets.
      const bool windows = preset == "windows";
      if (windows || isTaskbarRow(first)) {
        const BarConfig defaults;
        const auto list = [](std::span<const std::string_view> names) {
          return std::vector<std::string>(names.begin(), names.end());
        };
        out.emplace_back(std::vector<std::string>{"bar", first.name, "start"},
                         windows ? std::vector<std::string>{} : defaults.startWidgets);
        out.emplace_back(std::vector<std::string>{"bar", first.name, "center"},
                         windows ? list(kTaskbarCenter) : defaults.centerWidgets);
        for (const RowInt& field : kTaskbarGeometry) {
          out.emplace_back(std::vector<std::string>{"bar", first.name, std::string(field.key)},
                           windows ? field.taskbar : std::int64_t{defaults.*field.member});
        }
        out.emplace_back(std::vector<std::string>{"bar", first.name, "end"},
                         windows ? list(kTaskbarEnd) : defaults.endWidgets);
      }
    }
    out.emplace_back(std::vector<std::string>{"dock", "enabled"}, p->dock);
    for (const auto& [key, value] : compositorParts(*p)) {
      out.emplace_back(compositorPath(key), std::string(value));
    }
    out.emplace_back(compositorPath("appearance.corner_radius"), std::string(p->radius));
    return out;
  }

  std::string current(const Config& cfg, const SettingsControl* compositor) {
    for (const std::string_view name : kPresets) {
      if (matches(name, *find(name), cfg, compositor)) {
        return std::string(name);
      }
    }
    return std::string(kCustom);
  }

} // namespace desktop_layout
