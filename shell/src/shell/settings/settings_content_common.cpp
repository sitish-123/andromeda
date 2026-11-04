#include "shell/settings/settings_content_common.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "i18n/i18n.h"
#include "shell/settings/settings_content.h"
#include "shell/settings/settings_selection_style.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/glyph.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "compositors/settings_control.h"

#include <array>
#include <vector>
#include <cmath>
#include <cstdint>
#include <format>
#include <unordered_set>
#include <utility>

namespace settings {

  bool isMonitorOverrideSettingPath(const std::vector<std::string>& path) {
    return path.size() >= 5 && path[0] == "bar" && path[2] == "monitor";
  }

  bool monitorOverrideHasExplicitValue(const Config& cfg, const std::vector<std::string>& path) {
    if (!isMonitorOverrideSettingPath(path)) {
      return false;
    }

    const auto* bar = findBar(cfg, path[1]);
    if (bar == nullptr) {
      return false;
    }

    const auto* override = findMonitorOverride(*bar, path[3]);
    if (override == nullptr) {
      return false;
    }

    const std::string_view key = path.back();
    if (key == "enabled") {
      return override->enabled.has_value();
    }
    if (key == "auto_hide") {
      return override->autoHide.has_value();
    }
    if (key == "auto_hide_delay_ms") {
      return override->autoHideDelayMs.has_value();
    }
    if (key == "smart_auto_hide") {
      return override->smartAutoHide.has_value();
    }
    if (key == "show_on_workspace_switch") {
      return override->showOnWorkspaceSwitch.has_value();
    }
    if (key == "reserve_space") {
      return override->reserveSpace.has_value();
    }
    if (key == "thickness") {
      return override->thickness.has_value();
    }
    if (key == "scale") {
      return override->scale.has_value();
    }
    if (key == "margin_ends") {
      return override->marginEnds.has_value();
    }
    if (key == "margin_edge") {
      return override->marginEdge.has_value();
    }
    if (key == "margin_opposite_edge") {
      return override->marginOppositeEdge.has_value();
    }
    if (key == "padding") {
      return override->padding.has_value();
    }
    if (key == "radius") {
      return override->radius.has_value();
    }
    if (key == "radius_top_left") {
      return override->radiusTopLeft.has_value();
    }
    if (key == "radius_top_right") {
      return override->radiusTopRight.has_value();
    }
    if (key == "radius_bottom_left") {
      return override->radiusBottomLeft.has_value();
    }
    if (key == "radius_bottom_right") {
      return override->radiusBottomRight.has_value();
    }
    if (key == "concave_edge_corners") {
      return override->concaveEdgeCorners.has_value();
    }
    if (key == "background_opacity") {
      return override->backgroundOpacity.has_value();
    }
    if (key == "compositor_blur") {
      return override->compositorBlur.has_value();
    }
    if (key == "border") {
      return override->border.has_value();
    }
    if (key == "border_width") {
      return override->borderWidth.has_value();
    }
    if (key == "shadow") {
      return override->shadow.has_value();
    }
    if (key == "panel_overlap") {
      return override->panelOverlap.has_value();
    }
    if (key == "widget_spacing") {
      return override->widgetSpacing.has_value();
    }
    if (key == "capsule") {
      return override->widgetCapsuleDefault.has_value();
    }
    if (key == "capsule_fill") {
      return override->widgetCapsuleFill.has_value();
    }
    if (key == "capsule_border") {
      return override->widgetCapsuleBorderSpecified;
    }
    if (key == "capsule_border_width") {
      return override->widgetCapsuleBorderWidth.has_value();
    }
    if (key == "capsule_foreground") {
      return override->widgetCapsuleForeground.has_value();
    }
    if (key == "color") {
      return override->widgetColor.has_value();
    }
    if (key == "icon_color") {
      return override->widgetIconColor.has_value();
    }
    if (key == "capsule_padding") {
      return override->widgetCapsulePadding.has_value();
    }
    if (key == "capsule_radius") {
      return override->widgetCapsuleRadius.has_value();
    }
    if (key == "capsule_opacity") {
      return override->widgetCapsuleOpacity.has_value();
    }
    if (key == "start") {
      return override->startWidgets.has_value();
    }
    if (key == "center") {
      return override->centerWidgets.has_value();
    }
    if (key == "end") {
      return override->endWidgets.has_value();
    }
    if (path.size() >= 6 && path[4] == "dead_zone" && path[5] == "click_through") {
      return override->deadZone.clickThrough.has_value();
    }
    if (path.size() >= 6 && path[4] == "dead_zone") {
      return override->deadZone.actions.has_value();
    }
    return false;
  }

