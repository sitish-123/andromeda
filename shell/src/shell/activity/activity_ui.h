#pragma once

#include "i18n/i18n.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/graph.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <format>
#include <memory>
#include <string>
#include <vector>

// Shared parts of the Activity window files (DESIGN.md "Activity v3"); sizes in design units, times the ui scale.
namespace activity_ui {

  inline const float kWindowWidth = Tokens::spaceP8 * 142.0F;
  inline const float kWindowHeight = Tokens::spaceP48 * 15.0F;
  inline const float kWindowMinWidth = Tokens::spaceP40 * 19.0F;
  inline const float kWindowMinHeight = Tokens::spaceP48 * 10.0F;
  inline const float kToolbarHeight = Tokens::spaceP4 * 13.0F;
  inline const float kSearchWidth = Tokens::spaceP8 * 25.0F;
  inline const float kInspectorWidth = Tokens::spaceP48 * 7.0F;
  inline const float kStripHeight = Tokens::spaceP8 * 19.0F;
  inline const float kStripGraphHeight = Tokens::spaceP8 * 9.0F;
  inline const float kStripGridWidth = Tokens::spaceP4 * 105.0F;
  inline const float kStripGridCpuWidth = Tokens::spaceP40 * 9.0F;
  inline const float kStripMinWindowHeight = Tokens::spaceP64 * 10.0F;
  inline const float kNameMinWidth = Tokens::spaceP48 * 5.0F;
  inline const float kColumnWidth = Tokens::spaceP48 * 2.0F;
  inline const float kColumnMaxWidth = Tokens::spaceP40 * 4.0F;
  inline const float kNameMaxWidth = Tokens::spaceP48 * 10.0F;
  inline constexpr const float& kStatusWidth = Tokens::sizeControlM;
  inline const float kChildIndent = Tokens::spaceP12 * 3.0F;
  inline constexpr const float& kRowHeight = Tokens::sizeRowCompact;
  inline constexpr const float& kInspectorIcon = Tokens::sizeControlL;
  inline constexpr const float& kMiniGraphHeight = Tokens::sizeControlXl;
  inline constexpr float kHeatAlpha = 0.26F;
  inline constexpr int kSkeletonRows = 6;
  inline constexpr auto kSkeletonFor = std::chrono::milliseconds(300);
  inline constexpr auto kResortEvery = std::chrono::seconds(5);

  inline std::string memoryText(std::uint64_t bytes) {
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    return mb >= 1024.0 ? std::format("{:.1f} GB", mb / 1024.0) : std::format("{:.0f} MB", mb);
  }

  inline std::string cpuText(double percent) { return std::format("{:.1f} %", percent); }

  inline std::string rateText(double bytesPerSec) {
    if (bytesPerSec >= 1024.0 * 1024.0) {
      return std::format("{:.1f} MB/s", bytesPerSec / (1024.0 * 1024.0));
    }
    return std::format("{:.0f} KB/s", bytesPerSec / 1024.0);
  }

  // h:mm:ss of CPU time.
  inline std::string clockText(std::uint64_t usec) {
    const std::uint64_t s = usec / 1'000'000;
    return std::format("{}:{:02}:{:02}", s / 3600, (s % 3600) / 60, s % 60);
  }

  inline std::string uptimeText(std::uint64_t startedUsec) {
    if (startedUsec == 0) {
      return "–";
    }
    const auto now =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    const auto seconds =
        static_cast<std::uint64_t>(std::max<std::int64_t>(0, now - static_cast<std::int64_t>(startedUsec))) / 1'000'000;
    if (seconds >= 3600) {
      return std::format("{} h {} min", seconds / 3600, (seconds % 3600) / 60);
    }
    return std::format("{} min", std::max<std::uint64_t>(1, seconds / 60));
  }

  inline std::string energyText(int bars) {
    return i18n::tr(bars >= 3 ? "activity.energy-high" : bars == 2 ? "activity.energy-medium" : "activity.energy-low");
  }

  // Values scaled to [0, 1] by their own maximum, never below `floor` (Graph wants normalized series).
  template <typename Range> std::vector<float> normalized(const Range& values, float floor = 1.0F) {
    float top = floor;
    for (const float v : values) {
      top = std::max(top, v);
    }
    std::vector<float> out;
    out.reserve(values.size());
    for (const float v : values) {
      out.push_back(std::clamp(v / top, 0.0F, 1.0F));
    }
    return out;
  }

  inline std::unique_ptr<Graph> makeGraph(std::vector<float> values, float width, float height, ColorRole role) {
    auto graph = std::make_unique<Graph>();
    graph->setFillOpacity(0.3F);
    graph->setColor(colorSpecFromRole(role));
    graph->setValues(std::move(values));
    graph->setSize(width, height);
    return graph;
  }

  // Grouped card of the inspector (parts .scard): solid_thin 50 % on glass, the scheme card colour otherwise.
  inline std::unique_ptr<Flex> card(bool glass, float scale) {
    return ui::column({
        .align = FlexAlign::Stretch,
        .paddingV = Style::spaceXs * scale,
        .paddingH = Style::spaceLg * scale,
        .fill = glass ? colorSpecFromRole(ColorRole::SolidThin, 0.5F)
                      : fixedColorSpec(rgbaHex(isResolvedLightTheme() ? Tokens::schemeLight.card : Tokens::schemeDark.card)),
        .radius = Tokens::radiusR8 * scale,
        .border = colorSpecFromRole(glass ? ColorRole::LensEdge : ColorRole::Stroke),
        .borderWidth = 0.5F * scale,
    });
  }

  // A 40 row of a card: key left, value right (parts .srow).
  inline std::unique_ptr<Flex> cardRow(std::string key, std::string value, float scale, Label** valueOut = nullptr) {
    return ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .minHeight = Tokens::sizeRow * scale},
        ui::label({
            .text = std::move(key),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::Label1),
            .maxLines = 1,
            .ellipsize = TextEllipsize::End,
            .flexGrow = 1.0F,
        }),
        ui::label({
            .out = valueOut,
            .text = std::move(value),
            .fontSize = Style::fontSizeBody * scale,
            .fontFeatures = std::string("tnum"),
            .color = colorSpecFromRole(ColorRole::Label2),
            .textAlign = TextAlign::End,
        })
    );
  }

  inline std::unique_ptr<Label>
  text(std::string value, float size, ColorRole role, FontWeight weight = FontWeight::Normal, Label** out = nullptr) {
    return ui::label({
        .out = out,
        .text = std::move(value),
        .fontSize = size,
        .fontWeight = weight,
        .color = colorSpecFromRole(role),
        .maxLines = 1,
        .ellipsize = TextEllipsize::End,
    });
  }

} // namespace activity_ui
