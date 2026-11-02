#include "shell/settings/settings_control_factory.h"
#include "shell/settings/settings_pages.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "i18n/i18n.h"
#include "render/scene/input_area.h"
#include "render/scene/rect_node.h"
#include "shell/bar/widget_action.h"
#include "shell/bar/widget_gesture.h"
#include "shell/settings/color_spec_picker.h"
#include "shell/settings/path_browse.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_selection_style.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/list_editor.h"
#include "ui/controls/segmented.h"
#include "ui/controls/select.h"
#include "ui/controls/slider.h"
#include "ui/controls/stepper.h"
#include "ui/controls/toggle.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <unordered_set>
#include <utility>

namespace settings {

  namespace {
    // Fixed-width slot so unit suffixes (%, °C, MB/s, s) left-align into one column across rows,
    // keeping every slider's value box at the same right edge.
    constexpr float kSuffixSlotWidth = 36.0F;

    std::unique_ptr<Node> makeSuffixSlot(std::string suffix, float scale) {
      if (suffix.empty()) {
        return nullptr;
      }
      return ui::row(
          {.align = FlexAlign::Center, .justify = FlexJustify::Start, .width = kSuffixSlotWidth * scale},
          ui::label({
              .text = std::move(suffix),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::Label2),
          })
      );
    }

    // Horizontal space the leading invert slot occupies: corner glyph + gap + a Small toggle.
    // Invertible sliders give this much (plus the row gap) back from their track so the toggle tucks
    // in on the left without widening the control cluster.
    TOKENS_LIVE(
        kInvertSlotContentWidth,
        Style::fontSizeBody + Style::spaceXs + Style::toggleThumbSizeSm + (2.0F * Style::toggleInsetSm)
            + Style::toggleTravelSm
    );

