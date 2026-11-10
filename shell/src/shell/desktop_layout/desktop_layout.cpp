#include "shell/desktop_layout/desktop_layout.h"

#include "compositors/settings_control.h"
#include "i18n/i18n.h"
#include "render/scene/rect_node.h"
#include "ui/builders.h"
#include "shell/settings/settings_selection_style.h"
#include "ui/style.h"
#include "ui/palette.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <span>

namespace desktop_layout {

  namespace {

    struct Preset {
      bool islands;
      std::string_view barPosition;
      bool dock;
      std::string_view arrangement; // compositor layout.mode
      std::string_view overview;    // compositor overview.style
      std::string_view titleBarMode; // compositor appearance.title_bar.mode
      std::string_view titleBar;    // compositor appearance.title_bar.style
      std::string_view buttons;     // compositor appearance.title_bar.button_position
      std::string_view radius;      // compositor appearance.corner_radius; empty = the default
    };

    // macOS and Windows use a full-width bar until their own top bar (DL5) and taskbar (DL2) exist.
    const Preset* find(std::string_view name) {
      static const Preset andromeda{true, "top", true, "scrolling", "spaces", "auto", "andromeda", "right", ""};
      static const Preset macos{false, "top", true, "floating", "spaces", "on", "macos", "left", ""};
      static const Preset windows{false, "bottom", false, "floating", "taskview", "on", "windows", "right", "8"};
      static const Preset tiling{true, "top", false, "scrolling", "grid", "auto", "andromeda", "right", ""};
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

    // A preset's own full-width bar row (board desktop-layouts.html sections 1 and 7): its lanes and thickness; the
    // row is flush to the screen edge and both ends, square.
    struct BarRow {
      std::vector<std::string_view> start;
      std::vector<std::string_view> center;
      std::vector<std::string_view> end;
      std::int64_t thickness;
      std::optional<double> solidOpacity; // bar solid_opacity; unset keeps glass
      std::int64_t padding;       // the leading end
      std::int64_t paddingEnd;    // the trailing end
      std::int64_t widgetSpacing; // between widgets in a lane
    };
    struct RowInt {
      std::string_view key;
      std::int32_t BarConfig::* member;
      std::int64_t value; // thickness, padding and widget_spacing take the BarRow's value instead
    };
    constexpr std::array<RowInt, 10> kRowGeometry{{
        {"thickness", &BarConfig::thickness, 0},
        {"margin_ends", &BarConfig::marginEnds, 0},
        {"margin_edge", &BarConfig::marginEdge, 0},
        {"padding", &BarConfig::padding, 0},
        {"padding_end", &BarConfig::paddingEnd, 0},
        {"radius_top_left", &BarConfig::radiusTopLeft, 0},
        {"radius_top_right", &BarConfig::radiusTopRight, 0},
        {"radius_bottom_left", &BarConfig::radiusBottomLeft, 0},
        {"radius_bottom_right", &BarConfig::radiusBottomRight, 0},
        {"widget_spacing", &BarConfig::widgetSpacing, 0},
    }};

    // Windows: Start and the window buttons centred; tray, one hover cell for Wi-Fi + volume + battery, the two-line
    // clock, the notifications bell, the Show Desktop sliver. macOS: brand menu and the bold app name leading, the
    // same status cell and the clock trailing. Insets and gaps: Windows lanes 4 apart with Show Desktop flush right;
    // macOS 12 from the leading edge, 16 from the trailing edge and between items.
    const BarRow* rowFor(std::string_view preset) {
      static const BarRow windows{
          {}, {"start", "windows_taskbar"}, {"tray", "group:status", "windows_clock", "notifications", "show_desktop"}, 48,
          std::nullopt, 0, 0, 4
      };
      static const BarRow macos{
          {"brand", "macos_app"}, {}, {"group:status", "macos_clock", "notifications"}, 32, std::nullopt, 12, 16, 16
      };
      return preset == "windows" ? &windows : preset == "macos" ? &macos : nullptr;
    }

    std::span<const std::string_view> capsuledWidgets(std::string_view preset) {
      static constexpr std::array<std::string_view, 2> kWindows{"windows_clock", "notifications"};
      return preset == "windows" ? std::span<const std::string_view>(kWindows) : std::span<const std::string_view>{};
    }

    BarCapsuleGroupStyle statusGroup() {
      BarCapsuleGroupStyle group;
      group.id = "status";
      group.members = {"status_network", "status_volume", "status_battery"};
      group.fill = clearColorSpec();
      group.radius = Style::spaceSm;
      // Each icon opens its own page, so each lights alone.
      group.hoverAsOne = false;
      return group;
    }

    bool isRow(const BarConfig& bar, const BarRow& row) {
      return std::ranges::equal(bar.startWidgets, row.start) && std::ranges::equal(bar.centerWidgets, row.center)
          && std::ranges::equal(bar.endWidgets, row.end);
    }

    // The preset row the bar is set up as, if any.
    const BarRow* currentRow(const BarConfig& bar) {
      for (const std::string_view name : {"windows", "macos"}) {
        if (isRow(bar, *rowFor(name))) {
          return rowFor(name);
        }
      }
      return nullptr;
    }

    std::vector<std::pair<std::string, std::string_view>> compositorParts(const Preset& p) {
      return {
          {"layout.mode", p.arrangement},
          {"overview.style", p.overview},
          {"appearance.title_bar.mode", p.titleBarMode},
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

    // macOS and Windows show desktop icons, from their own corner (DESIGN.md "Desktop icons").
    bool hasDesktopIcons(std::string_view name) { return name == "macos" || name == "windows"; }

    bool matches(std::string_view name, const Preset& p, const Config& cfg, const SettingsControl* compositor) {
      if (!cfg.bars.empty() && currentRow(cfg.bars.front()) != rowFor(name)) {
        return false;
      }
      const bool bars = std::ranges::all_of(cfg.bars, [&](const BarConfig& bar) { return bar.islands == p.islands; })
          && (cfg.bars.empty() || cfg.bars.front().position == p.barPosition);
      // Desktop icons are a choice of their own (Settings › Desktop and Dock): a layout sets them, but turning them
      // on or off later keeps the layout.
      if (!bars || cfg.dock.enabled != p.dock) {
        return false;
      }
      return std::ranges::all_of(compositorParts(p), [&](const auto& part) {
        return compositor->value(part.first) == part.second;
      });
    }

  } // namespace

  namespace {
    Snapshot& customMix() {
      static Snapshot mix;
      return mix;
    }
  } // namespace

  void setCustomMix(const Snapshot& now) { customMix() = now; }

  std::string customMixLabel() {
    const Snapshot& mix = customMix();
    std::vector<std::string> parts;
    const auto add = [&parts](std::string part) {
      if (std::ranges::find(parts, part) == parts.end()) {
        parts.push_back(std::move(part));
      }
    };
    for (const BarConfig& bar : mix.bars) {
      const BarRow* row = currentRow(bar);
      add(i18n::tr(
          row == rowFor("windows") ? "settings.desktop-layout.mix.taskbar"
              : row == rowFor("macos") ? "settings.desktop-layout.mix.top-bar"
              : bar.islands            ? "settings.desktop-layout.mix.islands"
                                       : "settings.desktop-layout.mix.bar"
      ));
    }
    if (mix.dock) {
      add(i18n::tr("settings.desktop-layout.mix.dock"));
    }
    std::string label;
    for (const std::string& part : parts) {
      label += (label.empty() ? "" : " + ") + part;
    }
    if (!label.empty()) {
      label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
    }
    return label;
  }

  std::unique_ptr<Node> preview(std::string_view preset, float width, float height, float scale) {
    // Board desktop-layouts.html section 6 pics, in its 200 x 124 px: glass shapes over the wallpaper, app windows.
    enum class Kind : std::uint8_t { Glass, Window };
    struct Shape {
      float x, y, w, h, r;
      Kind kind;
    };
    std::vector<Shape> shapes;
    const auto island = [](float x, float y) { return Shape{x, y, 36, 8, 4, Kind::Glass}; };
    if (preset == "andromeda") {
      shapes = {island(8, 6), {76, 6, 48, 8, 4, Kind::Glass}, island(156, 6), {24, 24, 72, 68, 4, Kind::Window},
                {104, 24, 72, 68, 4, Kind::Window}, {68, 102, 64, 16, 6, Kind::Glass}};
    } else if (preset == "macos") {
      shapes = {{0, 0, 200, 8, 0, Kind::Glass},     {28, 20, 96, 64, 4, Kind::Window}, {84, 40, 88, 56, 4, Kind::Window},
                {52, 102, 96, 16, 6, Kind::Glass}, {180, 16, 12, 12, 4, Kind::Glass}};
    } else if (preset == "windows") {
      shapes = {{24, 16, 100, 68, 4, Kind::Window}, {84, 36, 92, 60, 4, Kind::Window}, {0, 112, 200, 12, 0, Kind::Glass},
                {8, 8, 12, 12, 4, Kind::Glass}};
    } else if (preset == kCustom) {
      const Snapshot& mix = customMix();
      const BarConfig* bar = mix.bars.empty() ? nullptr : &mix.bars.front();
      const bool bottom = bar != nullptr && bar->position == "bottom";
      shapes = {{24, 16, 100, 68, 4, Kind::Window}, {84, 36, 92, 60, 4, Kind::Window}};
      if (bar != nullptr && bar->islands) {
        const float y = bottom ? 110 : 6;
        shapes.push_back(island(8, y));
        shapes.push_back({76, y, 48, 8, 4, Kind::Glass});
        shapes.push_back(island(156, y));
      } else if (bar != nullptr) {
        shapes.push_back(bottom ? Shape{0, 112, 200, 12, 0, Kind::Glass} : Shape{0, 0, 200, 8, 0, Kind::Glass});
      }
      if (mix.dock) {
        shapes.push_back({68, bottom ? 88.0F : 102.0F, 64, 16, 6, Kind::Glass});
      }
    } else if (preset == "tiling") {
      shapes = {island(8, 6), island(156, 6), {8, 20, 88, 96, 4, Kind::Window}, {104, 20, 88, 44, 4, Kind::Window},
                {104, 72, 88, 44, 4, Kind::Window}};
    }
    const bool light = isLightPalette();
    // Concentric with the picker's accent ring.
    const float cardRadius = (settings::metrics::kPickerRadius - Style::focusRingWidth - Style::focusRingGap) * scale;
    auto card = ui::column({.radius = cardRadius, .clipChildren = true, .width = width, .height = height});
    // The default wallpaper's night blue to teal, as a horizontal sweep, under a 0.5 px tokens.css --edge hairline.
    const Color night = rgbHex(0x0D1630);
    const Color teal = rgbHex(0x14455A);
    auto wallpaper = std::make_unique<RectNode>();
    wallpaper->setParticipatesInLayout(false);
    wallpaper->setFrameSize(width, height);
    wallpaper->setStyle(RoundedRectStyle{
        .fill = night,
        .border = Color{1.0F, 1.0F, 1.0F, light ? 0.749F : 0.122F},
        .fillMode = FillMode::LinearGradient,
        .gradientDirection = GradientDirection::Horizontal,
        .gradientStops = {GradientStop{0.0F, night}, GradientStop{0.55F, teal}, GradientStop{1.0F, night}},
        .radius = cardRadius,
        .softness = 1.0F,
        .borderWidth = 0.5F * scale,
    });
    card->addChild(std::move(wallpaper));
    const float sx = width / 200.0F;
    const float sy = height / 124.0F;
    const Color glass = light ? Color{1.0F, 1.0F, 1.0F, 0.62F} : Color{60 / 255.0F, 60 / 255.0F, 68 / 255.0F, 0.78F};
    const Color window = light ? Color{1.0F, 1.0F, 1.0F, 0.92F} : Color{48 / 255.0F, 48 / 255.0F, 54 / 255.0F, 0.95F};
    for (const Shape& shape : shapes) {
      const float x = shape.x * sx;
      const float y = shape.y * sy;
      const float w = shape.w * sx;
      const float h = shape.h * sy;
      if (shape.kind == Kind::Window) {
        // Board shadow 0 2 6 black 25 %.
        auto shadow = std::make_unique<RectNode>();
        shadow->setParticipatesInLayout(false);
        shadow->setPosition(x, y + 2.0F * sy);
        shadow->setFrameSize(w, h);
        shadow->setStyle(RoundedRectStyle{
            .fill = Color{0.0F, 0.0F, 0.0F, 0.25F},
            .border = clearColor(),
            .radius = shape.r * sx,
            .softness = 6.0F * sx,
            .borderWidth = 0.0F,
        });
        card->addChild(std::move(shadow));
      }
      auto part = std::make_unique<RectNode>();
      part->setParticipatesInLayout(false);
      part->setPosition(x, y);
      part->setFrameSize(w, h);
      part->setStyle(RoundedRectStyle{
          .fill = shape.kind == Kind::Glass ? glass : window,
          .border = clearColor(),
          .radius = shape.r * sx,
          .softness = 1.0F,
          .borderWidth = 0.0F,
      });
      card->addChild(std::move(part));
    }
    return card;
  }

  Snapshot snapshot(const Config& cfg, const SettingsControl* compositor) {
    Snapshot now{.bars = cfg.bars, .dock = cfg.dock.enabled};
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
      if (const BarRow* row = rowFor(preset)) {
        const auto list = [](std::span<const std::string_view> names) {
          return std::vector<std::string>(names.begin(), names.end());
        };
        out.emplace_back(Path{"bar", first, "start"}, list(row->start));
        out.emplace_back(Path{"bar", first, "center"}, list(row->center));
        out.emplace_back(Path{"bar", first, "end"}, list(row->end));
        out.emplace_back(Path{"bar", first, "capsule_group"}, std::vector<BarCapsuleGroupStyle>{statusGroup()});
        if (row->solidOpacity) {
          out.emplace_back(Path{"bar", first, "solid_opacity"}, *row->solidOpacity);
        }
        for (const RowInt& field : kRowGeometry) {
          const std::int64_t value = field.key == "thickness" ? row->thickness
              : field.key == "padding"                    ? row->padding
              : field.key == "padding_end"                ? row->paddingEnd
              : field.key == "widget_spacing"             ? row->widgetSpacing
                                                          : field.value;
          out.emplace_back(Path{"bar", first, std::string(field.key)}, value);
        }
        // A clear capsule reserves capsule_padding on both sides: the mockup's wider clock and bell gaps.
        for (const std::string_view widget : capsuledWidgets(preset)) {
          out.emplace_back(Path{"widget", std::string(widget), "capsule"}, true);
          out.emplace_back(Path{"widget", std::string(widget), "capsule_fill"}, std::string("#00000000"));
        }
        // Status flyouts float with a gap off a full-width bar, as on macOS and Windows.
        out.emplace_back(Path{"shell", "panel", "control_center_placement"}, std::string("floating"));
      }
    }
    out.emplace_back(Path{"dock", "enabled"}, p->dock);
    if (hasDesktopIcons(preset)) {
      out.emplace_back(Path{"desktop_icons", "start_corner"}, std::string(preset == "macos" ? "top_right" : "top_left"));
    }
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
    // Leaving a preset row (to another preset or to none) takes its lanes, group and geometry away.
    const BarRow* row = now.bars.empty() ? nullptr : currentRow(now.bars.front());
    if (row != nullptr && row != rowFor(preset)) {
      const std::string& first = now.bars.front().name;
      for (const std::string_view key : {"start", "center", "end", "capsule_group", "solid_opacity"}) {
        out.push_back(Path{"bar", first, std::string(key)});
      }
      for (const RowInt& field : kRowGeometry) {
        out.push_back(Path{"bar", first, std::string(field.key)});
      }
      out.push_back(Path{"shell", "panel", "control_center_placement"});
      for (const std::string_view widget : capsuledWidgets(row == rowFor("windows") ? "windows" : "")) {
        out.push_back(Path{"widget", std::string(widget), "capsule"});
        out.push_back(Path{"widget", std::string(widget), "capsule_fill"});
      }
    }
    if (!hasDesktopIcons(preset)) {
      out.push_back(Path{"desktop_icons", "start_corner"});
    }
    if (preset != "windows" && now.cornerRadius == find("windows")->radius) {
      out.push_back(compositorPath("appearance.corner_radius"));
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
