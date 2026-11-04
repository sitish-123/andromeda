#include "shell/settings/settings_content_displays.h"

#include "i18n/i18n.h"
#include "shell/settings/settings_content.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_control_factory.h"
#include "shell/settings/settings_selection_style.h"
#include "ui/controls/button.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

  // The arrangement canvas places each output by hand (participatesInLayout false). Flex skips such children, so their
  // own content (the wallpaper and the name) would never lay out; lay them out at the size they were given.
  // A parent Flex reaches this node through arrange(), which skips doLayout, so both entry points do it.
  class PositionedLayout final : public Flex {
  protected:
    void doLayout(Renderer& renderer) override {
      Flex::doLayout(renderer);
      layoutPositioned(renderer);
    }
    void doArrange(Renderer& renderer, const LayoutRect& rect) override {
      Flex::doArrange(renderer, rect);
      layoutPositioned(renderer);
    }

  private:
    void layoutPositioned(Renderer& renderer) {
      for (const auto& child : children()) {
        if (child->visible() && !child->participatesInLayout()) {
          child->layout(renderer);
        }
      }
    }
  };

} // namespace

namespace settings {

  namespace {

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

    const std::string* mirrorSourceOf(const SettingsDisplaysContext& ctx, const std::string& name) {
      if (ctx.mirrors == nullptr) {
        return nullptr;
      }
      const auto it = ctx.mirrors->mirrors().find(name);
      return it != ctx.mirrors->mirrors().end() ? &it->second : nullptr;
    }

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
      body.addChild(SettingsControlFactory::makeRow(
          i18n::tr("settings.displays.resolution"),
          sizes.size() == 1
              ? makeLabel(sizeLabels.front(), Style::fontSizeBody * ctx.scale, colorSpecFromRole(ColorRole::Label2))
              : makeDisplaySelect(
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
      body.addChild(SettingsControlFactory::makeRow(
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

    // Scale as five thumbnails from larger text (high scale) to more space: more text lines per thumbnail.
    void addScaleRow(Flex& body, const OutputHeadConfig& config, const SettingsDisplaysContext& ctx) {
      std::vector<double> thumbScales = {2.0, 1.75, 1.5, 1.25, 1.0};
      if (!std::ranges::any_of(thumbScales, [&](double v) { return std::abs(v - config.scale) < 0.001; })) {
        thumbScales.push_back(config.scale);
        std::ranges::sort(thumbScales, std::greater<>{});
      }
      const float scale = ctx.scale;
      auto strip = ui::row({.align = FlexAlign::Start, .gap = Style::spaceSm * scale, .fillWidth = true});
      for (std::size_t i = 0; i < thumbScales.size(); ++i) {
        const double value = thumbScales[i];
        const bool selected = std::abs(value - config.scale) < 0.001;
        const ColorSpec none = clearColorSpec();
        const Button::ButtonStateColors state{
            .bg = none, .border = selected ? colorSpecFromRole(ColorRole::Label1) : none, .label = none
        };
        auto preview = ui::column({
            .align = FlexAlign::Stretch,
            .gap = Style::spaceXs * scale,
            .fill = colorSpecFromRole(ColorRole::Fill1),
            .radius = (metrics::kPickerRadius - (Style::focusRingWidth + Style::focusRingGap)) * scale,
            .height = (metrics::kThumbHeight - 2.0F * (Style::focusRingWidth + Style::focusRingGap)) * scale,
            .flexGrow = 1.0F,
            // The end inset keeps the text lines clear of the selection badge (20 at inset 8).
            .configure = [scale](Flex& flex) {
              flex.setPadding(Style::spaceSm * scale, (Style::spaceSm + 20.0F + Style::spaceXs) * scale,
                              Style::spaceSm * scale, Style::spaceSm * scale);
            },
        });
        for (std::size_t line = 0; line < 3 + i; ++line) {
          preview->addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Label3), .radius = Style::spaceXs * scale,
                                     .height = 2.0F * scale}));
        }
        auto tile = ui::button({
            .variant = ButtonVariant::Ghost,
            .customPalette = Button::ButtonPalette{
                .borderWidth = Style::focusRingWidth * scale, .normal = state, .hover = state, .pressed = state,
                .disabled = state
            },
            .minHeight = metrics::kThumbHeight * scale,
            .padding = (Style::focusRingWidth + Style::focusRingGap) * scale,
            .radius = metrics::kPickerRadius * scale,
            .onClick = [edit = ctx.edit, config, value] {
              OutputHeadConfig next = config;
              next.scale = value;
              edit(std::move(next));
            },
            .configure = [](Button& button) {
              button.setTabStop(false);
              button.setFlexGrow(1.0F);
            },
        });
        tile->addChild(std::move(preview));
        tile->setCheckBadge(selected, scale, metrics::kPickerRadius * scale);
        const char* captionKey = value == 2.0 ? "settings.displays.scale-larger" : value == 1.5
            ? "settings.displays.scale-default" : value == 1.0 ? "settings.displays.scale-space" : nullptr;
        const std::string caption =
            captionKey != nullptr ? i18n::tr(captionKey) : std::format("{} %", static_cast<int>(std::lround(value * 100.0)));
        strip->addChild(ui::column(
            {.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale, .minWidth = metrics::kPickerTileWidth * scale,
             .flexGrow = 1.0F},
            std::move(tile),
            ui::row(
                {.justify = FlexJustify::Center},
                makeLabel(
                    caption, Style::fontSizeCaption * scale,
                    colorSpecFromRole(selected ? ColorRole::Label1 : ColorRole::Label2),
                    selected ? FontWeight::Bold : FontWeight::Medium
                )
            )
        ));
      }
      body.addChild(ui::column(
          {.align = FlexAlign::Stretch, .paddingV = Style::spaceSm * scale, .paddingH = Style::spaceSm * scale},
          std::move(strip)
      ));
    }

    void addRotationRow(Flex& body, const OutputHeadConfig& config, const SettingsDisplaysContext& ctx) {
      std::vector<std::string> labels = {i18n::tr("settings.displays.rotation-normal"), "90°", "180°", "270°"};
      const std::optional<std::size_t> selected =
          config.transform <= 3 ? std::optional<std::size_t>{static_cast<std::size_t>(config.transform)} : std::nullopt;
      body.addChild(SettingsControlFactory::makeRow(
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
        if (heads[i].name == head.name || !other.enabled || mirrorSourceOf(ctx, heads[i].name) != nullptr) {
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
      body.addChild(SettingsControlFactory::makeRow(
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

    void addChoiceProperty(
        Flex& body, const OutputHead& head, const SettingsDisplaysContext& ctx, std::string_view key,
        std::string_view labelKey, std::initializer_list<std::string_view> values
    ) {
      const std::string current = ctx.mirrors->property(head.name, key);
      std::vector<std::string> labels;
      std::vector<std::string> choices;
      std::optional<std::size_t> selected;
      for (const std::string_view value : values) {
        if (value == current) {
          selected = choices.size();
        }
        choices.emplace_back(value);
        labels.push_back(i18n::tr(std::string(labelKey) + "-options." + std::string(value)));
      }
      body.addChild(SettingsControlFactory::makeRow(
          i18n::tr(labelKey),
          makeDisplaySelect(
              std::move(labels), selected, ctx.scale,
              [setProperty = ctx.setProperty, target = head.name, key = std::string(key),
               choices = std::move(choices)](std::size_t index) { setProperty(target, key, choices[index]); }
          ),
          ctx.scale
      ));
    }

    void addToggleProperty(
        Flex& body, const OutputHead& head, const SettingsDisplaysContext& ctx, std::string_view key,
        std::string_view labelKey
    ) {
      body.addChild(SettingsControlFactory::makeRow(
          i18n::tr(labelKey),
          ui::toggle({
              .checked = ctx.mirrors->property(head.name, key) == "true",
              .scale = ctx.scale,
              .onChange = [setProperty = ctx.setProperty, target = head.name, key = std::string(key)](
                              bool enabled
                          ) { setProperty(target, key, enabled ? "true" : "false"); },
          }),
          ctx.scale
      ));
    }

    void addStepperProperty(
        Flex& body, const OutputHead& head, const SettingsDisplaysContext& ctx, std::string_view key,
        std::string_view labelKey, int min, int max, int step, int fallback
    ) {
      const std::string current = ctx.mirrors->property(head.name, key);
      int value = fallback;
      std::from_chars(current.data(), current.data() + current.size(), value);
      body.addChild(SettingsControlFactory::makeRow(
          i18n::tr(labelKey),
          ui::stepper({
              .minValue = min,
              .maxValue = max,
              .step = step,
              .value = value,
              .scale = ctx.scale,
              .onValueCommitted = [setProperty = ctx.setProperty, target = head.name, key = std::string(key)](
                                      int committed
                                  ) { setProperty(target, key, std::to_string(committed)); },
          }),
          ctx.scale
      ));
    }

    // What zwlr_output_manager_v1 cannot express: VRR only in fullscreen, HDR, tearing, and the display's workspaces.
    void addPropertyRows(Flex& body, const OutputHead& head, const SettingsDisplaysContext& ctx) {
      if (head.adaptiveSyncReported) {
        addChoiceProperty(body, head, ctx, "vrr", "settings.displays.vrr", {"disabled", "always", "fullscreen"});
      }
      addChoiceProperty(body, head, ctx, "hdr", "settings.displays.hdr", {"off", "on", "auto", "fullscreen"});
      if (ctx.mirrors->property(head.name, "hdr") != "off") {
        addStepperProperty(body, head, ctx, "sdr_white", "settings.displays.sdr-white", 80, 1000, 10, 203);
      }
      addToggleProperty(body, head, ctx, "tearing", "settings.displays.tearing");

      const std::string workspaces = ctx.mirrors->property(head.name, "workspaces");
      if (workspaces == "named") {
        body.addChild(makeSettingSubtitleLabel(i18n::tr("settings.displays.workspaces-named"), ctx.scale));
      } else {
        const bool dynamic = workspaces == "dynamic" || workspaces.empty();
        body.addChild(SettingsControlFactory::makeRow(
            i18n::tr("settings.displays.workspaces"),
            makeDisplaySelect(
                {i18n::tr("settings.displays.workspaces-options.dynamic"),
                 i18n::tr("settings.displays.workspaces-options.fixed")},
                dynamic ? 0U : 1U, ctx.scale,
                [setProperty = ctx.setProperty, target = head.name, dynamic](std::size_t index) {
                  if ((index == 0) != dynamic) {
                    setProperty(target, "workspaces", index == 0 ? "dynamic" : "4");
                  }
                }
            ),
            ctx.scale
        ));
        if (dynamic) {
          addStepperProperty(body, head, ctx, "min_workspaces", "settings.displays.min-workspaces", 1, 64, 1, 1);
        } else {
          addStepperProperty(body, head, ctx, "workspaces", "settings.displays.workspace-count", 1, 64, 1, 4);
        }
      }
      addChoiceProperty(
          body, head, ctx, "workspace_axis", "settings.displays.workspace-axis", {"vertical", "horizontal"}
      );
      addToggleProperty(body, head, ctx, "cyclic_workspaces", "settings.displays.cyclic-workspaces");
    }

    bool isBuiltIn(const OutputHead& head) {
      return head.name.starts_with("eDP") || head.name.starts_with("LVDS") || head.name.starts_with("DSI");
    }

    void addNightLightRow(Flex& body, const SettingsDisplaysContext& ctx) {
      if (!ctx.setNightLight) {
        return;
      }
      auto control = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * ctx.scale});
      control->addChild(makeLabel(
          i18n::tr(ctx.nightLight ? "settings.displays.night-light-on" : "settings.displays.night-light-off"),
          Style::fontSizeBody * ctx.scale, colorSpecFromRole(ColorRole::Label2)
      ));
      control->addChild(ui::toggle({
          .checked = ctx.nightLight,
          .scale = ctx.scale,
          .onChange = [set = ctx.setNightLight](bool on) { set(on); },
      }));
      body.addChild(SettingsControlFactory::makeRow(i18n::tr("settings.displays.night-light"), std::move(control), ctx.scale));
    }

    // Extend or mirror, for exactly two enabled displays: the second mirrors the first.
    void addMultipleDisplaysRow(Flex& body, const SettingsDisplaysContext& ctx) {
      const auto& heads = ctx.outputs->heads();
      std::vector<std::string> enabled;
      for (std::size_t i = 0; i < heads.size() && i < ctx.edits.size(); ++i) {
        if (ctx.edits[i].enabled) {
          enabled.push_back(heads[i].name);
        }
      }
      if (enabled.size() != 2 || ctx.mirrors == nullptr || !ctx.mirrors->ready() || !ctx.setMirror) {
        return;
      }
      const std::string source = enabled[0];
      const std::string target = enabled[1];
      const bool mirrored = mirrorSourceOf(ctx, target) != nullptr;
      body.addChild(SettingsControlFactory::makeRow(
          i18n::tr("settings.displays.multiple"),
          ui::segmented({
              .options = std::vector<ui::SegmentedOption>{
                  {.label = i18n::tr("settings.displays.extend")}, {.label = i18n::tr("settings.displays.mirror")}
              },
              .selectedIndex = mirrored ? 1U : 0U,
              .scale = ctx.scale,
              .onChange = [set = ctx.setMirror, target, source](std::size_t index) {
                set(target, index == 1 ? source : std::string{});
              },
          }),
          ctx.scale
      ));
    }

    void addDisplayCard(
        Flex& content, const OutputHead& head, const OutputHeadConfig& config, const SettingsDisplaysContext& ctx
    ) {
      Flex* body = addSettingsCard(
          content, isBuiltIn(head) ? i18n::tr("settings.displays.built-in") : displayTitle(head), ctx.scale
      );
      const bool onlyEnabled = config.enabled && std::ranges::count_if(ctx.edits, &OutputHeadConfig::enabled) == 1;
      const auto addEnabledRow = [&] {
        body->addChild(SettingsControlFactory::makeRow(
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
      };
      if (!config.enabled) {
        addEnabledRow();
        return;
      }
      // A mirror is scaled to fit and has no place on the desktop, so only its mode and rotation matter.
      const bool mirror = mirrorSourceOf(ctx, head.name) != nullptr;
      if (!mirror) {
        addScaleRow(*body, config, ctx);
      }
      addModeRows(*body, head, config, ctx);
      if (!mirror && ctx.mirrors != nullptr && ctx.mirrors->ready()) {
        addPropertyRows(*body, head, ctx);
      } else if (!mirror && head.adaptiveSyncReported) {
        body->addChild(SettingsControlFactory::makeRow(
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
      addNightLightRow(*body, ctx);
      addMultipleDisplaysRow(*body, ctx);
      addRotationRow(*body, config, ctx);
      if (!mirror) {
        addPlacementRow(*body, head, config, ctx);
      }
      addEnabledRow();
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
            Style::fontSizeBody * ctx.scale, colorSpecFromRole(ColorRole::Label1)
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
          row->addChild(makeLabel(ctx.error, Style::fontSizeBody * ctx.scale, colorSpecFromRole(ColorRole::DangerText)));
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

  namespace {

    std::string selectedHeadName(const SettingsDisplaysContext& ctx) {
      const auto& heads = ctx.outputs->heads();
      for (const OutputHead& head : heads) {
        if (head.name == ctx.selected) {
          return head.name;
        }
      }
      return heads.empty() ? std::string{} : heads.front().name;
    }

    struct LayoutRect {
      std::size_t index;
      int x, y, w, h;
      bool active; // enabled and draggable; a disabled head is a dimmed tile placed beside the layout
    };

    bool overlapsAny(const std::vector<LayoutRect>& rects, const LayoutRect& moved, int x, int y) {
      return std::ranges::any_of(rects, [&](const LayoutRect& o) {
        return o.active && o.index != moved.index && x < o.x + o.w && o.x < x + moved.w && y < o.y + o.h
            && o.y < y + moved.h;
      });
    }

    // Moves `moved` by (dx, dy), then pulls it against the edges and tops/lefts of the other displays within kSnap.
    std::pair<int, int> snappedPosition(const std::vector<LayoutRect>& rects, const LayoutRect& moved, int dx, int dy) {
      constexpr int kSnap = 48; // layout pixels
      int x = moved.x + dx;
      int y = moved.y + dy;
      int bestX = kSnap + 1;
      int bestY = kSnap + 1;
      int snapX = x;
      int snapY = y;
      const auto consider = [](int value, int target, int& best, int& out) {
        if (std::abs(value - target) < best) {
          best = std::abs(value - target);
          out = target;
        }
      };
      for (const LayoutRect& o : rects) {
        if (o.index == moved.index || !o.active) {
          continue;
        }
        consider(x, o.x + o.w, bestX, snapX);
        consider(x + moved.w, o.x, bestX, snapX);
        consider(x, o.x, bestX, snapX);
        consider(y, o.y + o.h, bestY, snapY);
        consider(y + moved.h, o.y, bestY, snapY);
        consider(y, o.y, bestY, snapY);
      }
      return {bestX <= kSnap ? snapX : x, bestY <= kSnap ? snapY : y};
    }

    // Layout rectangles scaled into a canvas; a drag snaps the display against the edges of its neighbours.
    void addArrangementCanvas(Flex& content, const SettingsDisplaysContext& ctx) {
      constexpr float kCanvasHeight = 148.0F;
      constexpr float kCanvasInset = 16.0F;
      constexpr float kCanvasWidth = 560.0F;
      const float scale = ctx.scale;
      const auto& heads = ctx.outputs->heads();
      std::vector<LayoutRect> rects;
      for (std::size_t i = 0; i < heads.size() && i < ctx.edits.size(); ++i) {
        if (!ctx.edits[i].enabled || mirrorSourceOf(ctx, heads[i].name) != nullptr) {
          continue;
        }
        const auto [w, h] = displayLogicalSize(heads[i], ctx.edits[i]);
        if (w > 0 && h > 0) {
          rects.push_back({i, ctx.edits[i].x, ctx.edits[i].y, w, h, true});
        }
      }
      if (rects.empty()) {
        return;
      }
      int minX = rects[0].x;
      int minY = rects[0].y;
      int maxX = rects[0].x + rects[0].w;
      int maxY = rects[0].y + rects[0].h;
      for (const LayoutRect& r : rects) {
        minX = std::min(minX, r.x);
        minY = std::min(minY, r.y);
        maxX = std::max(maxX, r.x + r.w);
        maxY = std::max(maxY, r.y + r.h);
      }
      // Disabled heads sit in a row to the right of the layout so they stay selectable.
      const int gap = (maxX - minX) / 16 + 1;
      int nextX = maxX + gap;
      for (std::size_t i = 0; i < heads.size() && i < ctx.edits.size(); ++i) {
        if (ctx.edits[i].enabled) {
          continue;
        }
        const auto mode = std::ranges::find_if(heads[i].modes, &OutputMode::preferred);
        const OutputMode* shown = ctx.edits[i].mode != nullptr ? findMode(heads[i], ctx.edits[i].mode)
            : mode != heads[i].modes.end()                    ? &*mode
            : heads[i].modes.empty()                          ? nullptr
                                                              : &heads[i].modes.front();
        if (shown == nullptr) {
          continue;
        }
        OutputHeadConfig sized = ctx.edits[i];
        sized.mode = shown->handle;
        const auto [w, h] = displayLogicalSize(heads[i], sized);
        if (w <= 0 || h <= 0) {
          continue;
        }
        rects.push_back({i, nextX, minY, w, h, false});
        nextX += w + gap;
        maxX = std::max(maxX, nextX - gap);
        maxY = std::max(maxY, minY + h);
      }
      const float fit = std::min(
          (kCanvasWidth - 2.0F * kCanvasInset) / static_cast<float>(std::max(1, maxX - minX)),
          (kCanvasHeight - 2.0F * kCanvasInset) / static_cast<float>(std::max(1, maxY - minY))
      );
      const std::string selectedName = selectedHeadName(ctx);
      const float layoutW = static_cast<float>(maxX - minX) * fit;
      const float layoutH = static_cast<float>(maxY - minY) * fit;

      auto layout = std::make_unique<PositionedLayout>();
      layout->setDirection(FlexDirection::Vertical);
      layout->setSize(layoutW * scale, layoutH * scale);
      layout->setMinWidth(layoutW * scale);
      layout->setMinHeight(layoutH * scale);
      for (const LayoutRect& r : rects) {
        const bool selected = heads[r.index].name == selectedName;
        const ColorSpec none = clearColorSpec();
        const Button::ButtonStateColors state{
            .bg = none,
            .border = selected ? colorSpecFromRole(ColorRole::Accent) : none,
            .label = r.active ? fixedColorSpec(rgbHex(0xFFFFFF)) : colorSpecFromRole(ColorRole::Label3)
        };
        struct Press {
          bool active = false;
          float x = 0.0F;
          float y = 0.0F;
        };
        auto pressAt = std::make_shared<Press>();
        layout->addChild(ui::button({
            .variant = ButtonVariant::Ghost,
            .customPalette = Button::ButtonPalette{
                .borderWidth = Style::focusRingWidth * scale, .normal = state, .hover = state, .pressed = state,
                .disabled = state
            },
            .minWidth = static_cast<float>(r.w) * fit * scale,
            .minHeight = static_cast<float>(r.h) * fit * scale,
            .padding = (Style::focusRingWidth + Style::focusRingGap) * scale,
            .radius = Style::spaceSm * scale,
            .width = static_cast<float>(r.w) * fit * scale,
            .height = static_cast<float>(r.h) * fit * scale,
            .onPress = [pressAt, rects, r, fit, scale, edit = ctx.edit, select = ctx.select, name = heads[r.index].name,
                        config = ctx.edits[r.index]](
                           float x, float y, bool pressed
                       ) {
              if (pressed) {
                *pressAt = {true, x, y};
                return;
              }
              if (!std::exchange(pressAt->active, false)) {
                return;
              }
              const float unit = fit * scale;
              const int dx = static_cast<int>(std::lround((x - pressAt->x) / unit));
              const int dy = static_cast<int>(std::lround((y - pressAt->y) / unit));
              if ((dx == 0 && dy == 0) || !r.active) {
                if (select) {
                  select(name);
                }
                return;
              }
              const auto [nx, ny] = snappedPosition(rects, r, dx, dy);
              if (overlapsAny(rects, r, nx, ny)) {
                return;
              }
              OutputHeadConfig next = config;
              next.x = nx;
              next.y = ny;
              edit(std::move(next));
            },
            .configure = [r, fit, scale, minX, minY, name = heads[r.index].name](Button& button) {
              button.setTabStop(false);
              button.setDirection(FlexDirection::Vertical);
              button.setAlign(FlexAlign::Stretch);
              // The output shows its wallpaper (mockup settings-v2 Displays); off outputs stay a plain fill.
              auto screen = ui::column({
                  .align = FlexAlign::Center,
                  .justify = FlexJustify::Center,
                  .fill = colorSpecFromRole(ColorRole::Fill2),
                  .radius = (Style::spaceSm - Style::focusRingWidth - Style::focusRingGap) * scale,
                  .flexGrow = 1.0F,
              });
              if (r.active) {
                applyWallpaperIllustration(*screen);
              }
              screen->addChild(ui::label({
                  .text = name,
                  .fontSize = Style::fontSizeCaption * scale,
                  .fontWeight = FontWeight::SemiBold,
                  .color = r.active ? fixedColorSpec(rgbHex(0xFFFFFF)) : colorSpecFromRole(ColorRole::Label3),
                  .maxLines = 1,
              }));
              button.addChild(std::move(screen));
              button.setParticipatesInLayout(false);
              button.setPosition(
                  static_cast<float>(r.x - minX) * fit * scale, static_cast<float>(r.y - minY) * fit * scale
              );
            },
        }));
      }
      auto plane = ui::row(
          {.align = FlexAlign::Center,
           .justify = FlexJustify::Center,
           .minHeight = kCanvasHeight * scale,
           .fillWidth = true},
          std::move(layout)
      );
      addSettingsGroupCard(content, {}, scale)->addChild(std::move(plane));
    }

  } // namespace

  void addSettingsDisplays(Flex& content, const SettingsDisplaysContext& ctx) {
    if (ctx.outputs == nullptr || !ctx.outputs->ready()) {
      content.addChild(makeSettingSubtitleLabel(i18n::tr("settings.displays.unavailable"), ctx.scale));
      return;
    }
    const auto& heads = ctx.outputs->heads();
    addArrangementCanvas(content, ctx);
    const std::string selectedName = selectedHeadName(ctx);
    for (std::size_t i = 0; i < heads.size() && i < ctx.edits.size(); ++i) {
      if (heads[i].name == selectedName) {
        addDisplayCard(content, heads[i], ctx.edits[i], ctx);
      }
    }
    addActions(content, ctx);
  }

} // namespace settings