    // Leading slot carrying the concave-corner invert toggle: a corner glyph (labelling the toggle
    // in lieu of a text caption) plus a Small toggle. The slot sizes to its content. Reserve builds
    // the same glyph+toggle but invisible, so a slider without a toggle keeps its slider and value
    // box column-aligned with sibling sliders while showing no controls.
    std::unique_ptr<Node> makeInvertSlot(
        SliderSetting::InvertSlot slot, bool enabled, const std::shared_ptr<bool>& inverted,
        std::function<void(double)> commitValue, Slider* sliderPtr, float scale
    ) {
      if (slot == SliderSetting::InvertSlot::None) {
        return nullptr;
      }
      const bool placeholder = slot == SliderSetting::InvertSlot::Reserve;
      const std::optional<bool> hidden = placeholder ? std::optional<bool>{false} : std::nullopt;
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});

      row->addChild(
          ui::glyph({
              .glyph = "border-corner-pill",
              .glyphSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(enabled ? ColorRole::Label2 : ColorRole::Stroke),
              .visible = hidden,
          })
      );
      ui::ToggleProps toggleProps{
          .checked = *inverted,
          .enabled = enabled,
          .toggleSize = ToggleSize::Small,
          .scale = scale,
          .visible = hidden,
      };
      if (!placeholder && enabled) {
        toggleProps.onChange = [inverted, commitValue = std::move(commitValue), sliderPtr](bool on) {
          *inverted = on;
          commitValue(sliderPtr->value());
        };
      }
      row->addChild(ui::toggle(std::move(toggleProps)));
      return row;
    }

    std::string joinSettingPath(const std::vector<std::string>& path) {
      std::string joined;
      joined.reserve(64);
      for (std::size_t i = 0; i < path.size(); ++i) {
        if (i > 0) {
          joined += '.';
        }
        joined += path[i];
      }
      return joined;
    }

    bool tagTabFocusKey(Node& root, const std::string& key) {
      if (auto* segmented = dynamic_cast<Segmented*>(&root)) {
        if (InputArea* area = segmented->focusArea(); area != nullptr) {
          area->setTabFocusKey(key);
          return true;
        }
      }
      if (auto* area = dynamic_cast<InputArea*>(&root); area && area->focusable() && area->tabStop()) {
        area->setTabFocusKey(key);
        return true;
      }

      for (const auto& child : root.children()) {
        if (tagTabFocusKey(*child, key)) {
          return true;
        }
      }
      return false;
    }

    // Synthetic picker entry: a grammar keyword rather than an IPC command, so it cannot collide
    // with a real command name.
    constexpr std::string_view kActionExecOption = "\x01exec";

    // Argument specs spell required arguments as <id> and optional ones as [context]. A <> nested
    // inside brackets is still optional, so only a top-level one makes the argument mandatory.
    [[nodiscard]] bool specTakesArguments(std::string_view spec) { return !spec.empty(); }

    [[nodiscard]] bool specRequiresArgument(std::string_view spec) {
      int optionalDepth = 0;
      for (const char c : spec) {
        if (c == '[') {
          ++optionalDepth;
        } else if (c == ']') {
          optionalDepth = std::max(0, optionalDepth - 1);
        } else if (c == '<' && optionalDepth == 0) {
          return true;
        }
      }
      return false;
    }

  } // namespace

  SettingsControlFactory::SettingsControlFactory(SettingsContentContext ctx)
      : m_ctx(std::move(ctx)), m_scale(m_ctx.scale) {}

  bool SettingsControlFactory::isTemplateEnableTogglePath(const std::vector<std::string>& path) {
    return path == std::vector<std::string>{"theme", "templates", "enable_builtin_templates"}
    || path == std::vector<std::string>{"theme", "templates", "enable_community_templates"};
  }

  std::unique_ptr<Button> SettingsControlFactory::makeResetButton(const std::vector<std::string>& path) {
    return makeGroupedResetButton(std::vector<std::vector<std::string>>{path});
  }

  std::unique_ptr<Button>
  SettingsControlFactory::makeResetButton(const std::vector<std::string>& path, std::function<void()> onConfirmed) {
    return makeGroupedResetButton(std::vector<std::vector<std::string>>{path}, std::move(onConfirmed));
  }

  std::unique_ptr<Button> SettingsControlFactory::makeGroupedResetButton(
      std::vector<std::vector<std::string>> paths, std::function<void()> onConfirmed
  ) {
    auto& ctx = m_ctx;
    const float scale = m_scale;
    const bool pendingConfirmation = ctx.isResetConfirmationPending && ctx.isResetConfirmationPending(paths);
    return ui::button({
        .text = pendingConfirmation ? std::optional{i18n::tr("settings.actions.confirm-reset")} : std::nullopt,
        .glyph = "arrow-back-up",
        .fontSize = Style::fontSizeCaption * scale,
        .glyphSize = Style::fontSizeBody * scale,
        .variant = pendingConfirmation ? ButtonVariant::Destructive : ButtonVariant::Ghost,
        .tooltip = i18n::tr("settings.actions.reset-to-default"),
        .minWidth = Style::controlHeight * scale,
        .minHeight = Style::controlHeight * scale,
        .paddingV = Style::spaceXs * scale,
        .paddingH = Style::spaceXs * scale,
        .radius = Style::scaledRadiusMd(scale),
        .onClick = [clearOverrides = ctx.clearOverrides, requestConfirmation = ctx.requestResetConfirmation,
                    requestRebuild = ctx.requestRebuild, paths = std::move(paths), onConfirmed = std::move(onConfirmed),
                    pendingConfirmation] mutable {
          if (!pendingConfirmation) {
            requestConfirmation(paths);
            requestRebuild();
            return;
          }
          if (onConfirmed) {
            onConfirmed();
            return;
          }
          clearOverrides(std::move(paths));
        },
    });
  }

  std::unique_ptr<Flex> SettingsControlFactory::makeStatusBadge(
      std::string_view label, const ColorSpec& fill, const ColorSpec& color, bool matchResetHeight
  ) {
    const float scale = m_scale;
    return ui::row(
        {.align = FlexAlign::Center,
         .paddingV = matchResetHeight ? Style::spaceXs * scale : 0.0F,
         .paddingH = matchResetHeight ? Style::spaceSm * scale : Style::spaceXs * scale,
         .fill = fill,
         .radius = Style::scaledRadiusSm(scale),
         .minHeight = matchResetHeight ? std::optional<float>{Style::controlHeight * scale} : std::nullopt},
        makeLabel(label, Style::fontSizeCaption * scale, color, FontWeight::Bold)
    );
  }

  std::unique_ptr<Flex> SettingsControlFactory::makeOverrideResetActions(const std::vector<std::string>& path) {
    const float scale = m_scale;
    return ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale}, makeResetButton(path));
  }

  namespace {

    // R7: the tile radius minus its ring and gap.
    constexpr float kPreviewRadius = metrics::kPickerRadius - Style::focusRingWidth - Style::focusRingGap;

    // Mode illustration (mockup .agent/mockups/settings.html A): wallpaper, two bar pills and a window in the mode's
    // colours; auto splits all three down the middle. 116 x 74 at r 10 (R7 inside the 12 tile with its 2 + 2 ring).
    constexpr float kModeThumbWidth = 116.0F;
    constexpr float kModeThumbHeight = 74.0F;

    std::unique_ptr<RectNode> modeLayer(float x, float y, float w, float h, float radius, std::array<Color, 3> stops,
                                        bool split) {
      auto rect = std::make_unique<RectNode>();
      rect->setParticipatesInLayout(false);
      rect->setPosition(x, y);
      rect->setFrameSize(w, h);
      const float mid = split ? 0.5F : 0.6F;
      rect->setStyle(RoundedRectStyle{
          .fill = stops[0],
          .border = clearColor(),
          .fillMode = FillMode::LinearGradient,
          .gradientDirection = GradientDirection::Horizontal,
          .gradientStops = {GradientStop{0.0F, stops[0]}, GradientStop{mid, stops[split ? 0 : 1]},
                            GradientStop{mid, stops[split ? 2 : 1]}, GradientStop{1.0F, stops[2]}},
          .radius = radius,
          .softness = 1.0F,
          .borderWidth = 0.0F,
      });
      return rect;
    }

    std::unique_ptr<Flex> makeModePreview(std::string_view value, float scale) {
      const bool dark = value == "dark";
      const bool split = value == "auto";
      const float w = kModeThumbWidth * scale;
      const float h = kModeThumbHeight * scale;
      const Color wpLight0 = rgbHex(0xF6C9A8);
      const Color wpLight1 = rgbHex(0x9FB6FF);
      const Color wpLight2 = rgbHex(0x8FE6D2);
      const Color wpDark0 = rgbHex(0x24161C);
      const Color wpDark1 = rgbHex(0x121A33);
      const Color wpDark2 = rgbHex(0x0C2A28);
      const Color winLight = rgbHex(0xF2F2F6);
      const Color winDark = rgbHex(0x1E1E22);
      const Color pillLight{1.0F, 1.0F, 1.0F, 0.7F};
      const Color pillDark{28.0F / 255.0F, 28.0F / 255.0F, 32.0F / 255.0F, 0.75F};

      auto thumb = ui::column({
          .radius = kPreviewRadius * scale,
          .border = colorSpecFromRole(ColorRole::Separator),
          .borderWidth = 0.5F * scale,
          .width = w,
          .height = h,
      });
      std::array<Color, 3> wallpaper = dark ? std::array{wpDark0, wpDark1, wpDark2}
          : split                         ? std::array{wpLight0, wpLight0, wpDark1}
                                          : std::array{wpLight0, wpLight1, wpLight2};
      thumb->addChild(modeLayer(0.0F, 0.0F, w, h, kPreviewRadius * scale, wallpaper, split));
      const float pillW = 22.0F * scale;
      const float pillH = 7.0F * scale;
      const float inset = 6.0F * scale;
      const Color pillStart = dark ? pillDark : pillLight;
      const Color pillEnd = dark || split ? pillDark : pillLight;
      thumb->addChild(modeLayer(inset, 5.0F * scale, pillW, pillH, pillH * 0.5F, {pillStart, pillStart, pillStart}, false));
      thumb->addChild(modeLayer(w - inset - pillW, 5.0F * scale, pillW, pillH, pillH * 0.5F, {pillEnd, pillEnd, pillEnd}, false));
      const Color winStart = dark ? winDark : winLight;
      const Color winEnd = dark || split ? winDark : winLight;
      // The auto window straddles the split: its own halves meet at the thumbnail's centre line.
      const float winX = split ? w * 0.5F - 32.0F * scale : 18.0F * scale;
      thumb->addChild(modeLayer(winX, 20.0F * scale, 64.0F * scale, 42.0F * scale, 5.0F * scale,
                                {winStart, winStart, winEnd}, split));
      return thumb;
    }

  } // namespace

  void SettingsControlFactory::makeRow(Flex& section, const SettingEntry& entry, std::unique_ptr<Node> control) {
    auto& ctx = m_ctx;
    const float scale = m_scale;
    const Config& cfg = m_ctx.config;
    // Range sliders own a second config path (high/critical); both reset and report "override" together.
    const auto* rangeSlider = std::get_if<RangeSliderSetting>(&entry.control);
    const auto* selectSetting = std::get_if<SelectSetting>(&entry.control);
    const bool overridden = ctx.configService != nullptr
        && settingEntryHasEffectiveOverride(entry, *ctx.configService, ctx.compositorSettings);
    const bool redundantGuiOverride =
        ctx.configService != nullptr && ctx.configService->hasOverride(entry.path) && !overridden;
    const bool monitorSetting = isMonitorOverrideSettingPath(entry.path);
    const bool monitorExplicit = monitorOverrideHasExplicitValue(cfg, entry.path) && !redundantGuiOverride;
    const bool monitorInherited = monitorSetting && !monitorExplicit;

    auto titleRow = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .fillWidth = true,
    });
    titleRow->addChild(
        makeLabel(entry.title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::Label1), FontWeight::Medium)
    );
    if (monitorExplicit) {
      titleRow->addChild(makeStatusBadge(
          i18n::tr("settings.badges.monitor"), colorSpecFromRole(ColorRole::Fill1),
          colorSpecFromRole(ColorRole::Label1), false
      ));
    } else if (monitorInherited) {
      titleRow->addChild(makeStatusBadge(
          i18n::tr("settings.badges.inherited"), colorSpecFromRole(ColorRole::Fill2),
          colorSpecFromRole(ColorRole::Label2), false
      ));
    }
    titleRow->addChild(ui::spacer());

    auto actions = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
    if (overridden) {
      if (rangeSlider != nullptr) {
        actions->addChild(
            makeGroupedResetButton(std::vector<std::vector<std::string>>{entry.path, rangeSlider->highPath})
        );
      } else if (selectSetting != nullptr && !selectSetting->linkedPath.empty()) {
        actions->addChild(
            makeGroupedResetButton(std::vector<std::vector<std::string>>{entry.path, selectSetting->linkedPath})
        );
      } else {
        actions->addChild(makeResetButton(entry.path));
      }
    }
    tagTabFocusKey(*control, joinSettingPath(entry.path));
    actions->addChild(std::move(control));

    auto row = layoutRow(
        std::move(titleRow), settingEntryShowsCaption(entry) ? std::string_view{entry.subtitle} : std::string_view{},
        !isTemplateEnableTogglePath(entry.path), std::move(actions), scale
    );
    section.addChild(std::move(row));
  }

  std::unique_ptr<Flex> SettingsControlFactory::makeRow(
      std::string_view title, std::unique_ptr<Node> control, float scale, std::string_view subtitle
  ) {
    auto titleRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
    titleRow->addChild(
        makeLabel(title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::Label1), FontWeight::Medium)
    );
    titleRow->addChild(ui::spacer());
    auto actions = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
    actions->addChild(std::move(control));
    return layoutRow(std::move(titleRow), subtitle, true, std::move(actions), scale);
  }

  std::unique_ptr<Flex> SettingsControlFactory::layoutRow(
      std::unique_ptr<Flex> titleRow, std::string_view subtitle, bool tightCopy, std::unique_ptr<Flex> actions,
      float scale
  ) {
    ui::FlexProps copyProps{.align = FlexAlign::Start, .flexGrow = 1.0F};
    if (tightCopy) {
      copyProps.gap = Style::spaceXs * scale;
    }
    auto copy = ui::column(copyProps);
    copy->addChild(std::move(titleRow));
    if (!subtitle.empty()) {
      copy->addChild(makeSettingSubtitleLabel(subtitle, scale));
    }
    return ui::row(
        {.align = FlexAlign::Center,
         .justify = FlexJustify::SpaceBetween,
         .gap = Style::spaceXs * scale,
         .paddingV = 2.0F * scale,
         .paddingH = 0.0F,
         .minHeight = (subtitle.empty() ? metrics::kRowHeight : metrics::kRowHeightTwoLine) * scale},
        std::move(copy), std::move(actions)
    );
  }

  std::unique_ptr<Toggle> SettingsControlFactory::makeToggle(
      bool checked, bool enabled, const std::vector<std::string>& path, std::optional<bool> clearWhenValue
  ) {
    auto& ctx = m_ctx;
    const float scale = m_scale;
    if (enabled) {
      return ui::toggle({
          .checked = checked,
          .enabled = enabled,
          .scale = scale,
          .onChange = [configService = ctx.configService, setOverride = ctx.setOverride,
                       clearOverride = ctx.clearOverride, requestRebuild = ctx.requestRebuild, path,
                       clearWhenValue](bool value) {
            if (clearWhenValue.has_value()
                && value == *clearWhenValue
                && configService != nullptr
                && configService->hasOverride(path)) {
              clearOverride(path);
            } else {
              setOverride(path, value);
            }
            if (requestRebuild) {
              requestRebuild();
            }
          },
      });
    }
    return ui::toggle({
        .checked = checked,
        .enabled = enabled,
        .scale = scale,
    });
  }

  std::unique_ptr<Node>
  SettingsControlFactory::makeSelect(const SelectSetting& setting, const std::vector<std::string>& path) {
    auto& ctx = m_ctx;
    const float scale = m_scale;
    if (setting.tiles) {
      const bool swatches = !path.empty() && path.back() == "builtin";
      const auto commit = [setOverride = ctx.setOverride, requestRebuild = ctx.requestRebuild, path](std::string value) {
        setOverride(path, std::move(value));
        if (requestRebuild) {
          requestRebuild();
        }
      };
      const auto ringed = [scale](bool selected, const ColorSpec& ring) {
        const ColorSpec none = clearColorSpec();
        const Button::ButtonStateColors state{.bg = none, .border = selected ? ring : none, .label = none};
        return Button::ButtonPalette{
            .borderWidth = Style::focusRingWidth * scale, .normal = state, .hover = state, .pressed = state,
            .disabled = state
        };
      };
      if (swatches) {
        // The first palettes as 20 px circles; the last circle opens the whole catalog.
        constexpr std::size_t kShown = 5;
        auto strip = ui::row({.align = FlexAlign::Center, .gap = Style::focusRingGap * scale});
        std::vector<SelectOption> shown;
        for (const auto& opt : setting.options) {
          if (shown.size() < kShown) {
            shown.push_back(opt);
          }
        }
        // The positions stay fixed; a choice beyond them rings the catalog button.
        const bool hiddenSelected = !setting.selectedValue.empty()
            && std::ranges::none_of(shown, [&](const SelectOption& o) { return o.value == setting.selectedValue; });
        const ColorSpec ringFill = colorSpecFromRole(ColorRole::Fill2);
        const Button::ButtonStateColors catalogState{
            .bg = ringFill, .border = hiddenSelected ? colorSpecFromRole(ColorRole::Label3) : clearColorSpec(),
            .label = colorSpecFromRole(ColorRole::Label2)
        };
        const float circle = metrics::kSwatchSize * scale;
        for (const auto& opt : shown) {
          const bool selected = opt.value == setting.selectedValue;
          const ColorSpec fill = opt.preview.swatches.empty() ? colorSpecFromRole(ColorRole::Accent) : opt.preview.swatches.front();
          auto dot = ui::button({
              .variant = ButtonVariant::Ghost,
              .customPalette = ringed(selected, colorSpecFromRole(ColorRole::Label3)),
              .tooltip = opt.label,
              .minWidth = circle + 2.0F * Style::focusRingWidth * scale + 2.0F * Style::focusRingGap * scale,
              .minHeight = circle + 2.0F * Style::focusRingWidth * scale + 2.0F * Style::focusRingGap * scale,
              .padding = Style::focusRingGap * scale,
              .radius = circle,
              .onClick = [commit, value = opt.value] { commit(value); },
              .configure = [fill, circle](Button& button) {
                button.setTabStop(false);
                button.addChild(ui::box({.fill = fill, .radius = circle * 0.5F, .width = circle, .height = circle}));
              },
          });
          strip->addChild(std::move(dot));
        }
        strip->addChild(ui::button({
            .glyph = "chevron-down",
            .glyphSize = Style::fontSizeCaption * scale,
            .variant = ButtonVariant::Tonal,
            .customPalette = Button::ButtonPalette{
                .borderWidth = Style::focusRingWidth * scale, .normal = catalogState, .hover = catalogState,
                .pressed = catalogState, .disabled = catalogState
            },
            .tooltip = i18n::tr("settings.schema.appearance.builtin-palette.label"),
            .minWidth = circle,
            .minHeight = circle,
            .padding = 0.0F,
            .radius = circle * 0.5F,
            .onClick = [openPopup = ctx.openSearchPickerPopup, options = setting.options,
                        selectedValue = setting.selectedValue, path, commit] {
              if (openPopup) {
                openPopup(SearchPickerOpenRequest{
                    .title = i18n::tr("settings.schema.appearance.builtin-palette.label"),
                    .options = options,
                    .selectedValue = selectedValue,
                    .settingPath = path,
                    .onSelect = commit,
                });
              }
            },
            .configure = [](Button& button) { button.setTabStop(false); },
        }));
        return strip;
      }
      // Appearance thumbnails: a miniature screen per mode, the chosen one ringed.
      auto strip = ui::row({.align = FlexAlign::Start, .justify = FlexJustify::Center, .gap = 22.0F * scale,
                            .fillWidth = true});
      auto ordered = setting.options;
      std::ranges::stable_sort(ordered, {}, [](const SelectOption& option) {
        return option.value == "auto" ? 0 : option.value == "light" ? 1 : 2;
      });
      for (const auto& opt : ordered) {
        const bool selected = opt.value == setting.selectedValue;
        auto tile = ui::button({
            .variant = ButtonVariant::Ghost,
            .customPalette = ringed(selected, colorSpecFromRole(ColorRole::Label1)),
            .padding = (Style::focusRingWidth + Style::focusRingGap) * scale,
            .radius = metrics::kPickerRadius * scale,
            .onClick = [commit, value = opt.value] { commit(value); },
            .configure = [value = opt.value, scale, selected](Button& button) {
              button.setTabStop(false);
              button.setFlexGrow(0.0F);
              button.addChild(makeModePreview(value, scale));
              // macOS marks the mode by an accent ring alone; a badge would cover the bar pill.
              button.setCheckBadge(selected, scale, metrics::kPickerRadius * scale, /*badge=*/false,
                                   colorSpecFromRole(ColorRole::Accent));
            },
        });
        strip->addChild(ui::column(
            {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
            std::move(tile),
            ui::row(
                {.justify = FlexJustify::Center},
                makeLabel(
                    opt.label, Style::fontSizeCaption * scale,
                    colorSpecFromRole(selected ? ColorRole::Label1 : ColorRole::Label2),
                    selected ? FontWeight::Bold : FontWeight::Medium
                )
            )
        ));
      }
      return strip;
    }
    if (setting.segmented) {
      const auto& options = setting.options;
      std::vector<ui::SegmentedOption> segmentedOptions;
      segmentedOptions.reserve(options.size());
      for (const auto& opt : options) {
        segmentedOptions.push_back(ui::SegmentedOption{.label = opt.label});
      }
      const SelectValueType valueType = setting.valueType;
      const auto groupedCommit = setting.groupedCommit;
      return ui::segmented({
          .options = std::move(segmentedOptions),
          .selectedIndex = optionIndex(options, setting.selectedValue),
          .scale = scale,
          .onChange = [setOverride = ctx.setOverride, setOverrides = ctx.setOverrides,
                       clearOverride = ctx.clearOverride, requestRebuild = ctx.requestRebuild, path, options, valueType,
                       groupedCommit](std::size_t index) {
            if (index < options.size()) {
              if (groupedCommit) {
                setOverrides(groupedCommit(options[index].value, path));
              } else if (options[index].value.empty() && valueType == SelectValueType::Integer) {
                clearOverride(path);
              } else if (valueType == SelectValueType::Integer) {
                setOverride(path, static_cast<std::int64_t>(std::stoll(options[index].value)));
              } else if (valueType == SelectValueType::Boolean) {
                setOverride(path, options[index].value == "true");
              } else {
                setOverride(path, options[index].value);
              }
              if (requestRebuild) {
                requestRebuild();
              }
            }
          },
      });
    }

    const auto selectedIndex = optionIndex(setting.options, setting.selectedValue);
    const bool missingSelection = !selectedIndex.has_value();
    const bool unknownValue = missingSelection && !setting.selectedValue.empty();
    const bool clearSelection = unknownValue || (missingSelection && setting.allowEmptySelection);
    const float selectWidth = setting.preferredWidth > 0.0F ? setting.preferredWidth : 190.0F;
    auto options = setting.options;
    const bool clearOnEmpty = setting.clearOnEmpty;
    const SelectValueType valueType = setting.valueType;
    const auto groupedCommit = setting.groupedCommit;
    return ui::select({
        .options = optionLabels(setting.options),
        .selectedIndex = selectedIndex,
        .clearSelection = clearSelection,
        .placeholder = unknownValue ? std::optional<std::string>{i18n::tr(
                                          "settings.controls.select.unknown-value", "value", setting.selectedValue
                                      )}
                                    : std::nullopt,
        .fontSize = Style::fontSizeBody * scale,
        .controlHeight = Style::controlHeight * scale,
        .glyphSize = Style::fontSizeBody * scale,
        .colorSwatchPreviews = optionSwatchPreviews(setting.options),
        .width = selectWidth * scale,
        .height = Style::controlHeight * scale,
        .onSelectionChanged = [clearOverride = ctx.clearOverride, setOverride = ctx.setOverride,
                               setOverrides = ctx.setOverrides, path, options, clearOnEmpty, valueType,
                               groupedCommit](std::size_t index, std::string_view /*label*/) {
          if (index < options.size()) {
            if (groupedCommit) {
              setOverrides(groupedCommit(options[index].value, path));
              return;
            }
            if (options[index].value.empty() && (clearOnEmpty || valueType == SelectValueType::Integer)) {
              clearOverride(path);
              return;
            }
            if (valueType == SelectValueType::Integer) {
              setOverride(path, static_cast<std::int64_t>(std::stoll(options[index].value)));
            } else if (valueType == SelectValueType::Boolean) {
              setOverride(path, options[index].value == "true");
            } else {
              setOverride(path, options[index].value);
            }
          }
        },
    });
  }

  std::unique_ptr<Node> SettingsControlFactory::makeSearchPicker(
      const SearchPickerSetting& setting, std::string title, std::vector<std::string> path
  ) {
    auto& ctx = m_ctx;
    const float scale = m_scale;
    return ui::button({
        .text = optionLabel(setting.options, setting.selectedValue),
        .fontSize = Style::fontSizeBody * scale,
        .contentAlign = ButtonContentAlign::Start,
        .variant = ButtonVariant::Default,
        .minWidth = 190.0F * scale,
        .minHeight = Style::controlHeight * scale,
        .paddingV = Style::spaceSm * scale,
        .paddingH = Style::spaceMd * scale,
        .radius = Style::scaledRadiusMd(scale),
        .onClick = [openPopup = ctx.openSearchPickerPopup, title = std::move(title), options = setting.options,
                    selectedValue = setting.selectedValue, placeholder = setting.placeholder,
                    emptyText = setting.emptyText, path = std::move(path), onSelect = setting.onSelect] {
          if (openPopup) {
            openPopup(
                SearchPickerOpenRequest{
                    .title = title,
                    .options = options,
                    .selectedValue = selectedValue,
                    .placeholder = placeholder,
                    .emptyText = emptyText,
                    .settingPath = path,
                    .onSelect = onSelect,
                }
            );
          }
        },
    });
  }

  std::unique_ptr<Flex> SettingsControlFactory::makeSlider(
      double value, double minValue, double maxValue, double step, const std::vector<std::string>& path,
      bool integerValue,
      const std::function<std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>(double)>& linkedCommit,
      std::string valueSuffix, SliderSetting::InvertSlot invertSlot, bool invertEnabled
  ) {
    auto& ctx = m_ctx;
    const float scale = m_scale;
    auto wrap = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});

    // Signed radius sliders show the magnitude; a leading toggle carries the sign.
    const bool invertible = invertSlot == SliderSetting::InvertSlot::Toggle;
    auto inverted = std::make_shared<bool>(value < 0.0);
    const double magnitude = std::abs(value);
    const double sliderValue = invertible ? magnitude : value;

    // Narrow the track by the leading slot so the cluster keeps its normal total width.
    const float sliderWidth = (invertSlot == SliderSetting::InvertSlot::None
                                   ? Style::sliderDefaultWidth
                                   : Style::sliderDefaultWidth - kInvertSlotContentWidth - Style::spaceSm)
        * scale;

    Input* valueInputPtr = nullptr;
    auto valueInput = ui::input({
        .out = &valueInputPtr,
        .value = formatSliderValue(sliderValue, integerValue),
        .fontSize = Style::fontSizeCaption * scale,
        .controlHeight = Style::controlHeight * scale,
        .horizontalPadding = Style::spaceXs * scale,
        .width = 50.0F * scale,
        .height = Style::controlHeight * scale,
    });

    Slider* sliderPtr = nullptr;
    auto slider = ui::slider({
        .out = &sliderPtr,
        .minValue = minValue,
        .maxValue = maxValue,
        .step = step,
        .value = sliderValue,
        .trackHeight = Style::sliderTrackHeight * scale,
        .thumbSize = Style::sliderThumbSize * scale,
        .controlHeight = Style::controlHeight * scale,
        .width = sliderWidth,
        .height = Style::controlHeight * scale,
        .onValueChanged = [valueInputPtr, integerValue](double next) {
          valueInputPtr->setInvalid(false);
          valueInputPtr->setValue(formatSliderValue(next, integerValue));
        },
    });
    valueInputPtr->setValue(formatSliderValue(sliderPtr->value(), integerValue));

    // Helper: commit either via single setOverride or as an atomic batch when linkedCommit
    // returns extra overrides (cross-field constraints).
    const auto commit = [setOverride = ctx.setOverride, setOverrides = ctx.setOverrides, path, integerValue,
                         linkedCommit](double v) {
      ConfigOverrideValue primary =
          integerValue ? ConfigOverrideValue{static_cast<std::int64_t>(std::lround(v))} : ConfigOverrideValue{v};
      if (linkedCommit) {
        auto extras = linkedCommit(v);
        if (!extras.empty()) {
          std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> all;
          all.reserve(extras.size() + 1);
          all.emplace_back(path, std::move(primary));
          for (auto& e : extras) {
            all.push_back(std::move(e));
          }
          setOverrides(std::move(all));
          return;
        }
      }
      setOverride(path, std::move(primary));
    };

    // For invertible sliders the slider value is the magnitude; fold in the sign the toggle holds.
    std::function<void(double)> commitValue = commit;
    if (invertible) {
      commitValue = [commit, inverted](double magValue) { commit(*inverted ? -magValue : magValue); };
    }

    slider->setOnDragEnd([commitValue, sliderPtr] { commitValue(sliderPtr->value()); });

    const auto commitInputText = [commitValue, sliderPtr, valueInputPtr, minValue, maxValue,
                                  integerValue](const std::string& text) {
      const auto parsed = parseDoubleInput(text);
      if (!parsed.has_value() || *parsed < minValue || *parsed > maxValue) {
        valueInputPtr->setInvalid(true);
        return false;
      }
      const double v = *parsed;
      valueInputPtr->setInvalid(false);
      sliderPtr->setValue(v);
      const double snapped = sliderPtr->value();
      valueInputPtr->setValue(formatSliderValue(snapped, integerValue));
      commitValue(snapped);
      return true;
    };

    valueInput->setOnChange([valueInputPtr](const std::string& /*text*/) { valueInputPtr->setInvalid(false); });
    valueInput->setOnSubmit([commitInputText](const std::string& text) { (void)commitInputText(text); });
    valueInput->setOnFocusLoss([commitInputText, valueInputPtr] { (void)commitInputText(valueInputPtr->value()); });

    // Invert toggle leads, then slider, then the numeric value field on the right (reset from makeRow
    // stays left of this cluster).
    if (auto invert = makeInvertSlot(invertSlot, invertEnabled, inverted, commitValue, sliderPtr, scale)) {
      wrap->addChild(std::move(invert));
    }
    const std::string joined = joinSettingPath(path);
    std::pair<std::string, std::string> ends;
    if (joined == "accessibility.ui_scale") {
      ends = {"A", "A"};
    } else if (joined == "dock.icon_size") {
      ends = {i18n::tr("settings.sliders.small"), i18n::tr("settings.sliders.large")};
    }
    const bool labelled = !ends.first.empty();
    const auto endLabel = [&](const std::string& text, bool large) {
      return makeLabel(
          text, (large ? Style::fontSizeTitle : Style::fontSizeCaption) * scale,
          colorSpecFromRole(ColorRole::Label2)
      );
    };
    if (labelled) {
      wrap->addChild(endLabel(ends.first, false));
    }
    wrap->addChild(std::move(slider));
    if (labelled) {
      wrap->addChild(endLabel(ends.second, true));
      valueInput->setVisible(false);
      valueInput->setParticipatesInLayout(false);
    }
    wrap->addChild(std::move(valueInput));
    if (auto suffix = makeSuffixSlot(std::move(valueSuffix), scale)) {
      wrap->addChild(std::move(suffix));
    }
    return wrap;
  }

  std::unique_ptr<Flex>
  SettingsControlFactory::makeRangeSlider(const RangeSliderSetting& setting, const std::vector<std::string>& lowPath) {
    auto& ctx = m_ctx;
    const float scale = m_scale;
    const bool integerValue = setting.integerValue;
    const std::vector<std::string> highPath = setting.highPath;
    auto wrap = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});

    auto makeValueInput = [&](double value, Input** out) {
      return ui::input({
          .out = out,
          .value = formatSliderValue(value, integerValue),
          .fontSize = Style::fontSizeCaption * scale,
          .controlHeight = Style::controlHeight * scale,
          .horizontalPadding = Style::spaceXs * scale,
          .width = 50.0F * scale,
          .height = Style::controlHeight * scale,
      });
    };

    Input* lowInputPtr = nullptr;
    Input* highInputPtr = nullptr;
    auto lowInput = makeValueInput(setting.lowValue, &lowInputPtr);
    auto highInput = makeValueInput(setting.highValue, &highInputPtr);

    RangeSlider* sliderPtr = nullptr;
    auto slider = ui::rangeSlider({
        .out = &sliderPtr,
        .minValue = setting.minValue,
        .maxValue = setting.maxValue,
        .step = setting.step,
        .lowValue = setting.lowValue,
        .highValue = setting.highValue,
        .trackHeight = Style::sliderTrackHeight * scale,
        .thumbSize = Style::sliderThumbSize * scale,
        .controlHeight = Style::controlHeight * scale,
        .width = Style::sliderDefaultWidth * scale,
        .height = Style::controlHeight * scale,
        .onLowChanged =
            [lowInputPtr, integerValue](double next) {
              lowInputPtr->setInvalid(false);
              lowInputPtr->setValue(formatSliderValue(next, integerValue));
            },
        .onHighChanged =
            [highInputPtr, integerValue](double next) {
              highInputPtr->setInvalid(false);
              highInputPtr->setValue(formatSliderValue(next, integerValue));
            },
    });
    lowInputPtr->setValue(formatSliderValue(sliderPtr->lowValue(), integerValue));
    highInputPtr->setValue(formatSliderValue(sliderPtr->highValue(), integerValue));

    const auto commitTo = [setOverride = ctx.setOverride,
                           integerValue](const std::vector<std::string>& path, double v) {
      ConfigOverrideValue value =
          integerValue ? ConfigOverrideValue{static_cast<std::int64_t>(std::lround(v))} : ConfigOverrideValue{v};
      setOverride(path, std::move(value));
    };

    // Drag commits both ends together — they are one linked pair (reset clears both).
    slider->setOnDragEnd([commitTo, sliderPtr, lowPath, highPath] {
      commitTo(lowPath, sliderPtr->lowValue());
      commitTo(highPath, sliderPtr->highValue());
    });

    const auto commitInput = [commitTo, sliderPtr, integerValue](
                                 Input* input, const std::vector<std::string>& path, double minValue, double maxValue,
                                 bool isLow
                             ) {
      const auto parsed = parseDoubleInput(input->value());
      if (!parsed.has_value() || *parsed < minValue || *parsed > maxValue) {
        input->setInvalid(true);
        return false;
      }
      input->setInvalid(false);
      if (isLow) {
        sliderPtr->setLowValue(*parsed);
        input->setValue(formatSliderValue(sliderPtr->lowValue(), integerValue));
        commitTo(path, sliderPtr->lowValue());
      } else {
        sliderPtr->setHighValue(*parsed);
        input->setValue(formatSliderValue(sliderPtr->highValue(), integerValue));
        commitTo(path, sliderPtr->highValue());
      }
      return true;
    };

    const double minValue = setting.minValue;
    const double maxValue = setting.maxValue;
    lowInput->setOnChange([lowInputPtr](const std::string& /*text*/) { lowInputPtr->setInvalid(false); });
    lowInput->setOnSubmit([commitInput, lowInputPtr, lowPath, minValue, maxValue](const std::string& /*text*/) {
      (void)commitInput(lowInputPtr, lowPath, minValue, maxValue, true);
    });
    lowInput->setOnFocusLoss([commitInput, lowInputPtr, lowPath, minValue, maxValue] {
      (void)commitInput(lowInputPtr, lowPath, minValue, maxValue, true);
    });
    highInput->setOnChange([highInputPtr](const std::string& /*text*/) { highInputPtr->setInvalid(false); });
    highInput->setOnSubmit([commitInput, highInputPtr, highPath, minValue, maxValue](const std::string& /*text*/) {
      (void)commitInput(highInputPtr, highPath, minValue, maxValue, false);
    });
    highInput->setOnFocusLoss([commitInput, highInputPtr, highPath, minValue, maxValue] {
      (void)commitInput(highInputPtr, highPath, minValue, maxValue, false);
    });

    wrap->addChild(std::move(slider));
    wrap->addChild(std::move(lowInput));
    wrap->addChild(makeLabel("–", Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::Label2)));
    wrap->addChild(std::move(highInput));
    if (auto suffix = makeSuffixSlot(setting.valueSuffix, scale)) {
      wrap->addChild(std::move(suffix));
    }
    return wrap;
  }

  std::unique_ptr<Node> SettingsControlFactory::makeGestureActionRow(
      const GestureActionSetting& setting, const std::string& title, const std::vector<std::string>& path
  ) {
    auto& ctx = m_ctx;
    const float scale = m_scale;

    // Shared by value: the context is a build-pass local, but these callbacks are stored in the
    // scene and fire long after it is gone.
    const auto catalog = std::make_shared<const std::vector<GestureActionOption>>(ctx.actionCatalog);
    const auto specFor = [catalog](std::string_view verb) -> std::string {
      const auto it = std::ranges::find_if(*catalog, [verb](const GestureActionOption& action) {
        return action.option.value == verb;
      });
      return it != catalog->end() ? it->argsSpec : std::string{};
    };

    const bool isOverridden = !setting.configured.empty();
    const std::string effective = isOverridden ? setting.configured : setting.defaultAction;
    const auto parsed = noctalia::bar::parseWidgetAction(effective);

    // A row holds its chosen command until the argument is typed, because committing a bare verb
    // that needs one would store a binding that silently does nothing.
    const bool pending = ctx.pendingGestureKey == setting.gestureKey;
    std::string selected;
    std::string argument = !pending && parsed.has_value() ? parsed->args : std::string{};
    if (pending) {
      selected = ctx.pendingGestureVerb;
    } else if (!isOverridden) {
      selected.clear();
    } else if (!parsed.has_value() || parsed->kind == noctalia::bar::WidgetAction::Kind::None) {
      selected = std::string(noctalia::bar::kNoneVerb);
    } else if (parsed->kind == noctalia::bar::WidgetAction::Kind::Exec) {
      selected = std::string(kActionExecOption);
    } else {
      selected = parsed->verb;
    }

    // Keep the picker on "Default" for an inherited binding, but derive the argument editor from
    // the effective action so optional arguments such as volume/brightness step remain editable.
    std::string actionVerb = selected;
    if (!pending && parsed.has_value()) {
      if (parsed->kind == noctalia::bar::WidgetAction::Kind::Exec) {
        actionVerb = std::string(kActionExecOption);
      } else if (parsed->kind == noctalia::bar::WidgetAction::Kind::Ipc) {
        actionVerb = parsed->verb;
      }
    }
    const bool execMode = actionVerb == kActionExecOption;

    std::vector<SelectOption> options;
    options.reserve(ctx.actionCatalog.size() + 3);
    options.push_back(
        SelectOption{
            .value = {},
            .label = setting.defaultAction.empty()
                ? i18n::tr("settings.widgets.actions.unset")
                : std::format("{} ({})", i18n::tr("settings.widgets.actions.default"), setting.defaultAction),
        }
    );
    options.push_back(
        SelectOption{
            .value = std::string(noctalia::bar::kNoneVerb),
            .label = i18n::tr("settings.widgets.actions.disabled"),
        }
    );
    options.push_back(
        SelectOption{
            .value = std::string(kActionExecOption),
            .label = i18n::tr("settings.widgets.actions.run-command"),
        }
    );
    for (const auto& action : ctx.actionCatalog) {
      options.push_back(action.option);
    }

    SearchPickerSetting picker;
    picker.options = std::move(options);
    picker.selectedValue = selected;
    picker.emptyText = i18n::tr("ui.controls.search-picker.empty");
    picker.onSelect = [setOverride = ctx.setOverride, clearOverride = ctx.clearOverride,
                       requestRebuild = ctx.requestRebuild, pendingKey = &ctx.pendingGestureKey,
                       pendingVerb = &ctx.pendingGestureVerb, specFor, path,
                       key = setting.gestureKey](const std::string& value) {
      pendingKey->clear();
      pendingVerb->clear();
      const bool needsArgument = value == kActionExecOption || (!value.empty() && specRequiresArgument(specFor(value)));
      if (needsArgument) {
        // Nothing to store yet: the value is completed by the argument field.
        *pendingKey = key;
        *pendingVerb = value;
        clearOverride(path);
      } else if (value.empty()) {
        clearOverride(path);
      } else {
        setOverride(path, value);
      }
      if (requestRebuild) {
        requestRebuild();
      }
    };

    const std::string argsSpec = execMode ? std::string{} : specFor(actionVerb);
    const bool wantsArgument = execMode || (!actionVerb.empty() && specTakesArguments(argsSpec));

    auto control = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
    control->addChild(makeSearchPicker(picker, title, path));
    if (wantsArgument) {
      control->addChild(
          ui::input({
              .value = argument,
              .placeholder = execMode ? i18n::tr("settings.widgets.actions.command-placeholder") : argsSpec,
              .fontSize = Style::fontSizeBody * scale,
              .controlHeight = Style::controlHeight * scale,
              .horizontalPadding = Style::spaceSm * scale,
              .width = 220.0F * scale,
              .height = Style::controlHeight * scale,
              .onSubmit =
                  [setOverride = ctx.setOverride, clearOverride = ctx.clearOverride,
                   requestRebuild = ctx.requestRebuild, pendingKey = &ctx.pendingGestureKey,
                   pendingVerb = &ctx.pendingGestureVerb, specFor, path, verb = actionVerb,
                   defaultAction = setting.defaultAction, effectiveAction = effective,
                   execMode](const std::string& text) {
                    const std::string trimmed = StringUtils::trim(text);
                    std::string commandLine;
                    bool shouldClear = false;
                    if (trimmed.empty() && (execMode || specRequiresArgument(specFor(verb)))) {
                      shouldClear = true;
                    } else {
                      if (execMode) {
                        commandLine = std::string(noctalia::bar::kExecVerb) + " " + trimmed;
                      } else {
                        commandLine = verb;
                        if (!trimmed.empty()) {
                          commandLine += " " + trimmed;
                        }
                      }
                      shouldClear = commandLine == defaultAction;
                    }

                    // setOverride/clearOverride each queue a DeferredCall. Skip them
                    // when the value hasn't changed so rapid focus switches between
                    // argument text fields don't pile up deferred callbacks.
                    const bool changed = shouldClear ? !effectiveAction.empty() : commandLine != effectiveAction;
                    if (changed) {
                      if (shouldClear) {
                        clearOverride(path);
                      } else {
                        setOverride(path, commandLine);
                      }
                    }
                    pendingKey->clear();
                    pendingVerb->clear();
                    if (requestRebuild && changed) {
                      requestRebuild();
                    }
                  },
              .submitOnFocusLoss = true,
          })
      );
    }
    return control;
  }

  std::unique_ptr<Input> SettingsControlFactory::makeText(
      const std::string& value, const std::string& placeholder, const std::vector<std::string>& path, float width
  ) {
    auto& ctx = m_ctx;
    const float scale = m_scale;
    const float inputWidth = (width > 0.0F ? width : 190.0F) * scale;
    Input* inputPtr = nullptr;
    auto input = ui::input({
        .out = &inputPtr,
        .value = value,
        .placeholder = placeholder,
        .fontSize = Style::fontSizeBody * scale,
        .controlHeight = Style::controlHeight * scale,
        .horizontalPadding = Style::spaceSm * scale,
        .width = inputWidth,
        .height = Style::controlHeight * scale,
        .submitOnFocusLoss = true,
    });
    input->setOnChange([inputPtr](const std::string& /*text*/) { inputPtr->setInvalid(false); });
    input->setOnSubmit([configService = ctx.configService, setOverride = ctx.setOverride, path, effectiveValue = value,
                        inputPtr](const std::string& text) {
      if (configService != nullptr && !configService->validateOverride(path, ConfigOverrideValue{text})) {
        inputPtr->setInvalid(true);
        // Send the rejected mutation through the normal error-reporting path so the
        // editor sheet shows the shared schema diagnostic beside this control.
        setOverride(path, text);
        return;
      }
      inputPtr->setInvalid(false);
      if (text == effectiveValue) {
        return;
      }
      setOverride(path, text);
    });
    return input;
  }

  std::unique_ptr<Node>
  SettingsControlFactory::makePathBrowse(const TextSetting& setting, std::vector<std::string> path) {
    auto input = makeText(setting.value, setting.placeholder, path, setting.width > 0.0F ? setting.width : 280.0F);
    Input* inputPtr = input.get();
    const bool selectFolder = setting.browseMode == TextSettingBrowseMode::SelectFolder;
    const float scale = m_scale;
    auto& ctx = m_ctx;

    return ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceSm * scale}, std::move(input),
        ui::button({
            .glyph = selectFolder ? "folder" : "file-text",
            .glyphSize = Style::fontSizeBody * scale,
            .variant = ButtonVariant::Default,
            .minWidth = Style::controlHeight * scale,
            .minHeight = Style::controlHeight * scale,
            .paddingV = Style::spaceXs * scale,
            .paddingH = Style::spaceSm * scale,
            .radius = Style::scaledRadiusMd(scale),
            .onClick = [setOverride = ctx.setOverride, requestRebuild = ctx.requestRebuild, path = std::move(path),
                        inputPtr, selectFolder, extensions = setting.browseFileExtensions,
                        fallbackDirectory = setting.browseFallbackDirectory] {
              FileDialogOptions options;
              options.mode = selectFolder ? FileDialogMode::SelectFolder : FileDialogMode::Open;
              options.defaultViewMode = FileDialogViewMode::List;
              options.title = selectFolder ? i18n::tr("settings.controls.path-browse.folder-title")
                                           : i18n::tr("settings.controls.path-browse.file-title");
              if (!selectFolder) {
                options.extensions = extensions;
              }

              const std::string currentValue = inputPtr->value();
              if (!currentValue.empty()) {
                applyPathDialogStartValue(
                    options, currentValue, selectFolder ? PathBrowseKind::Folder : PathBrowseKind::File
                );
              } else {
                applyPathDialogStartValue(options, fallbackDirectory, PathBrowseKind::Folder);
              }

              (void)FileDialog::open(
                  std::move(options), [setOverride, requestRebuild, path](std::optional<std::filesystem::path> picked) {
                    if (!picked.has_value()) {
                      return;
                    }
                    setOverride(path, picked->string());
                    if (requestRebuild) {
                      requestRebuild();
                    }
                  }
              );
            },
        })
    );
  }

  std::unique_ptr<Input> SettingsControlFactory::makeOptionalNumber(
      const OptionalNumberSetting& setting, const std::vector<std::string>& path
  ) {
    auto& ctx = m_ctx;
    const float scale = m_scale;
    Input* inputPtr = nullptr;
    auto input = ui::input({
        .out = &inputPtr,
        .value = setting.value.has_value() ? std::format("{}", *setting.value) : "",
        .placeholder = setting.placeholder,
        .fontSize = Style::fontSizeBody * scale,
        .controlHeight = Style::controlHeight * scale,
        .horizontalPadding = Style::spaceSm * scale,
        .width = 190.0F * scale,
        .height = Style::controlHeight * scale,
    });
    input->setOnChange([inputPtr](const std::string& /*text*/) { inputPtr->setInvalid(false); });
    input->setOnSubmit([configService = ctx.configService, clearOverride = ctx.clearOverride,
                        setOverride = ctx.setOverride, path, inputPtr, minValue = setting.minValue,
                        maxValue = setting.maxValue](const std::string& text) {
      if (isBlankInput(text)) {
        inputPtr->setInvalid(false);
        if (configService != nullptr && configService->hasOverride(path)) {
          clearOverride(path);
        }
        return;
      }

      const auto parsed = parseDoubleInput(text);
      if (!parsed.has_value() || *parsed < minValue || *parsed > maxValue) {
        inputPtr->setInvalid(true);
        return;
      }

      inputPtr->setInvalid(false);
      setOverride(path, *parsed);
    });
    return input;
  }

  std::unique_ptr<Stepper>
  SettingsControlFactory::makeStepper(const StepperSetting& setting, const std::vector<std::string>& path) {
    auto& ctx = m_ctx;
    const float scale = m_scale;
    const int minValue = std::min(setting.minValue, setting.maxValue);
    const int maxValue = std::max(setting.minValue, setting.maxValue);
    const int currentValue = std::clamp(setting.value, minValue, maxValue);

    return ui::stepper({
        .minValue = minValue,
        .maxValue = maxValue,
        .step = setting.step,
        .value = currentValue,
        .scale = scale,
        .valueSuffix = setting.valueSuffix.empty() ? std::nullopt : std::optional<std::string>{setting.valueSuffix},
        .onValueCommitted = [setOverride = ctx.setOverride, path](int value) {
          setOverride(path, static_cast<std::int64_t>(value));
        },
    });
  }

  std::unique_ptr<Flex> SettingsControlFactory::makeOptionalStepper(
      const OptionalStepperSetting& setting, const std::vector<std::string>& path
  ) {
    auto& ctx = m_ctx;
    const float scale = m_scale;
    auto wrap = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});

    const int minValue = std::min(setting.minValue, setting.maxValue);
    const int maxValue = std::max(setting.minValue, setting.maxValue);
    const int currentValue = std::clamp(setting.value.value_or(setting.fallbackValue), minValue, maxValue);

    auto segmented = ui::segmented({
        .options =
            std::vector<ui::SegmentedOption>{
                {.label = setting.unsetLabel},
                {.label = setting.customLabel},
            },
        .selectedIndex = static_cast<std::size_t>(setting.value.has_value() ? 1 : 0),
        .scale = scale,
        .onChange = [setOverride = ctx.setOverride, path, currentValue](std::size_t index) {
          if (index == 0) {
            setOverride(path, std::string("auto"));
            return;
          }
          setOverride(path, static_cast<std::int64_t>(currentValue));
        },
    });

    auto stepper = ui::stepper({
        .minValue = minValue,
        .maxValue = maxValue,
        .step = setting.step,
        .value = currentValue,
        .enabled = setting.value.has_value(),
        .scale = scale,
        .onValueCommitted = [setOverride = ctx.setOverride, path](int value) {
          setOverride(path, static_cast<std::int64_t>(value));
        },
    });

    wrap->addChild(std::move(segmented));
    wrap->addChild(std::move(stepper));
    return wrap;
  }

  std::unique_ptr<Node> SettingsControlFactory::makeColorSpecPicker(
      const ColorSpecPickerSetting& setting, const std::vector<std::string>& path
  ) {
    auto& ctx = m_ctx;
    const float scale = m_scale;
    ColorSpecSelectOptions options{
        .roles = setting.roles,
        .selectedValue = setting.selectedValue,
        .allowNone = setting.allowNone,
        .allowCustomColor = setting.allowCustomColor,
        .noneLabel = setting.noneLabel,
        .fontSize = Style::fontSizeBody * scale,
        .controlHeight = Style::controlHeight * scale,
        .glyphSize = Style::fontSizeBody * scale,
        .width = 190.0F * scale,
    };
    return makeColorSpecSelect(
        std::move(options), [setOverride = ctx.setOverride, path](std::string value) { setOverride(path, value); },
        [configService = ctx.configService, clearOverride = ctx.clearOverride, requestRebuild = ctx.requestRebuild,
         path] {
          if (configService != nullptr && configService->hasOverride(path)) {
            clearOverride(path);
          } else {
            requestRebuild();
          }
        }
    );
  }

  std::unique_ptr<Flex> SettingsControlFactory::makeCollectionBlock(
      const SettingEntry& entry, bool overridden, bool reserveTitleHeight, bool titleMaxTwoLines, bool fillWidth,
      bool flexGrow, bool compactTitleDescription
  ) {
    const float scale = m_scale;
    ui::FlexProps blockProps{
        .align = FlexAlign::Stretch,
        .paddingV = Style::spaceXs * scale,
        .paddingH = 0.0F,
        .fillWidth = fillWidth ? std::optional<bool>{true} : std::nullopt,
        .flexGrow = flexGrow ? std::optional<float>{1.0F} : std::nullopt,
    };
    if (!compactTitleDescription) {
      blockProps.gap = Style::spaceXs * scale;
    }
    auto block = ui::column(blockProps);

    auto titleRow = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .minHeight = reserveTitleHeight ? std::optional<float>{Style::controlHeight * scale} : std::nullopt,
        .fillWidth = compactTitleDescription ? std::optional<bool>{true} : std::nullopt,
    });
    titleRow->addChild(
        ui::label({
            .text = entry.title,
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::Normal,
            .color = colorSpecFromRole(ColorRole::Label1),
            .maxLines = titleMaxTwoLines ? std::optional<int>{2} : std::nullopt,
        })
    );

    std::unique_ptr<Flex> overrideActions;
    if (overridden && !compactTitleDescription) {
      overrideActions = makeOverrideResetActions(entry.path);
    }

    ui::FlexProps copyProps{.align = FlexAlign::Start, .fillWidth = true};
    if (!compactTitleDescription) {
      copyProps.gap = Style::spaceXs * scale;
      copyProps.flexGrow = 1.0F;
    }
    auto copy = ui::column(copyProps);
    copy->addChild(std::move(titleRow));
    if (settingEntryShowsCaption(entry) && !entry.subtitle.empty()) {
      auto subtitle = makeSettingSubtitleLabel(entry.subtitle, scale);
      if (compactTitleDescription) {
        subtitle->setFlexGrow(1.0F);
      }
      copy->addChild(std::move(subtitle));
    }

    if (!compactTitleDescription && overrideActions != nullptr) {
      auto header = ui::row({.align = FlexAlign::Start, .gap = Style::spaceSm * scale, .fillWidth = true});
      header->addChild(std::move(copy));
      header->addChild(std::move(overrideActions));
      block->addChild(std::move(header));
    } else {
      block->addChild(std::move(copy));
    }
    return block;
  }

  void SettingsControlFactory::makeListBlock(Flex& section, const SettingEntry& entry, const ListSetting& list) {
    auto& ctx = m_ctx;
    const float scale = m_scale;
    const bool overridden = (ctx.configService != nullptr && ctx.configService->hasEffectiveOverride(entry.path));

    auto block = makeCollectionBlock(entry, overridden);

    auto listEditor = std::make_unique<ListEditor>();
    listEditor->setScale(scale);
    listEditor->setAddPlaceholder(i18n::tr("settings.controls.list.add-entry-placeholder"));
    std::vector<ListEditorOption> suggestedOptions;
    suggestedOptions.reserve(list.suggestedOptions.size());
    for (const auto& opt : list.suggestedOptions) {
      suggestedOptions.push_back(ListEditorOption{.value = opt.value, .label = opt.label});
    }
    listEditor->setSuggestedOptions(std::move(suggestedOptions));
    listEditor->setItems(list.items);
    listEditor->setOnAddRequested([setOverride = ctx.setOverride, items = list.items,
                                   path = entry.path](std::string value) mutable {
      if (value.empty()) {
        return;
      }
      items.push_back(std::move(value));
      setOverride(path, items);
    });
    listEditor->setOnRemoveRequested([setOverride = ctx.setOverride, items = list.items,
                                      path = entry.path](std::size_t index) mutable {
      if (index >= items.size()) {
        return;
      }
      items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
      setOverride(path, items);
    });
    listEditor->setOnMoveRequested([setOverride = ctx.setOverride, items = list.items,
                                    path = entry.path](std::size_t from, std::size_t to) mutable {
      if (from >= items.size() || to >= items.size() || from == to) {
        return;
      }
      std::swap(items[from], items[to]);
      setOverride(path, items);
    });
    block->addChild(std::move(listEditor));

    section.addChild(std::move(block));
  }

  void
  SettingsControlFactory::makeStringMapBlock(Flex& section, const SettingEntry& entry, const StringMapSetting& map) {
    auto& ctx = m_ctx;
    const float scale = m_scale;
    const bool overridden = (ctx.configService != nullptr && ctx.configService->hasEffectiveOverride(entry.path));

    auto block = makeCollectionBlock(entry, overridden);

    const auto setAndCommit = [setOverride = ctx.setOverride, clearOverride = ctx.clearOverride,
                               requestRebuild = ctx.requestRebuild,
                               path = entry.path](const std::string& key, const std::string& value) {
      auto entryPath = path;
      entryPath.push_back(key);
      if (value.empty()) {
        clearOverride(entryPath);
      } else {
        setOverride(entryPath, value);
      }
      if (requestRebuild) {
        requestRebuild();
      }
    };

    const auto removeAndCommit = [clearOverride = ctx.clearOverride, requestRebuild = ctx.requestRebuild,
                                  path = entry.path](const std::string& key) {
      auto entryPath = path;
      entryPath.push_back(key);
      clearOverride(entryPath);
      if (requestRebuild) {
        requestRebuild();
      }
    };

    std::unordered_set<std::string> suggestedSet(map.suggestedKeys.begin(), map.suggestedKeys.end());
    std::vector<std::string> suggested = map.suggestedKeys;
    std::ranges::sort(suggested);

    std::vector<std::string> customKeys;
    for (const auto& [key, value] : map.entries) {
      (void)value;
      if (!suggestedSet.contains(key)) {
        customKeys.push_back(key);
      }
    }
    std::ranges::sort(customKeys);

    const auto addSuggestedRow = [&](const std::string& key) {
      const auto valueIt = map.entries.find(key);
      const std::string value = valueIt != map.entries.end() ? valueIt->second : std::string{};
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      row->addChild(
          ui::label({
              .text = key,
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::Label1),
              .flexGrow = 1.0F,
          })
      );
      row->addChild(
          ui::label({
              .text = "->",
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::Label2),
          })
      );
      row->addChild(
          ui::input({
              .value = value,
              .placeholder = map.valuePlaceholder,
              .fontSize = Style::fontSizeBody * scale,
              .controlHeight = Style::controlHeight * scale,
              .horizontalPadding = Style::spaceSm * scale,
              .height = Style::controlHeight * scale,
              .flexGrow = 1.0F,
              .onSubmit = [setAndCommit, key](const std::string& newValue) { setAndCommit(key, newValue); },
              .submitOnFocusLoss = true,
          })
      );
      row->addChild(
          ui::button({
              // nullopt, not "": an empty glyph name resolves to the missing-glyph skull.
              .glyph = value.empty() ? std::nullopt : std::optional<std::string>{"close"},
              .fontSize = Style::fontSizeCaption * scale,
              .glyphSize = Style::fontSizeCaption * scale,
              .variant = ButtonVariant::Ghost,
              .minWidth = Style::controlHeight * scale,
              .minHeight = Style::controlHeight * scale,
              .onClick = [removeAndCommit, key, value] {
                if (!value.empty()) {
                  removeAndCommit(key);
                }
              },
          })
      );
      block->addChild(std::move(row));
    };

    const auto addCustomRow = [&](const std::string& key) {
      const std::string value = map.entries.at(key);
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      row->addChild(
          ui::input({
              .value = key,
              .placeholder = map.keyPlaceholder,
              .fontSize = Style::fontSizeBody * scale,
              .controlHeight = Style::controlHeight * scale,
              .horizontalPadding = Style::spaceSm * scale,
              .height = Style::controlHeight * scale,
              .flexGrow = 1.0F,
              .onSubmit =
                  [removeAndCommit, setAndCommit, value, oldKey = key](const std::string& newKey) {
                    if (newKey.empty() || newKey == oldKey) {
                      return;
                    }
                    removeAndCommit(oldKey);
                    setAndCommit(newKey, value);
                  },
              .submitOnFocusLoss = true,
          })
      );
      row->addChild(
          ui::label({
              .text = "->",
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::Label2),
          })
      );
      row->addChild(
          ui::input({
              .value = value,
              .placeholder = map.valuePlaceholder,
              .fontSize = Style::fontSizeBody * scale,
              .controlHeight = Style::controlHeight * scale,
              .horizontalPadding = Style::spaceSm * scale,
              .height = Style::controlHeight * scale,
              .flexGrow = 1.0F,
              .onSubmit = [setAndCommit, key](const std::string& newValue) { setAndCommit(key, newValue); },
              .submitOnFocusLoss = true,
          })
      );
      row->addChild(
          ui::button({
              .glyph = "close",
              .fontSize = Style::fontSizeCaption * scale,
              .glyphSize = Style::fontSizeCaption * scale,
              .variant = ButtonVariant::Ghost,
              .minWidth = Style::controlHeight * scale,
              .minHeight = Style::controlHeight * scale,
              .onClick = [removeAndCommit, key] { removeAndCommit(key); },
          })
      );
      block->addChild(std::move(row));
    };

    for (const auto& key : suggested) {
      addSuggestedRow(key);
    }
    for (const auto& key : customKeys) {
      addCustomRow(key);
    }

    auto addRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
    Input* keyInput = nullptr;
    Input* valueInput = nullptr;
    addRow->addChild(
        ui::input({
            .out = &keyInput,
            .placeholder = map.keyPlaceholder,
            .fontSize = Style::fontSizeBody * scale,
            .controlHeight = Style::controlHeight * scale,
            .horizontalPadding = Style::spaceSm * scale,
            .height = Style::controlHeight * scale,
            .flexGrow = 1.0F,
        })
    );
    addRow->addChild(
        ui::label({
            .text = "->",
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::Label2),
        })
    );
    addRow->addChild(
        ui::input({
            .out = &valueInput,
            .placeholder = map.valuePlaceholder,
            .fontSize = Style::fontSizeBody * scale,
            .controlHeight = Style::controlHeight * scale,
            .horizontalPadding = Style::spaceSm * scale,
            .height = Style::controlHeight * scale,
            .flexGrow = 1.0F,
        })
    );
    addRow->addChild(
        ui::button({
            .glyph = "add",
            .fontSize = Style::fontSizeCaption * scale,
            .glyphSize = Style::fontSizeCaption * scale,
            .variant = ButtonVariant::Ghost,
            .minWidth = Style::controlHeight * scale,
            .minHeight = Style::controlHeight * scale,
            .onClick = [keyInput, valueInput, setAndCommit] {
              if (keyInput == nullptr || valueInput == nullptr) {
                return;
              }
              const std::string key = keyInput->value();
              const std::string value = valueInput->value();
              if (!key.empty()) {
                setAndCommit(key, value);
              }
            },
        })
    );
    block->addChild(std::move(addRow));

    section.addChild(std::move(block));
  }

} // namespace settings
