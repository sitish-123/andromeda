#include "shell/settings/settings_content_displays.h"

#include "i18n/i18n.h"
#include "shell/settings/settings_content.h"
#include "shell/settings/settings_content_common.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace settings {

  namespace {

    constexpr std::array<double, 7> kScalePresets = {1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0};
    constexpr float kSelectWidth = 190.0F;

    const OutputMode* findMode(const OutputHead& head, const zwlr_output_mode_v1* handle) {
      const auto it = std::ranges::find(head.modes, handle, &OutputMode::handle);
      return it != head.modes.end() ? &*it : nullptr;
    }

    std::string displayTitle(const OutputHead& head) {
      std::string product = head.make.empty() ? head.model : head.make + " " + head.model;
      return product.empty() ? head.name : std::format("{} ({})", product, head.name);
    }

    // Virtual outputs report 0 mHz.
    std::string formatRefresh(std::int32_t refreshMhz) {
      if (refreshMhz <= 0) {
        return i18n::tr("settings.displays.refresh-default");
      }
      return std::format("{:.2f} Hz", static_cast<double>(refreshMhz) / 1000.0);
    }

    std::string formatScale(double scale) { return std::format("{:g}×", scale); }

    // Distinct sizes, largest first.
    std::vector<std::pair<int, int>> modeSizes(const OutputHead& head) {
      std::vector<std::pair<int, int>> sizes;
      for (const OutputMode& mode : head.modes) {
        if (!std::ranges::contains(sizes, std::pair{mode.width, mode.height})) {
          sizes.emplace_back(mode.width, mode.height);
        }
      }
      std::ranges::sort(sizes, [](const auto& a, const auto& b) { return a.first * a.second > b.first * b.second; });
      return sizes;
    }

    // Modes of one size, highest refresh first.
    std::vector<const OutputMode*> modesOfSize(const OutputHead& head, std::pair<int, int> size) {
      std::vector<const OutputMode*> modes;
      for (const OutputMode& mode : head.modes) {
        if (mode.width == size.first && mode.height == size.second) {
          modes.push_back(&mode);
        }
      }
      std::ranges::sort(modes, [](const OutputMode* a, const OutputMode* b) { return a->refreshMhz > b->refreshMhz; });
      return modes;
    }

    std::unique_ptr<Flex> makeDisplayRow(std::string_view title, std::unique_ptr<Node> control, float scale) {
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      row->addChild(
          makeLabel(title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold)
      );
      row->addChild(ui::spacer());
      row->addChild(std::move(control));
      return row;
    }

    std::unique_ptr<Node> makeDisplaySelect(
        std::vector<std::string> options, std::optional<std::size_t> selected, float scale,
        std::function<void(std::size_t)> onSelect
    ) {
      return ui::select({
          .options = std::move(options),
          .selectedIndex = selected,
          .clearSelection = !selected.has_value(),
          .placeholder = i18n::tr("settings.displays.custom"),
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .glyphSize = Style::fontSizeBody * scale,
          .width = kSelectWidth * scale,
          .height = Style::controlHeight * scale,
          .onSelectionChanged = [onSelect = std::move(onSelect)](std::size_t index, std::string_view) {
            onSelect(index);
          },
      });
    }

    void addModeRows(
        Flex& body, const OutputHead& head, const OutputHeadConfig& config, const SettingsDisplaysContext& ctx
    ) {
      const OutputMode* current = findMode(head, config.mode);
      const auto sizes = modeSizes(head);
      if (sizes.empty()) {
        return;
      }
      const std::pair<int, int> currentSize =
          current != nullptr ? std::pair{current->width, current->height} : sizes.front();

      std::vector<std::string> sizeLabels;
      std::vector<zwlr_output_mode_v1*> bestModes;
      for (const auto& size : sizes) {
        sizeLabels.push_back(std::format("{} × {}", size.first, size.second));
        bestModes.push_back(modesOfSize(head, size).front()->handle);
      }
      const auto sizeIt = std::ranges::find(sizes, currentSize);
      body.addChild(makeDisplayRow(
          i18n::tr("settings.displays.resolution"),
          makeDisplaySelect(
              std::move(sizeLabels), static_cast<std::size_t>(sizeIt - sizes.begin()), ctx.scale,
              [edit = ctx.edit, config, bestModes = std::move(bestModes)](std::size_t index) {
                OutputHeadConfig next = config;
                next.mode = bestModes[index];
                edit(std::move(next));
              }
          ),
          ctx.scale
      ));

      std::vector<zwlr_output_mode_v1*> refreshModes;
      std::vector<std::string> refreshLabels;
      std::optional<std::size_t> refreshIndex;
      for (const OutputMode* mode : modesOfSize(head, currentSize)) {
        if (mode == current) {
          refreshIndex = refreshModes.size();
        }
        refreshModes.push_back(mode->handle);
        refreshLabels.push_back(formatRefresh(mode->refreshMhz));
      }
      body.addChild(makeDisplayRow(
          i18n::tr("settings.displays.refresh-rate"),
          makeDisplaySelect(
              std::move(refreshLabels), refreshIndex, ctx.scale,
              [edit = ctx.edit, config, refreshModes = std::move(refreshModes)](std::size_t index) {
                OutputHeadConfig next = config;
                next.mode = refreshModes[index];
                edit(std::move(next));
              }
          ),
          ctx.scale
      ));
    }

    void addScaleRow(Flex& body, const OutputHeadConfig& config, const SettingsDisplaysContext& ctx) {
      std::vector<double> scales(kScalePresets.begin(), kScalePresets.end());
      if (!std::ranges::any_of(scales, [&](double s) { return std::abs(s - config.scale) < 0.001; })) {
        scales.push_back(config.scale);
        std::ranges::sort(scales);
      }
      std::vector<std::string> labels;
      std::optional<std::size_t> selected;
      for (std::size_t i = 0; i < scales.size(); ++i) {
        labels.push_back(formatScale(scales[i]));
        if (std::abs(scales[i] - config.scale) < 0.001) {
          selected = i;
        }
      }
      body.addChild(makeDisplayRow(
          i18n::tr("settings.displays.scale"),
          makeDisplaySelect(
              std::move(labels), selected, ctx.scale,
              [edit = ctx.edit, config, scales](std::size_t index) {
                OutputHeadConfig next = config;
                next.scale = scales[index];
                edit(std::move(next));
              }
          ),
          ctx.scale
      ));
    }

    void addRotationRow(Flex& body, const OutputHeadConfig& config, const SettingsDisplaysContext& ctx) {
      std::vector<std::string> labels = {i18n::tr("settings.displays.rotation-normal"), "90°", "180°", "270°"};
      const std::optional<std::size_t> selected =
          config.transform <= 3 ? std::optional<std::size_t>{static_cast<std::size_t>(config.transform)} : std::nullopt;
      body.addChild(makeDisplayRow(
          i18n::tr("settings.displays.rotation"),
          makeDisplaySelect(
              std::move(labels), selected, ctx.scale,
              [edit = ctx.edit, config](std::size_t index) {
                OutputHeadConfig next = config;
                next.transform = static_cast<std::int32_t>(index);
                edit(std::move(next));
              }
          ),
          ctx.scale
      ));
    }

    void addPlacementRow(
        Flex& body, const OutputHead& head, const OutputHeadConfig& config, const SettingsDisplaysContext& ctx
    ) {
      const auto& heads = ctx.outputs->heads();
      std::vector<std::pair<int, int>> positions;
      std::vector<std::string> labels;
      std::optional<std::size_t> selected;
      const auto size = displayLogicalSize(head, config);
      for (std::size_t i = 0; i < heads.size(); ++i) {
        const OutputHeadConfig& other = ctx.edits[i];
        if (heads[i].name == head.name || !other.enabled) {
          continue;
        }
        const auto otherSize = displayLogicalSize(heads[i], other);
        for (const auto [placement, key] : std::array{
                 std::pair{DisplayPlacement::RightOf, "settings.displays.right-of"},
                 std::pair{DisplayPlacement::LeftOf, "settings.displays.left-of"},
                 std::pair{DisplayPlacement::Above, "settings.displays.above"},
                 std::pair{DisplayPlacement::Below, "settings.displays.below"},
             }) {
          const auto position = displayPlacementPosition(other, otherSize, size, placement);
          if (position == std::pair{config.x, config.y}) {
            selected = positions.size();
          }
          positions.push_back(position);
          labels.push_back(i18n::tr(key, "name", heads[i].name));
        }
      }
      if (positions.empty()) {
        return;
      }
      body.addChild(makeDisplayRow(
          i18n::tr("settings.displays.position"),
          makeDisplaySelect(
              std::move(labels), selected, ctx.scale,
              [edit = ctx.edit, config, positions = std::move(positions)](std::size_t index) {
                OutputHeadConfig next = config;
                std::tie(next.x, next.y) = positions[index];
                edit(std::move(next));
              }
          ),
          ctx.scale
      ));
    }

    void addDisplayCard(
        Flex& content, const OutputHead& head, const OutputHeadConfig& config, const SettingsDisplaysContext& ctx
    ) {
      Flex* body = addSettingsCard(content, displayTitle(head), ctx.scale);
      const bool onlyEnabled = config.enabled && std::ranges::count_if(ctx.edits, &OutputHeadConfig::enabled) == 1;
      body->addChild(makeDisplayRow(
          i18n::tr("settings.displays.enabled"),
          ui::toggle({
              .checked = config.enabled,
              .enabled = !onlyEnabled,
              .scale = ctx.scale,
              .onChange =
                  [edit = ctx.edit, config](bool enabled) {
                    OutputHeadConfig next = config;
                    next.enabled = enabled;
                    edit(std::move(next));
                  },
          }),
          ctx.scale
      ));
      if (!config.enabled) {
        return;
      }
      addModeRows(*body, head, config, ctx);
      addScaleRow(*body, config, ctx);
      addRotationRow(*body, config, ctx);
      addPlacementRow(*body, head, config, ctx);
      if (head.adaptiveSyncReported) {
        body->addChild(makeDisplayRow(
            i18n::tr("settings.displays.adaptive-sync"),
            ui::toggle({
                .checked = config.adaptiveSync,
                .scale = ctx.scale,
                .onChange =
                    [edit = ctx.edit, config](bool enabled) {
                      OutputHeadConfig next = config;
                      next.adaptiveSync = enabled;
                      edit(std::move(next));
                    },
            }),
            ctx.scale
        ));
      }
    }

    std::unique_ptr<Node> makeActionButton(
        std::string text, ButtonVariant variant, bool enabled, float scale, std::function<void()> onClick
    ) {
      return ui::button({
          .text = std::move(text),
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .enabled = enabled,
          .variant = variant,
          .onClick = std::move(onClick),
      });
    }

    void addActions(Flex& content, const SettingsDisplaysContext& ctx) {
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * ctx.scale, .fillWidth = true});
      if (ctx.confirmSecondsLeft > 0) {
        row->addChild(makeLabel(
            i18n::tr("settings.displays.confirm", "seconds", std::to_string(ctx.confirmSecondsLeft)),
            Style::fontSizeBody * ctx.scale, colorSpecFromRole(ColorRole::OnSurface)
        ));
        row->addChild(ui::spacer());
        row->addChild(
            makeActionButton(i18n::tr("settings.displays.revert"), ButtonVariant::Default, true, ctx.scale, ctx.revert)
        );
        row->addChild(
            makeActionButton(i18n::tr("settings.displays.keep"), ButtonVariant::Primary, true, ctx.scale, ctx.keep)
        );
      } else {
        if (!ctx.error.empty()) {
          row->addChild(makeLabel(ctx.error, Style::fontSizeBody * ctx.scale, colorSpecFromRole(ColorRole::Error)));
        }
        row->addChild(ui::spacer());
        const bool idle = !ctx.outputs->busy();
        row->addChild(makeActionButton(
            i18n::tr("settings.displays.discard"), ButtonVariant::Default, ctx.dirty && idle, ctx.scale, ctx.discard
        ));
        row->addChild(makeActionButton(
            i18n::tr("settings.displays.apply"), ButtonVariant::Primary, ctx.dirty && idle, ctx.scale, ctx.apply
        ));
      }
      content.addChild(std::move(row));
    }

  } // namespace

  std::pair<int, int> displayLogicalSize(const OutputHead& head, const OutputHeadConfig& config) {
    const OutputMode* mode = findMode(head, config.mode);
    if (mode == nullptr || config.scale <= 0.0) {
      return {0, 0};
    }
    const int w = static_cast<int>(std::lround(mode->width / config.scale));
    const int h = static_cast<int>(std::lround(mode->height / config.scale));
    // Odd wl_output_transform values rotate by 90 or 270 degrees.
    return config.transform % 2 == 1 ? std::pair{h, w} : std::pair{w, h};
  }

  std::pair<int, int> displayPlacementPosition(
      const OutputHeadConfig& anchor, std::pair<int, int> anchorSize, std::pair<int, int> size,
      DisplayPlacement placement
  ) {
    switch (placement) {
    case DisplayPlacement::RightOf:
      return {anchor.x + anchorSize.first, anchor.y};
    case DisplayPlacement::LeftOf:
      return {anchor.x - size.first, anchor.y};
    case DisplayPlacement::Above:
      return {anchor.x, anchor.y - size.second};
    case DisplayPlacement::Below:
      return {anchor.x, anchor.y + anchorSize.second};
    }
    return {anchor.x, anchor.y};
  }

  void addSettingsDisplays(Flex& content, const SettingsDisplaysContext& ctx) {
    if (ctx.outputs == nullptr || !ctx.outputs->ready()) {
      content.addChild(makeSettingSubtitleLabel(i18n::tr("settings.displays.unavailable"), ctx.scale));
      return;
    }
    const auto& heads = ctx.outputs->heads();
    for (std::size_t i = 0; i < heads.size() && i < ctx.edits.size(); ++i) {
      addDisplayCard(content, heads[i], ctx.edits[i], ctx);
    }
    addActions(content, ctx);
  }

} // namespace settings
