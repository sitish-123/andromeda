#include "shell/desktop_layout/desktop_layout.h"

#include "compositors/settings_control.h"
#include "render/scene/rect_node.h"
#include "ui/builders.h"
#include "shell/settings/settings_selection_style.h"
#include "ui/style.h"
#include "ui/palette.h"

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
    constexpr std::array<std::string_view, 2> kTaskbarCenter = {"start", "windows_taskbar"};
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
    constexpr std::array<std::string_view, 6> kTaskbarEnd = {"tray", "network", "output_volume", "battery", "clock", "show_desktop"};

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
      return std::ranges::all_of(compositorParts(p), [&](const auto& part) {
        return compositor->value(part.first) == part.second;
      });
    }

  } // namespace

  std::unique_ptr<Node> preview(std::string_view preset, float width, float height, float scale) {
    // Grid units: 64 x 40. Glass = white over the wallpaper; windows are light app windows.
    enum class Kind : std::uint8_t { Glass, Window, Faint };
    struct Shape {
      float x, y, w, h;
      Kind kind;
    };
    std::vector<Shape> shapes;
    if (preset == "andromeda") {
      shapes = {{3, 2, 10, 3, Kind::Glass},   {25, 2, 14, 3, Kind::Glass},  {51, 2, 10, 3, Kind::Glass},
                {-10, 8, 14, 24, Kind::Faint}, {8, 8, 23, 24, Kind::Window}, {34, 8, 23, 24, Kind::Window},
                {22, 35, 20, 3, Kind::Glass}};
    } else if (preset == "macos") {
      shapes = {{0, 0, 64, 3, Kind::Glass}, {8, 8, 30, 20, Kind::Window}, {25, 13, 31, 18, Kind::Window},
                {18, 35, 28, 3, Kind::Glass}};
    } else if (preset == "windows") {
      shapes = {{8, 5, 30, 21, Kind::Window}, {24, 10, 32, 20, Kind::Window}, {0, 36, 64, 4, Kind::Glass}};
    } else if (preset == "tiling") {
      shapes = {{3, 2, 10, 3, Kind::Glass},     {25, 2, 14, 3, Kind::Glass},     {51, 2, 10, 3, Kind::Glass},
                {4, 7, 26, 30, Kind::Window}, {33, 7, 26, 30, Kind::Window}, {62, 7, 12, 30, Kind::Faint}};
    }
    // Concentric with the picker's accent ring; windows round at half that, glass shapes are capsules.
    const float cardRadius = (settings::metrics::kPickerRadius - Style::focusRingWidth - Style::focusRingGap) * scale;
    auto card = ui::column({.radius = cardRadius, .clipChildren = true, .width = width, .height = height});
    // The default wallpaper's night blue to teal, as a horizontal sweep.
    const Color night = rgbHex(0x0D1630);
    const Color teal = rgbHex(0x14455A);
    auto wallpaper = std::make_unique<RectNode>();
    wallpaper->setParticipatesInLayout(false);
    wallpaper->setFrameSize(width, height);
    wallpaper->setStyle(RoundedRectStyle{
        .fill = night,
        .border = clearColor(),
        .fillMode = FillMode::LinearGradient,
        .gradientDirection = GradientDirection::Horizontal,
        .gradientStops = {GradientStop{0.0F, night}, GradientStop{0.55F, teal}, GradientStop{1.0F, night}},
        .radius = cardRadius,
        .softness = 1.0F,
        .borderWidth = 0.0F,
    });
    card->addChild(std::move(wallpaper));
    const float sx = width / 64.0F;
    const float sy = height / 40.0F;
    for (const Shape& shape : shapes) {
      auto part = std::make_unique<RectNode>();
      part->setParticipatesInLayout(false);
      part->setPosition(shape.x * sx, shape.y * sy);
      part->setFrameSize(shape.w * sx, shape.h * sy);
      const Color fill = shape.kind == Kind::Glass ? Color{1.0F, 1.0F, 1.0F, 0.42F}
          : shape.kind == Kind::Window           ? rgbHex(0xF2F2F6)
                                                 : Color{0.949F, 0.949F, 0.965F, 0.35F};
      part->setStyle(RoundedRectStyle{
          .fill = fill,
          .border = shape.kind == Kind::Glass ? Color{1.0F, 1.0F, 1.0F, 0.5F} : clearColor(),
          .radius = shape.kind == Kind::Glass ? std::min(shape.h * sy, shape.w * sx) / 2 : cardRadius / 2,
          .softness = 1.0F,
          .borderWidth = shape.kind == Kind::Glass ? 0.5F * scale : 0.0F,
      });
      card->addChild(std::move(part));
    }
    return card;
  }

  Snapshot snapshot(const Config& cfg, const SettingsControl* compositor) {
    Snapshot now{.bars = cfg.bars};
    if (compositor != nullptr && compositor->ready()) {
      now.cornerRadius = compositor->value("appearance.corner_radius");
      for (const auto& [key, setting] : compositor->settings()) {
        if (key.starts_with("workspace.") && key.ends_with(".layout.mode") && !compositor->value(key).empty()) {
          now.workspaceModes.push_back(key);
        }
      }
    }
    return now;
  }

  std::vector<Write> writes(std::string_view preset, const Snapshot& now) {
    const Preset* p = find(preset);
    if (p == nullptr) {
      return {};
    }
    std::vector<Write> out;
    for (const BarConfig& bar : now.bars) {
      out.emplace_back(Path{"bar", bar.name, "islands"}, p->islands);
    }
    if (!now.bars.empty()) {
      const std::string& first = now.bars.front().name;
      out.emplace_back(Path{"bar", first, "position"}, std::string(p->barPosition));
      if (preset == "windows") {
        const auto list = [](std::span<const std::string_view> names) {
          return std::vector<std::string>(names.begin(), names.end());
        };
        out.emplace_back(Path{"bar", first, "start"}, std::vector<std::string>{});
        out.emplace_back(Path{"bar", first, "center"}, list(kTaskbarCenter));
        out.emplace_back(Path{"bar", first, "end"}, list(kTaskbarEnd));
        for (const RowInt& field : kTaskbarGeometry) {
          out.emplace_back(Path{"bar", first, std::string(field.key)}, field.taskbar);
        }
      }
    }
    out.emplace_back(Path{"dock", "enabled"}, p->dock);
    for (const auto& [key, value] : compositorParts(*p)) {
      out.emplace_back(compositorPath(key), std::string(value));
    }
    if (!p->radius.empty()) {
      out.emplace_back(compositorPath("appearance.corner_radius"), std::string(p->radius));
    }
    return out;
  }

  std::vector<Path> clears(std::string_view preset, const Snapshot& now) {
    const Preset* p = find(preset);
    if (p == nullptr) {
      return {};
    }
    std::vector<Path> out;
    for (const std::string& key : now.workspaceModes) {
      out.push_back(compositorPath(key));
    }
    if (preset != "windows") {
      if (!now.bars.empty() && isTaskbarRow(now.bars.front())) {
        const std::string& first = now.bars.front().name;
        for (const std::string_view key : {"start", "center", "end"}) {
          out.push_back(Path{"bar", first, std::string(key)});
        }
        for (const RowInt& field : kTaskbarGeometry) {
          out.push_back(Path{"bar", first, std::string(field.key)});
        }
      }
      if (now.cornerRadius == find("windows")->radius) {
        out.push_back(compositorPath("appearance.corner_radius"));
      }
    }
    return out;
  }

  std::string current(const Config& cfg, const SettingsControl* compositor) {
    if (compositor == nullptr || !compositor->ready()) {
      return {};
    }
    const Snapshot now = snapshot(cfg, compositor);
    if (!now.workspaceModes.empty()) {
      return std::string(kCustom);
    }
    for (const std::string_view name : kPresets) {
      if (matches(name, *find(name), cfg, compositor)) {
        return std::string(name);
      }
    }
    return std::string(kCustom);
  }

} // namespace desktop_layout