  bool settingEntryHasEffectiveOverride(
      const SettingEntry& entry, const ConfigService& configService, const SettingsControl* compositor
  ) {
    if (const auto key = compositorSettingKey(entry.path)) {
      return compositor != nullptr && compositor->customized(*key);
    }
    if (configService.hasEffectiveOverride(entry.path)) {
      return true;
    }
    if (const auto* range = std::get_if<RangeSliderSetting>(&entry.control)) {
      return configService.hasEffectiveOverride(range->highPath);
    }
    if (const auto* select = std::get_if<SelectSetting>(&entry.control)) {
      return !select->linkedPath.empty() && configService.hasEffectiveOverride(select->linkedPath);
    }
    return false;
  }

  std::unique_ptr<Label>
  makeLabel(std::string_view text, float fontSize, const ColorSpec& color, FontWeight fontWeight) {
    return ui::label({
        .text = std::string(text),
        .fontSize = fontSize,
        .fontWeight = fontWeight,
        .color = color,
    });
  }

  Flex* addSettingsCard(Flex& parent, std::string_view title, float scale) {
    return addSettingsGroupCard(parent, title, scale);
  }

  Flex* addSettingsGroupCard(Flex& parent, std::string_view caption, float scale) {
    Flex* bodyRaw = nullptr;
    auto group = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale});
    if (!caption.empty()) {
      auto captionRow = ui::column({.align = FlexAlign::Stretch});
      captionRow->setPadding(0.0F, Style::spaceLg * scale);
      captionRow->addChild(
          makeLabel(caption, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::Label2), FontWeight::Bold)
      );
      group->addChild(std::move(captionRow));
    }
    group->addChild(
        ui::column({
            .out = &bodyRaw,
            .align = FlexAlign::Stretch,
            .gap = 0.0F,
            .configure = [scale](Flex& container) {
              container.setFill(frameColors().cardFill);
              container.setBorder(frameColors().cardBorder, metrics::kCardBorderWidth * scale);
              container.setRadius(Style::scaledRadiusLg(scale));
              container.setPadding(Style::spaceXs * scale, Style::spaceLg * scale);
            },
        })
    );
    parent.addChild(std::move(group));
    return bodyRaw;
  }

  void applyWallpaperIllustration(Flex& flex) {
    const bool light = isResolvedLightTheme();
    const Color a = rgbHex(light ? 0xF6C9A8 : 0x24161C);
    const Color b = rgbHex(light ? 0x9FB6FF : 0x121A33);
    const Color c = rgbHex(light ? 0x8FE6D2 : 0x0C2A28);
    flex.setFillGradient(
        GradientDirection::Horizontal, {GradientStop{0.0F, a}, GradientStop{0.6F, b}, GradientStop{0.6F, b}, GradientStop{1.0F, c}}
    );
  }

  std::unique_ptr<Flex> makeDockPreview(const DockConfig& dock, float scale) {
    constexpr float kPreviewHeight = 120.0F;
    constexpr float kPreviewInset = 12.0F;
    constexpr float kShrink = 0.8F; // preview icons are 0.8 of the real size
    constexpr std::array<std::uint32_t, 5> kApps{0x3D6CF0, 0xF0603D, 0x5147E5, 0x2B9E63, 0xE8A23C};
    const bool vertical = dock.position == DockEdge::Left || dock.position == DockEdge::Right;
    const bool atEnd = dock.position == DockEdge::Bottom || dock.position == DockEdge::Right;
    const float icon = static_cast<float>(dock.iconSize) * kShrink;
    auto capsule = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceXs * scale,
        .padding = Style::spaceXs * scale,
        .fill = colorSpecFromRole(ColorRole::SolidThin),
        .radius = Style::scaledRadiusXl(scale),
    });
    if (vertical) {
      capsule->setDirection(FlexDirection::Vertical);
    }
    for (std::size_t i = 0; i < kApps.size(); ++i) {
      const float size = dock.magnification && i == 2 ? icon * dock.magnificationScale : icon;
      capsule->addChild(ui::box({
          .fill = fixedColorSpec(rgbHex(kApps[i])),
          .radius = Style::spaceMd * scale,
          .width = size * scale,
          .height = size * scale,
      }));
    }
    auto plane = ui::column({
        .align = vertical ? (atEnd ? FlexAlign::End : FlexAlign::Start) : FlexAlign::Center,
        .justify = vertical ? FlexJustify::Center : (atEnd ? FlexJustify::End : FlexJustify::Start),
        .padding = kPreviewInset * scale,
        .radius = Style::scaledRadiusMd(scale),
        .minHeight = kPreviewHeight * scale,
        .fillWidth = true,
    });
    applyWallpaperIllustration(*plane);
    plane->addChild(std::move(capsule));
    return plane;
  }

  void addSettingsRowSeparators(Flex& body, float scale) {
    // Only between shown rows. The card's own background rect is a child too; a separator after it draws a line
    // on the card's top edge.
    std::vector<std::size_t> shown;
    for (std::size_t index = 0; index < body.children().size(); ++index) {
      const auto& child = body.children()[index];
      if (child->visible() && child->participatesInLayout()) {
        shown.push_back(index);
      }
    }
    for (std::size_t k = shown.size(); k > 1; --k) {
      auto separator =
          ui::separator({.thickness = metrics::kRowSeparatorThickness * scale, .spacing = 0.0F, .gradientEdges = false});
      separator->setEndOutset(Style::spaceLg * scale);
      body.insertChildAt(shown[k - 1], std::move(separator));
    }
  }

  std::unique_ptr<Button>
  makeSettingsLinkRow(std::string_view label, std::string_view trailing, float scale, std::function<void()> onClick) {
    auto row = ui::button({
        .text = std::string(label),
        .fontSize = Style::fontSizeBody * scale,
        .contentAlign = ButtonContentAlign::Start,
        .variant = ButtonVariant::Tab,
        .minHeight = Style::controlHeight * scale,
        .paddingV = Style::spaceXs * scale,
        .paddingH = 0.0F,
        .gap = Style::spaceSm * scale,
        .radius = Style::scaledRadiusMd(scale),
        .onClick = std::move(onClick),
    });
    row->addChild(ui::spacer());
    if (!trailing.empty()) {
      row->addChild(makeLabel(trailing, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::Label2)));
    }
    row->addChild(
        ui::glyph({
            .glyph = "chevron-right",
            .glyphSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::Label3),
        })
    );
    return row;
  }

  void updateSettingsStatusBanner(Flex& banner, Label& message, std::string_view text, bool error) {
    message.setText(text);
    message.setColor(colorSpecFromRole(error ? ColorRole::DangerText : ColorRole::AccentText));
    banner.setFill(colorSpecFromRole(error ? ColorRole::Danger : ColorRole::Accent, 0.14F));
    banner.setBorder(colorSpecFromRole(error ? ColorRole::Danger : ColorRole::Accent, 0.45F), Style::borderWidth);
    banner.setVisible(!text.empty());
  }

  std::unique_ptr<Flex> makeSettingsStatusBanner(SettingsStatusBannerProps props) {
    const float scale = props.scale;
    const bool error = props.error;
    auto banner = ui::row({
        .out = props.out,
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .visible = !props.message.empty(),
        .configure = [scale, error](Flex& row) {
          row.setPadding(Style::spaceXs * scale, Style::spaceSm * scale);
          row.setRadius(Style::scaledRadiusMd(scale));
          row.setFill(colorSpecFromRole(error ? ColorRole::Danger : ColorRole::Accent, 0.14F));
          row.setBorder(colorSpecFromRole(error ? ColorRole::Danger : ColorRole::Accent, 0.45F), Style::borderWidth);
        },
    });
    auto message = ui::label({
        .out = props.messageOut,
        .text = std::move(props.message),
        .fontSize = Style::fontSizeCaption * scale,
        .fontWeight = FontWeight::Bold,
        .color = colorSpecFromRole(error ? ColorRole::DangerText : ColorRole::AccentText),
        .maxLines = props.scrollable ? 0 : 3,
        .flexGrow = props.scrollable ? 0.0F : 1.0F,
    });
    if (props.scrollable) {
      auto messageColumn = ui::column({
          .align = FlexAlign::Stretch,
          .maxHeight = 160.0F * scale,
          .flexGrow = 1.0F,
      });
      auto messageScroll = ui::scrollView({
          .contentScale = scale,
          .scrollbarVisible = true,
          .viewportPaddingH = 0.0F,
          .viewportPaddingV = 0.0F,
          .flexGrow = 1.0F,
          .configure = [](ScrollView& scroll) {
            scroll.clearFill();
            scroll.clearBorder();
            scroll.content()->setAlign(FlexAlign::Stretch);
          },
      });
      messageScroll->content()->addChild(std::move(message));
      messageColumn->addChild(std::move(messageScroll));
      banner->addChild(std::move(messageColumn));
    } else {
      banner->addChild(std::move(message));
    }
    if (props.onDismiss) {
      banner->addChild(
          ui::button({
              .glyph = "close",
              .glyphSize = Style::fontSizeCaption * scale,
              .variant = ButtonVariant::Ghost,
              .minWidth = Style::controlHeight * scale,
              .minHeight = Style::controlHeight * scale,
              .padding = Style::spaceXs * scale,
              .radius = Style::scaledRadiusSm(scale),
              .onClick = std::move(props.onDismiss),
          })
      );
    }
    return banner;
  }

  bool settingsSectionNeedsOfflineModeNotice(SettingsSection section) {
    switch (section) {
    case SettingsSection::Appearance:
    case SettingsSection::Launcher:
    case SettingsSection::Security:
    case SettingsSection::Location:
    case SettingsSection::Calendar:
      return true;
    case SettingsSection::Services:
    case SettingsSection::Wallpaper:
    case SettingsSection::Desktop:
    case SettingsSection::Dock:
    case SettingsSection::Panels:
    case SettingsSection::ControlCenter:
    case SettingsSection::Notifications:
    case SettingsSection::TextScale:
    case SettingsSection::Motion:
    case SettingsSection::Screenshot:
    case SettingsSection::Shell:
    case SettingsSection::Keybinds:
    case SettingsSection::Shortcuts:
    case SettingsSection::System:
    case SettingsSection::Power:
    case SettingsSection::Hooks:
    case SettingsSection::Niri:
    case SettingsSection::Windows:
    case SettingsSection::WindowStyle:
    case SettingsSection::Layout:
    case SettingsSection::Overview:
    case SettingsSection::Bar:
    case SettingsSection::Plugins:
    case SettingsSection::Displays:
    case SettingsSection::Input:
    case SettingsSection::DateTime:
    case SettingsSection::Language:
    case SettingsSection::DefaultApps:
    case SettingsSection::Devices:
      return false;
    }
    return false;
  }

  std::string offlineModeNoticeMessage(SettingsSection section) {
    switch (section) {
    case SettingsSection::Appearance:
      return i18n::tr("settings.window.offline-mode-notice.appearance");
    case SettingsSection::Launcher:
      return i18n::tr("settings.window.offline-mode-notice.launcher");
    case SettingsSection::Security:
      return i18n::tr("settings.window.offline-mode-notice.security");
    case SettingsSection::Calendar:
      return i18n::tr("settings.window.offline-mode-notice.calendar");
    case SettingsSection::Location:
      return i18n::tr("settings.window.offline-mode-notice.location");
    case SettingsSection::Services:
    case SettingsSection::Wallpaper:
    case SettingsSection::Desktop:
    case SettingsSection::Dock:
    case SettingsSection::Panels:
    case SettingsSection::ControlCenter:
    case SettingsSection::Notifications:
    case SettingsSection::TextScale:
    case SettingsSection::Motion:
    case SettingsSection::Screenshot:
    case SettingsSection::Shell:
    case SettingsSection::Keybinds:
    case SettingsSection::Shortcuts:
    case SettingsSection::System:
    case SettingsSection::Power:
    case SettingsSection::Hooks:
    case SettingsSection::Niri:
    case SettingsSection::Windows:
    case SettingsSection::WindowStyle:
    case SettingsSection::Layout:
    case SettingsSection::Overview:
    case SettingsSection::Bar:
    case SettingsSection::Plugins:
    case SettingsSection::Displays:
    case SettingsSection::Input:
    case SettingsSection::DateTime:
    case SettingsSection::Language:
    case SettingsSection::DefaultApps:
    case SettingsSection::Devices:
      return {};
    }
    return {};
  }

  std::unique_ptr<Flex> makeOfflineModeNotice(float scale, std::string message, bool showDisableHint) {
    if (showDisableHint) {
      message += ' ';
      message += i18n::tr("settings.window.offline-mode-notice.disable-hint");
    }
    auto panel = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * scale,
        .padding = Style::spaceSm * scale,
        .configure = [scale](Flex& column) {
          column.setRadius(Style::scaledRadiusMd(scale));
          column.setFill(colorSpecFromRole(ColorRole::Fill1));
          column.setBorder(colorSpecFromRole(ColorRole::Danger), Style::borderWidth);
        },
    });
    panel->addChild(
        ui::label({
            .text = i18n::tr("settings.window.offline-mode-notice.title"),
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::Label1),
        })
    );
    panel->addChild(
        ui::label({
            .text = std::move(message),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::Label1),
            .maxLines = 4,
        })
    );
    return panel;
  }

  std::unique_ptr<Label> makeSettingSubtitleLabel(std::string_view text, float scale) {
    return ui::label({
        .text = std::string(text),
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(ColorRole::Label2),
        .maxLines = kSettingDescriptionMaxLines,
    });
  }

  std::optional<std::size_t> optionIndex(const std::vector<SelectOption>& options, std::string_view value) {
    for (std::size_t i = 0; i < options.size(); ++i) {
      if (options[i].value == value) {
        return i;
      }
    }
    return std::nullopt;
  }

  std::string optionLabel(const std::vector<SelectOption>& options, std::string_view value) {
    for (const auto& opt : options) {
      if (opt.value == value) {
        return opt.label;
      }
    }
    return std::string(value);
  }

  std::vector<std::string> optionLabels(const std::vector<SelectOption>& options) {
    std::vector<std::string> labels;
    labels.reserve(options.size());
    for (const auto& opt : options) {
      labels.push_back(opt.label);
    }
    return labels;
  }

  std::vector<ColorSwatchPreview> optionSwatchPreviews(const std::vector<SelectOption>& options) {
    std::vector<ColorSwatchPreview> previews;
    previews.reserve(options.size());
    for (const auto& opt : options) {
      previews.push_back(opt.preview);
    }
    return previews;
  }

  bool isBlankInput(std::string_view text) { return StringUtils::trim(text).empty(); }

  std::string formatSliderValue(double value, bool integerValue) {
    if (integerValue) {
      return std::format("{}", static_cast<int>(std::llround(value)));
    }
    return StringUtils::formatFixedDotDecimal(value, 2);
  }

  std::optional<double> parseDoubleInput(std::string_view text) { return StringUtils::parseDotDecimal<double>(text); }

  std::vector<SelectOption> sessionActionKindOptions() {
    return {
        {"lock", i18n::tr("session.actions.lock"), {}},
        {"logout", i18n::tr("session.actions.logout"), {}},
        {"suspend", i18n::tr("session.actions.suspend"), {}},
        {"lock_and_suspend", i18n::tr("session.actions.lock-and-suspend"), {}},
        {"reboot", i18n::tr("session.actions.reboot"), {}},
        {"reboot_uefi", i18n::tr("session.actions.reboot-uefi"), {}},
        {"shutdown", i18n::tr("session.actions.shutdown"), {}},
        {"hibernate", i18n::tr("session.actions.hibernate"), {}},
        {"suspend_then_hibernate", i18n::tr("session.actions.suspend-then-hibernate"), {}},
        {"command", i18n::tr("session.actions.custom"), {}},
    };
  }

  std::string
  sessionActionRowSummary(const std::vector<SelectOption>& kindOptions, const SessionPanelActionConfig& row) {
    if (row.label.has_value() && !row.label->empty()) {
      return *row.label;
    }
    return optionLabel(kindOptions, row.action);
  }

  std::string sessionActionDisplayTitle(const SessionPanelActionConfig& row) {
    return sessionActionRowSummary(sessionActionKindOptions(), row);
  }

  std::string sanitizedIdleBehaviorName(std::string_view text) {
    std::string out = StringUtils::trim(text);
    for (char& ch : out) {
      if (ch == '.' || ch == '[' || ch == ']') {
        ch = '-';
      }
    }
    return out;
  }

  std::string uniqueIdleBehaviorName(
      std::string base, const std::vector<IdleBehaviorConfig>& rows, std::optional<std::size_t> ignoreIndex
  ) {
    base = sanitizedIdleBehaviorName(base);
    if (base.empty()) {
      base = "idle-behavior";
    }

    std::unordered_set<std::string> names;
    for (std::size_t i = 0; i < rows.size(); ++i) {
      if (ignoreIndex.has_value() && i == *ignoreIndex) {
        continue;
      }
      if (!rows[i].name.empty()) {
        names.insert(rows[i].name);
      }
    }

    if (!names.contains(base)) {
      return base;
    }
    for (int suffix = 2; suffix < 10000; ++suffix) {
      std::string candidate = std::format("{}-{}", base, suffix);
      if (!names.contains(candidate)) {
        return candidate;
      }
    }
    return base;
  }

  void normalizeIdleBehaviorNames(std::vector<IdleBehaviorConfig>& rows) {
    std::vector<IdleBehaviorConfig> normalized;
    normalized.reserve(rows.size());
    for (auto& row : rows) {
      row.name = uniqueIdleBehaviorName(row.name, normalized);
      normalized.push_back(row);
    }
    rows = std::move(normalized);
  }

  std::string idleBehaviorRowSummary(const IdleBehaviorConfig& row) {
    IdleBehaviorConfig norm = row;
    normalizeIdleBehaviorAction(norm);

    const auto displayName = [&] -> std::string {
      if (norm.action == "lock") {
        return i18n::tr("settings.idle.behavior.kind.lock");
      }
      if (norm.action == "dim") {
        return i18n::tr("settings.idle.behavior.kind.dim");
      }
      if (norm.action == "screen_off") {
        return i18n::tr("settings.idle.behavior.kind.screen-off");
      }
      if (norm.action == "suspend") {
        return i18n::tr("settings.idle.behavior.kind.suspend");
      }
      if (norm.action == "lock_and_suspend") {
        return i18n::tr("settings.idle.behavior.kind.lock-and-suspend");
      }
      if (norm.action == "hibernate") {
        return i18n::tr("settings.idle.behavior.kind.hibernate");
      }
      if (row.name.empty()) {
        return i18n::tr("settings.idle.behavior.unnamed");
      }
      return row.name;
    };

    const std::string name = displayName();
    if (name.empty()) {
      return i18n::tr("settings.idle.behavior.unnamed");
    }
    if (row.timeoutSeconds <= 0) {
      return i18n::tr("settings.idle.behavior.summary-disabled-timeout", "name", name);
    }
    return i18n::tr(
        "settings.idle.behavior.summary", "name", name, "seconds", StringUtils::formatDotDecimal(row.timeoutSeconds)
    );
  }

  std::string notificationFilterRowSummary(const NotificationFilterConfig& filter) {
    if (!filter.enabled) {
      if (!filter.match.empty()) {
        return i18n::tr("settings.notifications.filter.summary-disabled", "match", filter.match);
      }
      return i18n::tr("settings.notifications.filter.unnamed");
    }

    const std::string matchLabel =
        filter.match.empty() ? i18n::tr("settings.notifications.filter.unnamed") : filter.match;
    std::vector<std::string> parts;
    if (filter.showToast) {
      parts.emplace_back(i18n::tr("settings.notifications.filter.flag.toast"));
    }
    if (filter.saveHistory) {
      parts.emplace_back(i18n::tr("settings.notifications.filter.flag.history"));
    }
    if (filter.playSound) {
      parts.emplace_back(i18n::tr("settings.notifications.filter.flag.sound"));
    }
    if (filter.bypassDnd) {
      parts.emplace_back(i18n::tr("settings.notifications.filter.flag.dnd-bypass"));
    }
    if (filter.allowPermanent) {
      parts.emplace_back(i18n::tr("settings.notifications.filter.flag.permanent"));
    }
    if (!filter.allowedUrgencies.empty()) {
      std::vector<std::string> urgencyLabels;
      urgencyLabels.reserve(filter.allowedUrgencies.size());
      for (const auto& urgency : filter.allowedUrgencies) {
        urgencyLabels.emplace_back(i18n::tr("settings.options.notification-urgency." + urgency));
      }
      parts.emplace_back(StringUtils::join(urgencyLabels, ", "));
    }
    if (parts.empty()) {
      return i18n::tr("settings.notifications.filter.summary-blocked", "match", matchLabel);
    }
    return i18n::tr(
        "settings.notifications.filter.summary", "match", matchLabel, "flags", StringUtils::join(parts, ", ")
    );
  }

} // namespace settings
