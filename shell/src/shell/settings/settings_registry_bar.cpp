#include "shell/settings/settings_registry_internal.h"
#include "config/app_icon_settings.h"

#include "config/config_types.h"
#include "config/schema/config_schema.h"
#include "config/schema/ranges.h"
#include "core/files/directory_scanner.h"
#include "core/log.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "launcher/panel_catalog.h"
#include "pipewire/sound_player.h"
#include "shell/bar/widget_gesture.h"
#include "shell/bar/widget_gesture_defaults.h"
#include "shell/control_center/control_center_panel.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/settings/color_spec_picker.h"
#include "shell/settings/font_weight_catalog.h"
#include "shell/wallpaper/wallpaper_paths.h"
#include "system/sysmon_threshold_profile.h"
#include "theme/builtin_palettes.h"
#include "theme/builtin_templates.h"
#include "ui/app_icon_colorization.h"
#include "util/file_utils.h"
#include "util/string_utils.h"
#include "compositors/settings_control.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace settings {
  using namespace registry_detail;

  void registry_detail::add_osd_settings(RegistryContext& ctx) {
    using i18n::tr;
    const auto& cfg = ctx.cfg;
    const auto& env = ctx.env;
    auto& entries = ctx.entries;

    auto osdRows = generateSchemaEntries(noctalia::config::schema::osdSchema(), cfg.osd, {"osd"});
    appendRow(entries, osdRows, "enabled");
    const std::size_t osdGatedStart = entries.size();
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "osd", tr("settings.schema.shell.osd-orientation.label"),
        tr("settings.schema.shell.osd-orientation.description"), {"osd", "orientation"},
        asSegmented(plainSelect(
            {{"horizontal", "settings.options.orientation.horizontal"},
             {"vertical", "settings.options.orientation.vertical"}},
            cfg.osd.orientation
        )),
        "hud overlay volume brightness vertical"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "osd", tr("settings.schema.shell.osd-position.label"),
        tr("settings.schema.shell.osd-position.description"), {"osd", "position"},
        plainSelect(
            {{"top_right", "settings.options.screen-position.top-right"},
             {"top_left", "settings.options.screen-position.top-left"},
             {"top_center", "settings.options.screen-position.top-center"},
             {"bottom_right", "settings.options.screen-position.bottom-right"},
             {"bottom_left", "settings.options.screen-position.bottom-left"},
             {"bottom_center", "settings.options.screen-position.bottom-center"},
             {"center_right", "settings.options.screen-position.center-right"},
             {"center_left", "settings.options.screen-position.center-left"}},
            cfg.osd.position
        ),
        "hud overlay volume brightness horizontal text"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "osd", tr("settings.schema.shell.osd-position-vertical.label"),
        tr("settings.schema.shell.osd-position-vertical.description"), {"osd", "position_vertical"},
        plainSelect(
            {{"top_right", "settings.options.screen-position.top-right"},
             {"top_left", "settings.options.screen-position.top-left"},
             {"top_center", "settings.options.screen-position.top-center"},
             {"bottom_right", "settings.options.screen-position.bottom-right"},
             {"bottom_left", "settings.options.screen-position.bottom-left"},
             {"bottom_center", "settings.options.screen-position.bottom-center"},
             {"center_right", "settings.options.screen-position.center-right"},
             {"center_left", "settings.options.screen-position.center-left"}},
            cfg.osd.positionVertical
        ),
        "hud overlay volume brightness vertical slider"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "osd", tr("settings.schema.shell.osd-hide-delay.label"),
        tr("settings.schema.shell.osd-hide-delay.description"), {"osd", "hide_delay_ms"},
        StepperSetting{
            .value = static_cast<int>(cfg.osd.hideDelayMs),
            .minValue = static_cast<int>(noctalia::config::schema::kOsdHideDelayMsRange.min.value()),
            .maxValue = static_cast<int>(noctalia::config::schema::kOsdHideDelayMsRange.max.value()),
            .step = static_cast<int>(noctalia::config::schema::kOsdHideDelayMsRange.step.value()),
            .valueSuffix = "ms",
        },
        "hud overlay popup timeout duration visible"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "osd", tr("settings.schema.shell.osd-scale.label"),
        tr("settings.schema.shell.osd-scale.description"), {"osd", "scale"},
        sliderFor(cfg.osd.scale, noctalia::config::schema::kScaleRange, false),
        "hud overlay volume brightness size scale multiplier"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "osd", tr("settings.schema.shell.osd-offset-x.label"),
        tr("settings.schema.shell.osd-offset-x.description"), {"osd", "offset_x"},
        StepperSetting{.value = cfg.osd.offsetX, .minValue = 0, .maxValue = 200, .step = 1, .valueSuffix = "px"},
        "hud overlay horizontal margin"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "osd", tr("settings.schema.shell.osd-offset-y.label"),
        tr("settings.schema.shell.osd-offset-y.description"), {"osd", "offset_y"},
        StepperSetting{.value = cfg.osd.offsetY, .minValue = 0, .maxValue = 200, .step = 1, .valueSuffix = "px"},
        "hud overlay vertical margin"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "osd", tr("settings.schema.shell.osd-background-opacity.label"),
        tr("settings.schema.shell.osd-background-opacity.description"), {"osd", "background_opacity"},
        sliderFor(cfg.osd.backgroundOpacity, noctalia::config::schema::kUnitRange, false),
        "hud overlay popup alpha transparency translucent opacity"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "osd", tr("settings.schema.shell.osd-border.label"),
        tr("settings.schema.shell.osd-border.description"), {"osd", "border"}, ToggleSetting{cfg.osd.border},
        "outline border"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "osd", tr("settings.schema.shell.osd-border-color.label"),
        tr("settings.schema.shell.osd-border-color.description"), {"osd", "border_color"},
        colorSpecPicker(cfg.osd.borderColor), "outline border color theme accent"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "osd", tr("settings.schema.shell.osd-border-width.label"),
        tr("settings.schema.shell.osd-border-width.description"), {"osd", "border_width"},
        sliderFor(cfg.osd.borderWidth, noctalia::config::schema::kOsdBorderWidthRange, false),
        "outline border width thickness"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "osd", tr("settings.schema.shell.osd-follow-focused-output.label"),
        tr("settings.schema.shell.osd-follow-focused-output.description"), {"osd", "follow_focused_output"},
        ToggleSetting{cfg.osd.followFocusedOutput}, "monitor output display focused active hud overlay"
    ));
    {
      auto e = makeEntry(
          SettingsSection::Notifications, "osd", tr("settings.schema.shell.osd-monitors.label"),
          tr("settings.schema.shell.osd-monitors.description"), {"osd", "monitors"},
          ListSetting{.items = cfg.osd.monitors, .suggestedOptions = env.availableOutputs},
          "monitor output display screen hud overlay"
      );
      e.visibleWhen = [](const Config& c) { return !c.osd.followFocusedOutput; };
      entries.push_back(std::move(e));
    }
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "kinds", tr("settings.schema.shell.osd-kinds-volume.label"),
        tr("settings.schema.shell.osd-kinds-volume.description"), {"osd", "kinds", "volume"},
        ToggleSetting{cfg.osd.kinds.volume}, "hud overlay audio output input microphone"
    ));
    {
      const SettingVisibility volumeOn = [](const Config& c) { return c.osd.kinds.volume; };
      SettingEntry outputEntry = makeEntry(
          SettingsSection::Notifications, "kinds", tr("settings.schema.shell.osd-kinds-volume-output.label"),
          tr("settings.schema.shell.osd-kinds-volume-output.description"), {"osd", "kinds", "volume_output"},
          ToggleSetting{cfg.osd.kinds.volumeOutput}, "hud overlay audio speaker sink output"
      );
      outputEntry.advanced = true;
      outputEntry.visibleWhen = volumeOn;
      entries.push_back(std::move(outputEntry));
      SettingEntry inputEntry = makeEntry(
          SettingsSection::Notifications, "kinds", tr("settings.schema.shell.osd-kinds-volume-input.label"),
          tr("settings.schema.shell.osd-kinds-volume-input.description"), {"osd", "kinds", "volume_input"},
          ToggleSetting{cfg.osd.kinds.volumeInput}, "hud overlay audio microphone source input"
      );
      inputEntry.advanced = true;
      inputEntry.visibleWhen = volumeOn;
      entries.push_back(std::move(inputEntry));
    }
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "kinds", tr("settings.schema.shell.osd-kinds-brightness.label"),
        tr("settings.schema.shell.osd-kinds-brightness.description"), {"osd", "kinds", "brightness"},
        ToggleSetting{cfg.osd.kinds.brightness}, "hud overlay display backlight"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "kinds", tr("settings.schema.shell.osd-kinds-wifi.label"),
        tr("settings.schema.shell.osd-kinds-wifi.description"), {"osd", "kinds", "wifi"},
        ToggleSetting{cfg.osd.kinds.wifi}, "hud overlay wireless network"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "kinds", tr("settings.schema.shell.osd-kinds-bluetooth.label"),
        tr("settings.schema.shell.osd-kinds-bluetooth.description"), {"osd", "kinds", "bluetooth"},
        ToggleSetting{cfg.osd.kinds.bluetooth}, "hud overlay bt"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "kinds", tr("settings.schema.shell.osd-kinds-power-profile.label"),
        tr("settings.schema.shell.osd-kinds-power-profile.description"), {"osd", "kinds", "power_profile"},
        ToggleSetting{cfg.osd.kinds.powerProfile}, "hud overlay balanced performance power saver"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "kinds", tr("settings.schema.shell.osd-kinds-caffeine.label"),
        tr("settings.schema.shell.osd-kinds-caffeine.description"), {"osd", "kinds", "caffeine"},
        ToggleSetting{cfg.osd.kinds.caffeine}, "hud overlay idle inhibitor"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "kinds", tr("settings.schema.shell.osd-kinds-nightlight.label"),
        tr("settings.schema.shell.osd-kinds-nightlight.description"), {"osd", "kinds", "nightlight"},
        ToggleSetting{cfg.osd.kinds.nightlight}, "hud overlay night light gamma"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "kinds", tr("settings.schema.shell.osd-kinds-dnd.label"),
        tr("settings.schema.shell.osd-kinds-dnd.description"), {"osd", "kinds", "dnd"},
        ToggleSetting{cfg.osd.kinds.dnd}, "hud overlay do not disturb notifications"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "kinds", tr("settings.schema.shell.osd-kinds-lock-keys.label"),
        tr("settings.schema.shell.osd-kinds-lock-keys.description"), {"osd", "kinds", "lock_keys"},
        ToggleSetting{cfg.osd.kinds.lockKeys}, "hud overlay caps num scroll keyboard"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "kinds", tr("settings.schema.shell.osd-kinds-keyboard-layout.label"),
        tr("settings.schema.shell.osd-kinds-keyboard-layout.description"), {"osd", "kinds", "keyboard_layout"},
        ToggleSetting{cfg.osd.kinds.keyboardLayout}, "hud overlay xkb input language layout switch"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "kinds", tr("settings.schema.shell.osd-kinds-media.label"),
        tr("settings.schema.shell.osd-kinds-media.description"), {"osd", "kinds", "media"},
        ToggleSetting{cfg.osd.kinds.media}, "hud overlay mpris audio music"
    ));
    {
      SettingEntry mediaVolumeEntry = makeEntry(
          SettingsSection::Notifications, "kinds", tr("settings.schema.shell.osd-kinds-media-volume.label"),
          tr("settings.schema.shell.osd-kinds-media-volume.description"), {"osd", "kinds", "media_volume"},
          ToggleSetting{cfg.osd.kinds.mediaVolume}, "hud overlay mpris media player volume level"
      );
      mediaVolumeEntry.visibleWhen = [](const Config& c) { return c.osd.kinds.media; };
      entries.push_back(std::move(mediaVolumeEntry));
    }
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "kinds", tr("settings.schema.shell.osd-kinds-privacy.label"),
        tr("settings.schema.shell.osd-kinds-privacy.description"), {"osd", "kinds", "privacy"},
        ToggleSetting{cfg.osd.kinds.privacy}, "hud overlay microphone camera screen share recording"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "kinds", tr("settings.schema.shell.osd-kinds-keyboard-backlight.label"),
        tr("settings.schema.shell.osd-kinds-keyboard-backlight.description"), {"osd", "kinds", "keyboard_backlight"},
        ToggleSetting{cfg.osd.kinds.keyboardBacklight}, "hud overlay keyboard backlight kbd"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "kinds", tr("settings.schema.shell.osd-kinds-custom.label"),
        tr("settings.schema.shell.osd-kinds-custom.description"), {"osd", "kinds", "custom"},
        ToggleSetting{cfg.osd.kinds.custom}, "hud overlay plugin custom script"
    ));
    // Gate every OSD entry after the master toggle on osd.enabled, preserving any per-entry visibility.
    for (std::size_t i = osdGatedStart; i < entries.size(); ++i) {
      SettingVisibility prev = std::move(entries[i].visibleWhen);
      entries[i].visibleWhen = prev
          ? SettingVisibility{[prev = std::move(prev)](const Config& c) { return c.osd.enabled && prev(c); }}
          : SettingVisibility{[](const Config& c) { return c.osd.enabled; }};
    }
  }

  void registry_detail::add_bar_settings(RegistryContext& ctx) {
    using i18n::tr;
    const auto& cfg = ctx.cfg;
    const auto& env = ctx.env;
    auto& entries = ctx.entries;
    const auto& positionSelect = ctx.positionSelect;

    // Bar: register every configured bar so global search can surface settings from all of them.
    for (const auto& bar : cfg.bars) {
      constexpr SettingsSection section = SettingsSection::Bar;
      const std::vector<std::string> root = {"bar", bar.name};
      auto path = [&](std::string key) {
        std::vector<std::string> p = root;
        p.push_back(std::move(key));
        return p;
      };
      entries.push_back(makeEntry(
          section, "general", tr("settings.schema.shared.enabled.label"), tr("settings.schema.bar.enabled.description"),
          path("enabled"), ToggleSetting{bar.enabled}, "visible"
      ));
      entries.push_back(makeEntry(
          section, "general", tr("settings.schema.shared.position.label"),
          tr("settings.schema.bar.position.description"), path("position"), positionSelect(bar.position), "edge"
      ));
      entries.push_back(makeEntry(
          section, "general", tr("settings.schema.shared.auto-hide.label"),
          tr("settings.schema.bar.auto-hide.description"), path("auto_hide"),
          autoHideModeSelect(barAutoHideMode(bar.autoHide, bar.smartAutoHide), path("smart_auto_hide")),
          "autohide smart workspace"
      ));
      {
        auto e = makeEntry(
            section, "general", tr("settings.schema.bar.auto-hide-delay.label"),
            tr("settings.schema.bar.auto-hide-delay.description"), path("auto_hide_delay_ms"),
            StepperSetting{
                .value = bar.autoHideDelayMs,
                .minValue = static_cast<int>(noctalia::config::schema::kBarAutoHideDelayMsRange.min.value()),
                .maxValue = static_cast<int>(noctalia::config::schema::kBarAutoHideDelayMsRange.max.value()),
                .step = static_cast<int>(noctalia::config::schema::kBarAutoHideDelayMsRange.step.value()),
                .valueSuffix = "ms",
            },
            "autohide delay wait"
        );
        e.visibleWhen = [barName = bar.name](const Config& c) {
          const BarConfig* b = findBar(c, barName);
          return b != nullptr && b->autoHide && !b->smartAutoHide;
        };
        entries.push_back(std::move(e));
      }
      const SettingVisibility autoHideOn = [barName = bar.name](const Config& c) {
        const BarConfig* b = findBar(c, barName);
        return b != nullptr && b->isAutoHideEnabled();
      };
      {
        auto e = makeEntry(
            section, "general", tr("settings.schema.bar.show-on-workspace-switch.label"),
            tr("settings.schema.bar.show-on-workspace-switch.description"), path("show_on_workspace_switch"),
            ToggleSetting{bar.showOnWorkspaceSwitch}, "workspace reveal peek autohide"
        );
        e.visibleWhen = autoHideOn;
        entries.push_back(std::move(e));
      }
      entries.push_back(makeEntry(
          section, "general", tr("settings.schema.shared.reserve-space.label"),
          tr("settings.schema.bar.reserve-space.description"), path("reserve_space"), ToggleSetting{bar.reserveSpace},
          "exclusive zone"
      ));
      entries.push_back(makeEntry(
          section, "general", tr("settings.schema.bar.layer.label"), tr("settings.schema.bar.layer.description"),
          path("layer"),
          asSegmented(plainSelect(
              {{"top", "settings.options.layer.top"}, {"overlay", "settings.options.layer.overlay"}}, bar.layer
          )),
          "layer shell z-order"
      ));
      auto barRows = generateSchemaEntries(noctalia::config::schema::barFieldsSchema(), bar, root);
      for (const auto* key : {"thickness", "scale", "font_scale"}) {
        appendRow(entries, barRows, key);
      }
      entries.push_back(makeEntry(
          section, "layout", tr("settings.schema.shared.ends-margin.label"),
          tr("settings.schema.bar.ends-margin.description"), path("margin_ends"), barMarginStepper(bar.marginEnds),
          "gap inset"
      ));
      entries.push_back(makeEntry(
          section, "layout", tr("settings.schema.shared.edge-margin.label"),
          tr("settings.schema.bar.edge-margin.description"), path("margin_edge"), barMarginStepper(bar.marginEdge),
          "gap inset"
      ));
      entries.push_back(makeEntry(
          section, "layout", tr("settings.schema.shared.opposite-edge-margin.label"),
          tr("settings.schema.bar.opposite-edge-margin.description"), path("margin_opposite_edge"),
          barMarginStepper(bar.marginOppositeEdge), "gap inset strut"
      ));
      appendRow(entries, barRows, "padding");
      entries.push_back(makeEntry(
          section, "shape", tr("settings.schema.shared.corner-radius.label"),
          tr("settings.schema.bar.corner-radius.description"), path("radius"),
          barReservedSlider(bar.radius, 80.0F, 1.0F, true), "rounded"
      ));
      entries.push_back(makeEntry(
          section, "shape", tr("settings.schema.shared.corner-top-left.label"),
          tr("settings.schema.bar.corner-top-left.description"), path("radius_top_left"),
          barCornerSlider(bar.radiusTopLeft), "rounded corner", true
      ));
      entries.push_back(makeEntry(
          section, "shape", tr("settings.schema.shared.corner-top-right.label"),
          tr("settings.schema.bar.corner-top-right.description"), path("radius_top_right"),
          barCornerSlider(bar.radiusTopRight), "rounded corner", true
      ));
      entries.push_back(makeEntry(
          section, "shape", tr("settings.schema.shared.corner-bottom-left.label"),
          tr("settings.schema.bar.corner-bottom-left.description"), path("radius_bottom_left"),
          barCornerSlider(bar.radiusBottomLeft), "rounded corner", true
      ));
      entries.push_back(makeEntry(
          section, "shape", tr("settings.schema.shared.corner-bottom-right.label"),
          tr("settings.schema.bar.corner-bottom-right.description"), path("radius_bottom_right"),
          barCornerSlider(bar.radiusBottomRight), "rounded corner", true
      ));
      {
        auto e = makeEntry(
            section, "shape", tr("settings.schema.bar.concave-edge-corners.label"),
            tr("settings.schema.bar.concave-edge-corners.description"), path("concave_edge_corners"),
            ToggleSetting{bar.concaveEdgeCorners}, "rounded corner carve"
        );
        e.visibleWhen = [barName = bar.name](const Config& c) {
          const BarConfig* b = findBar(c, barName);
          return b != nullptr && b->marginEdge == 0;
        };
        entries.push_back(std::move(e));
      }
      entries.push_back(makeEntry(
          section, "shape", tr("settings.schema.bar.border.label"), tr("settings.schema.bar.border.description"),
          path("border"), colorSpecPicker(bar.border), "outline color", true
      ));
      entries.push_back(makeEntry(
          section, "shape", tr("settings.schema.bar.border-width.label"),
          tr("settings.schema.bar.border-width.description"), path("border_width"),
          barReservedSlider(bar.borderWidth, 20.0F, 0.5F, false), "outline stroke", true
      ));
      appendRow(entries, barRows, "islands");
      entries.push_back(makeEntry(
          section, "effects", tr("settings.schema.shared.background-opacity.label"),
          tr("settings.schema.bar.background-opacity.description"), path("background_opacity"),
          SliderSetting{bar.backgroundOpacity, 0.0F, 1.0F, 0.01F, false}, "alpha transparency translucent opacity"
      ));
      if (env.backgroundEffectBlurSupported) {
        entries.push_back(makeEntry(
            section, "effects", tr("settings.schema.bar.compositor-blur.label"),
            tr("settings.schema.bar.compositor-blur.description"), path("compositor_blur"),
            ToggleSetting{bar.compositorBlur}, "blur frosted background effect wayland"
        ));
      }
      entries.push_back(makeEntry(
          section, "effects", tr("settings.schema.shared.shadow.label"), tr("settings.schema.bar.shadow.description"),
          path("shadow"), ToggleSetting{bar.shadow}, "shadow"
      ));
      entries.push_back(makeEntry(
          section, "effects", tr("settings.schema.shared.contact-shadow.label"),
          tr("settings.schema.bar.contact-shadow.description"), path("contact_shadow"),
          ToggleSetting{bar.contactShadow}, "shadow contact panel attached"
      ));
      entries.push_back(makeEntry(
          section, "layout", tr("settings.schema.bar.panel-overlap.label"),
          tr("settings.schema.bar.panel-overlap.description"), path("panel_overlap"),
          barPanelOverlapStepper(bar.panelOverlap), "seam gap overlap attached panel fractional scale", true
      ));
      const std::string barResolvedFontFamily =
          bar.fontFamily && !bar.fontFamily->empty() ? *bar.fontFamily : cfg.shell.fontFamily;
      {
        SettingControl fontFamilyControl = TextSetting{
            .value = bar.fontFamily.value_or(""), .placeholder = cfg.shell.fontFamily, .browseFileExtensions = {}
        };
        if (!env.fontFamilies.empty()) {
          fontFamilyControl = SearchPickerSetting{
              .options = env.fontFamilies,
              .selectedValue = bar.fontFamily.value_or(""),
              .placeholder = cfg.shell.fontFamily,
              .emptyText = tr("ui.controls.search-picker.empty"),
              .preferredHeight = 280.0F,
          };
        }
        entries.push_back(makeEntry(
            section, "widgets", tr("settings.schema.bar.font-family.label"),
            tr("settings.schema.bar.font-family.description"), path("font_family"), std::move(fontFamilyControl),
            "typeface font"
        ));
      }
      {
        std::vector<SelectOption> fontWeightOptions;
        const auto widgetOptions =
            buildLabelFontWeightSelectOptions(barResolvedFontFamily, FontWeightSelectKind::BarDefault, bar.fontWeight);
        fontWeightOptions.reserve(widgetOptions.size());
        for (const auto& option : widgetOptions) {
          fontWeightOptions.push_back(SelectOption{option.value, tr(option.labelKey)});
        }
        SelectSetting fontWeightSelect{std::move(fontWeightOptions), std::to_string(bar.fontWeight)};
        fontWeightSelect.valueType = SelectValueType::Integer;
        entries.push_back(makeEntry(
            section, "widgets", tr("settings.schema.bar.font-weight.label"),
            tr("settings.schema.bar.font-weight.description"), path("font_weight"), std::move(fontWeightSelect),
            "font text weight"
        ));
      }
      appendRow(entries, barRows, "widget_spacing");
      entries.push_back(makeEntry(
          section, "widgets", tr("settings.schema.bar.widget-color.label"),
          tr("settings.schema.bar.widget-color.description"), path("color"), colorSpecPicker(bar.widgetColor, true),
          "color foreground", true
      ));
      entries.push_back(makeEntry(
          section, "widgets", tr("settings.schema.bar.widget-icon-color.label"),
          tr("settings.schema.bar.widget-icon-color.description"), path("icon_color"),
          colorSpecPicker(bar.widgetIconColor, true), "color icon", true
      ));
      for (const auto* key : {"hover_highlight", "show_tooltip", "capsule", "capsule_thickness"}) {
        appendRow(entries, barRows, key);
      }
      const SettingVisibility capsuleOn = [on = bar.widgetCapsuleDefault](const Config&) { return on; };
      {
        auto e = makeEntry(
            section, "capsules", tr("settings.schema.bar.capsule-radius.label"),
            tr("settings.schema.bar.capsule-radius.description"), path("capsule_radius"),
            OptionalStepperSetting{
                .value = radiusStepperValue(bar.widgetCapsuleRadius),
                .minValue = 0,
                .maxValue = 80,
                .step = 1,
                .fallbackValue = radiusStepperFallback(bar.widgetCapsuleRadius),
                .unsetLabel = tr("common.states.auto"),
                .customLabel = tr("common.states.custom")
            },
            "pill rounded radius", true
        );
        entries.push_back(std::move(e));
      }
      {
        auto e = makeEntry(
            section, "capsules", tr("settings.schema.bar.capsule-fill.label"),
            tr("settings.schema.bar.capsule-fill.description"), path("capsule_fill"),
            colorSpecPicker(bar.widgetCapsuleFill), "color pill", true
        );
        e.visibleWhen = capsuleOn;
        entries.push_back(std::move(e));
      }
      {
        auto e = makeEntry(
            section, "capsules", tr("settings.schema.bar.capsule-foreground.label"),
            tr("settings.schema.bar.capsule-foreground.description"), path("capsule_foreground"),
            colorSpecPicker(bar.widgetCapsuleForeground, true), "color foreground pill", true
        );
        e.visibleWhen = capsuleOn;
        entries.push_back(std::move(e));
      }
      {
        auto e = makeEntry(
            section, "capsules", tr("settings.schema.bar.capsule-border.label"),
            tr("settings.schema.bar.capsule-border.description"), path("capsule_border"),
            colorSpecPicker(bar.widgetCapsuleBorder, true), "color pill outline", true
        );
        e.visibleWhen = capsuleOn;
        entries.push_back(std::move(e));
      }
      {
        auto e = makeEntry(
            section, "capsules", tr("settings.schema.bar.capsule-border-width.label"),
            tr("settings.schema.bar.capsule-border-width.description"), path("capsule_border_width"),
            sliderFor(bar.widgetCapsuleBorderWidth, noctalia::config::schema::kBarCapsuleBorderWidthRange, false),
            "pill outline width", true
        );
        e.visibleWhen = capsuleOn;
        entries.push_back(std::move(e));
      }
      {
        auto e = makeEntry(
            section, "capsules", tr("settings.schema.bar.capsule-padding.label"),
            tr("settings.schema.bar.capsule-padding.description"), path("capsule_padding"),
            SliderSetting{bar.widgetCapsulePadding, 0.0F, 48.0F, 1.0F, false}, "pill inset", true
        );
        e.visibleWhen = capsuleOn;
        entries.push_back(std::move(e));
      }
      {
        auto e = makeEntry(
            section, "capsules", tr("settings.schema.bar.capsule-opacity.label"),
            tr("settings.schema.bar.capsule-opacity.description"), path("capsule_opacity"),
            SliderSetting{bar.widgetCapsuleOpacity, 0.0F, 1.0F, 0.01F, false}, "pill alpha", true
        );
        e.visibleWhen = capsuleOn;
        entries.push_back(std::move(e));
      }
      entries.push_back(makeEntry(
          section, "widget-list", tr("settings.schema.bar.start-widgets.label"),
          tr("settings.schema.bar.start-widgets.description"), path("start"), ListSetting{.items = bar.startWidgets},
          "left"
      ));
      entries.push_back(makeEntry(
          section, "widget-list", tr("settings.schema.bar.center-widgets.label"),
          tr("settings.schema.bar.center-widgets.description"), path("center"), ListSetting{.items = bar.centerWidgets},
          "middle"
      ));
      entries.push_back(makeEntry(
          section, "widget-list", tr("settings.schema.bar.end-widgets.label"),
          tr("settings.schema.bar.end-widgets.description"), path("end"), ListSetting{.items = bar.endWidgets}, "right"
      ));
      {
        auto e = makeEntry(
            section, "dead-zone", tr("settings.schema.bar.dead-zone-click-through.label"),
            tr("settings.schema.bar.dead-zone-click-through.description"),
            std::vector<std::string>{"bar", bar.name, "dead_zone", "click_through"},
            ToggleSetting{bar.deadZone.clickThrough}, "bar empty margin dead zone click through pass input region"
        );
        e.visibleWhen = [autoHideOn](const Config& c) { return !autoHideOn(c); };
        entries.push_back(std::move(e));
      }
      // One row per gesture, from the same closed set widget actions use. An unset row shows the
      // built-in default as its placeholder, so "empty" never reads as "does nothing".
      for (const auto gesture : noctalia::bar::allGestures()) {
        const std::string key(noctalia::bar::gestureConfigKey(gesture));
        const auto configured = bar.deadZone.actions.find(key);
        entries.push_back(makeEntry(
            section, "dead-zone", tr(noctalia::bar::gestureLabelKey(gesture)),
            tr("settings.schema.bar.dead-zone-action.description"),
            std::vector<std::string>{"bar", bar.name, "dead_zone", "actions", key},
            GestureActionSetting{
                .gestureKey = key,
                .configured = configured != bar.deadZone.actions.end() ? configured->second : std::string{},
                .defaultAction = deadZoneDefault(gesture),
            },
            "bar empty margin dead zone action command gesture " + key
        ));
      }
    }
  }

  void registry_detail::add_baroverrides_settings(RegistryContext& ctx) {
    using i18n::tr;
    const auto& cfg = ctx.cfg;
    const auto& env = ctx.env;
    auto& entries = ctx.entries;
    const auto& positionSelect = ctx.positionSelect;

    // Bar monitor overrides (all bars).
    for (const auto& bar : cfg.bars) {
      for (const auto& ovr : bar.monitorOverrides) {
        constexpr SettingsSection section = SettingsSection::Bar;
        const std::vector<std::string> root = {"bar", bar.name, "monitor", ovr.match};
        auto monitorPath = [&](std::string key) {
          std::vector<std::string> p = root;
          p.push_back(std::move(key));
          return p;
        };

        entries.push_back(makeEntry(
            section, "general", tr("settings.schema.shared.enabled.label"),
            tr("settings.schema.bar.enabled.description"), monitorPath("enabled"),
            ToggleSetting{ovr.enabled.value_or(bar.enabled)}, "visible"
        ));
        entries.push_back(makeEntry(
            section, "general", tr("settings.schema.shared.position.label"),
            tr("settings.schema.bar.position.description"), monitorPath("position"),
            positionSelect(ovr.position.value_or(bar.position)), "edge"
        ));
        entries.push_back(makeEntry(
            section, "general", tr("settings.schema.shared.auto-hide.label"),
            tr("settings.schema.bar.auto-hide.description"), monitorPath("auto_hide"),
            autoHideModeSelect(
                barAutoHideMode(ovr.autoHide.value_or(bar.autoHide), ovr.smartAutoHide.value_or(bar.smartAutoHide)),
                monitorPath("smart_auto_hide")
            ),
            "autohide smart workspace"
        ));
        {
          auto e = makeEntry(
              section, "general", tr("settings.schema.bar.auto-hide-delay.label"),
              tr("settings.schema.bar.auto-hide-delay.description"), monitorPath("auto_hide_delay_ms"),
              StepperSetting{
                  .value = ovr.autoHideDelayMs.value_or(bar.autoHideDelayMs),
                  .minValue = static_cast<int>(noctalia::config::schema::kBarAutoHideDelayMsRange.min.value()),
                  .maxValue = static_cast<int>(noctalia::config::schema::kBarAutoHideDelayMsRange.max.value()),
                  .step = static_cast<int>(noctalia::config::schema::kBarAutoHideDelayMsRange.step.value()),
                  .valueSuffix = "ms",
              },
              "autohide delay wait"
          );
          e.visibleWhen = [barName = bar.name, match = ovr.match](const Config& c) {
            const BarConfig* b = findBar(c, barName);
            if (b == nullptr) {
              return false;
            }
            const BarMonitorOverride* o = findMonitorOverride(*b, match);
            const bool autoHide = o != nullptr ? o->autoHide.value_or(b->autoHide) : b->autoHide;
            const bool smartAutoHide = o != nullptr ? o->smartAutoHide.value_or(b->smartAutoHide) : b->smartAutoHide;
            return autoHide && !smartAutoHide;
          };
          entries.push_back(std::move(e));
        }
        const SettingVisibility monitorAutoHideOn = [barName = bar.name, match = ovr.match](const Config& c) {
          const BarConfig* b = findBar(c, barName);
          if (b == nullptr) {
            return false;
          }
          const BarMonitorOverride* o = findMonitorOverride(*b, match);
          return o != nullptr ? o->isAutoHideEnabled(b->autoHide, b->smartAutoHide) : b->isAutoHideEnabled();
        };
        {
          auto e = makeEntry(
              section, "general", tr("settings.schema.bar.show-on-workspace-switch.label"),
              tr("settings.schema.bar.show-on-workspace-switch.description"), monitorPath("show_on_workspace_switch"),
              ToggleSetting{ovr.showOnWorkspaceSwitch.value_or(bar.showOnWorkspaceSwitch)},
              "workspace reveal peek autohide"
          );
          e.visibleWhen = monitorAutoHideOn;
          entries.push_back(std::move(e));
        }
        entries.push_back(makeEntry(
            section, "general", tr("settings.schema.shared.reserve-space.label"),
            tr("settings.schema.bar.reserve-space.description"), monitorPath("reserve_space"),
            ToggleSetting{ovr.reserveSpace.value_or(bar.reserveSpace)}, "exclusive zone"
        ));
        entries.push_back(makeEntry(
            section, "general", tr("settings.schema.bar.layer.label"), tr("settings.schema.bar.layer.description"),
            monitorPath("layer"),
            asSegmented(plainSelect(
                {{"top", "settings.options.layer.top"}, {"overlay", "settings.options.layer.overlay"}},
                ovr.layer.value_or(bar.layer)
            )),
            "layer shell z-order"
        ));
        entries.push_back(makeEntry(
            section, "layout", tr("settings.schema.bar.thickness.label"),
            tr("settings.schema.bar.thickness.description"), monitorPath("thickness"),
            SliderSetting{ovr.thickness.value_or(bar.thickness), 10.0F, 120.0F, 1.0F, true}, "height width"
        ));
        entries.push_back(makeEntry(
            section, "layout", tr("settings.schema.bar.content-scale.label"),
            tr("settings.schema.bar.content-scale.description"), monitorPath("scale"),
            SliderSetting{ovr.scale.value_or(bar.scale), 0.5F, 4.0F, 0.05F, false}, "zoom size"
        ));
        entries.push_back(makeEntry(
            section, "layout", tr("settings.schema.bar.font-scale.label"),
            tr("settings.schema.bar.font-scale.description"), monitorPath("font_scale"),
            sliderFor(ovr.fontScale.value_or(bar.fontScale), noctalia::config::schema::kBarFontScaleRange, false),
            "text zoom size"
        ));
        entries.push_back(makeEntry(
            section, "layout", tr("settings.schema.shared.ends-margin.label"),
            tr("settings.schema.bar.ends-margin.description"), monitorPath("margin_ends"),
            barMarginStepper(ovr.marginEnds.value_or(bar.marginEnds)), "gap inset"
        ));
        entries.push_back(makeEntry(
            section, "layout", tr("settings.schema.shared.edge-margin.label"),
            tr("settings.schema.bar.edge-margin.description"), monitorPath("margin_edge"),
            barMarginStepper(ovr.marginEdge.value_or(bar.marginEdge)), "gap inset"
        ));
        entries.push_back(makeEntry(
            section, "layout", tr("settings.schema.shared.opposite-edge-margin.label"),
            tr("settings.schema.bar.opposite-edge-margin.description"), monitorPath("margin_opposite_edge"),
            barMarginStepper(ovr.marginOppositeEdge.value_or(bar.marginOppositeEdge)), "gap inset strut"
        ));
        entries.push_back(makeEntry(
            section, "layout", tr("settings.schema.bar.content-padding.label"),
            tr("settings.schema.bar.content-padding.description"), monitorPath("padding"),
            SliderSetting{ovr.padding.value_or(bar.padding), 0.0F, 80.0F, 1.0F, true}, "inset"
        ));
        entries.push_back(makeEntry(
            section, "shape", tr("settings.schema.shared.corner-radius.label"),
            tr("settings.schema.bar.corner-radius.description"), monitorPath("radius"),
            barReservedSlider(ovr.radius.value_or(bar.radius), 80.0F, 1.0F, true), "rounded"
        ));
        entries.push_back(makeEntry(
            section, "shape", tr("settings.schema.shared.corner-top-left.label"),
            tr("settings.schema.bar.corner-top-left.description"), monitorPath("radius_top_left"),
            barCornerSlider(ovr.radiusTopLeft.value_or(bar.radiusTopLeft)), "rounded corner", true
        ));
        entries.push_back(makeEntry(
            section, "shape", tr("settings.schema.shared.corner-top-right.label"),
            tr("settings.schema.bar.corner-top-right.description"), monitorPath("radius_top_right"),
            barCornerSlider(ovr.radiusTopRight.value_or(bar.radiusTopRight)), "rounded corner", true
        ));
        entries.push_back(makeEntry(
            section, "shape", tr("settings.schema.shared.corner-bottom-left.label"),
            tr("settings.schema.bar.corner-bottom-left.description"), monitorPath("radius_bottom_left"),
            barCornerSlider(ovr.radiusBottomLeft.value_or(bar.radiusBottomLeft)), "rounded corner", true
        ));
        entries.push_back(makeEntry(
            section, "shape", tr("settings.schema.shared.corner-bottom-right.label"),
            tr("settings.schema.bar.corner-bottom-right.description"), monitorPath("radius_bottom_right"),
            barCornerSlider(ovr.radiusBottomRight.value_or(bar.radiusBottomRight)), "rounded corner", true
        ));
        {
          auto e = makeEntry(
              section, "shape", tr("settings.schema.bar.concave-edge-corners.label"),
              tr("settings.schema.bar.concave-edge-corners.description"), monitorPath("concave_edge_corners"),
              ToggleSetting{ovr.concaveEdgeCorners.value_or(bar.concaveEdgeCorners)}, "rounded corner carve"
          );
          e.visibleWhen = [barName = bar.name, match = ovr.match](const Config& c) {
            const BarConfig* b = findBar(c, barName);
            if (b == nullptr) {
              return false;
            }
            const BarMonitorOverride* mo = findMonitorOverride(*b, match);
            const std::int32_t marginEdge = mo && mo->marginEdge.has_value() ? *mo->marginEdge : b->marginEdge;
            return marginEdge == 0;
          };
          entries.push_back(std::move(e));
        }
        entries.push_back(makeEntry(
            section, "shape", tr("settings.schema.bar.border.label"), tr("settings.schema.bar.border.description"),
            monitorPath("border"), colorSpecPicker(ovr.border, true, tr("common.states.inherit")), "outline color", true
        ));
        entries.push_back(makeEntry(
            section, "shape", tr("settings.schema.bar.border-width.label"),
            tr("settings.schema.bar.border-width.description"), monitorPath("border_width"),
            barReservedSlider(ovr.borderWidth.value_or(bar.borderWidth), 20.0F, 0.5F, false), "outline stroke", true
        ));
        entries.push_back(makeEntry(
            section, "effects", tr("settings.schema.shared.background-opacity.label"),
            tr("settings.schema.bar.background-opacity.description"), monitorPath("background_opacity"),
            SliderSetting{ovr.backgroundOpacity.value_or(bar.backgroundOpacity), 0.0F, 1.0F, 0.01F, false},
            "alpha transparency translucent opacity"
        ));
        if (env.backgroundEffectBlurSupported) {
          entries.push_back(makeEntry(
              section, "effects", tr("settings.schema.bar.compositor-blur.label"),
              tr("settings.schema.bar.compositor-blur.description"), monitorPath("compositor_blur"),
              ToggleSetting{ovr.compositorBlur.value_or(bar.compositorBlur)}, "blur frosted background effect wayland"
          ));
        }
        entries.push_back(makeEntry(
            section, "effects", tr("settings.schema.shared.shadow.label"), tr("settings.schema.bar.shadow.description"),
            monitorPath("shadow"), ToggleSetting{ovr.shadow.value_or(bar.shadow)}, "shadow"
        ));
        entries.push_back(makeEntry(
            section, "effects", tr("settings.schema.shared.contact-shadow.label"),
            tr("settings.schema.bar.contact-shadow.description"), monitorPath("contact_shadow"),
            ToggleSetting{ovr.contactShadow.value_or(bar.contactShadow)}, "shadow contact panel attached"
        ));
        entries.push_back(makeEntry(
            section, "layout", tr("settings.schema.bar.panel-overlap.label"),
            tr("settings.schema.bar.panel-overlap.description"), monitorPath("panel_overlap"),
            barPanelOverlapStepper(ovr.panelOverlap.value_or(bar.panelOverlap)),
            "seam gap overlap attached panel fractional scale", true
        ));
        {
          const std::string monitorInheritedFontFamily = bar.fontFamily.value_or(cfg.shell.fontFamily);
          SettingControl fontFamilyControl = TextSetting{
              .value = ovr.fontFamily.value_or(""),
              .placeholder = monitorInheritedFontFamily,
              .browseFileExtensions = {}
          };
          if (!env.fontFamilies.empty()) {
            fontFamilyControl = SearchPickerSetting{
                .options = env.fontFamilies,
                .selectedValue = ovr.fontFamily.value_or(""),
                .placeholder = monitorInheritedFontFamily,
                .emptyText = tr("ui.controls.search-picker.empty"),
                .preferredHeight = 280.0F,
            };
          }
          entries.push_back(makeEntry(
              section, "widgets", tr("settings.schema.bar.font-family.label"),
              tr("settings.schema.bar.font-family.description"), monitorPath("font_family"),
              std::move(fontFamilyControl), "typeface font", true
          ));
        }
        entries.push_back(makeEntry(
            section, "widgets", tr("settings.schema.bar.widget-spacing.label"),
            tr("settings.schema.bar.widget-spacing.description"), monitorPath("widget_spacing"),
            SliderSetting{ovr.widgetSpacing.value_or(bar.widgetSpacing), 0.0F, 32.0F, 1.0F, true}, "gap"
        ));
        entries.push_back(makeEntry(
            section, "widgets", tr("settings.schema.bar.widget-color.label"),
            tr("settings.schema.bar.widget-color.description"), monitorPath("color"),
            colorSpecPicker(ovr.widgetColor, true, tr("common.states.inherit")), "color foreground", true
        ));
        entries.push_back(makeEntry(
            section, "widgets", tr("settings.schema.bar.widget-icon-color.label"),
            tr("settings.schema.bar.widget-icon-color.description"), monitorPath("icon_color"),
            colorSpecPicker(ovr.widgetIconColor, true, tr("common.states.inherit")), "color icon", true
        ));
        entries.push_back(makeEntry(
            section, "capsules", tr("settings.schema.bar.widget-capsules.label"),
            tr("settings.schema.bar.widget-capsules.description"), monitorPath("capsule"),
            ToggleSetting{ovr.widgetCapsuleDefault.value_or(bar.widgetCapsuleDefault)}, "pill"
        ));
        entries.push_back(makeEntry(
            section, "capsules", tr("settings.schema.bar.capsule-thickness.label"),
            tr("settings.schema.bar.capsule-thickness.description"), monitorPath("capsule_thickness"),
            SliderSetting{ovr.capsuleThickness.value_or(bar.capsuleThickness), 0.1F, 1.0F, 0.01F, false},
            "pill thickness size", true
        ));
        const SettingVisibility monitorCapsuleOn =
            [on = ovr.widgetCapsuleDefault.value_or(bar.widgetCapsuleDefault)](const Config&) { return on; };
        {
          auto e = makeEntry(
              section, "capsules", tr("settings.schema.bar.capsule-radius.label"),
              tr("settings.schema.bar.capsule-radius.description"), monitorPath("capsule_radius"),
              OptionalStepperSetting{
                  .value = radiusStepperValue(ovr.widgetCapsuleRadius),
                  .minValue = 0,
                  .maxValue = 80,
                  .step = 1,
                  .fallbackValue = radiusStepperFallback(bar.widgetCapsuleRadius),
                  .unsetLabel = tr("common.states.inherit"),
                  .customLabel = tr("common.states.custom")
              },
              "pill rounded radius", true
          );
          entries.push_back(std::move(e));
        }
        {
          auto e = makeEntry(
              section, "capsules", tr("settings.schema.bar.capsule-fill.label"),
              tr("settings.schema.bar.capsule-fill.description"), monitorPath("capsule_fill"),
              colorSpecPicker(ovr.widgetCapsuleFill, true, tr("common.states.inherit")), "color pill", true
          );
          e.visibleWhen = monitorCapsuleOn;
          entries.push_back(std::move(e));
        }
        {
          auto e = makeEntry(
              section, "capsules", tr("settings.schema.bar.capsule-foreground.label"),
              tr("settings.schema.bar.capsule-foreground.description"), monitorPath("capsule_foreground"),
              colorSpecPicker(ovr.widgetCapsuleForeground, true, tr("common.states.inherit")), "color foreground pill",
              true
          );
          e.visibleWhen = monitorCapsuleOn;
          entries.push_back(std::move(e));
        }
        {
          auto e = makeEntry(
              section, "capsules", tr("settings.schema.bar.capsule-border.label"),
              tr("settings.schema.bar.capsule-border.description"), monitorPath("capsule_border"),
              colorSpecPicker(
                  ovr.widgetCapsuleBorderSpecified ? ovr.widgetCapsuleBorder : std::optional<ColorSpec>{}, true,
                  tr("common.states.inherit")
              ),
              "color pill outline", true
          );
          e.visibleWhen = monitorCapsuleOn;
          entries.push_back(std::move(e));
        }
        {
          auto e = makeEntry(
              section, "capsules", tr("settings.schema.bar.capsule-border-width.label"),
              tr("settings.schema.bar.capsule-border-width.description"), monitorPath("capsule_border_width"),
              sliderFor(
                  ovr.widgetCapsuleBorderWidth.value_or(bar.widgetCapsuleBorderWidth),
                  noctalia::config::schema::kBarCapsuleBorderWidthRange, false
              ),
              "pill outline width", true
          );
          e.visibleWhen = monitorCapsuleOn;
          entries.push_back(std::move(e));
        }
        {
          auto e = makeEntry(
              section, "capsules", tr("settings.schema.bar.capsule-padding.label"),
              tr("settings.schema.bar.capsule-padding.description"), monitorPath("capsule_padding"),
              SliderSetting{ovr.widgetCapsulePadding.value_or(bar.widgetCapsulePadding), 0.0F, 48.0F, 1.0F, false},
              "pill inset", true
          );
          e.visibleWhen = monitorCapsuleOn;
          entries.push_back(std::move(e));
        }
        {
          auto e = makeEntry(
              section, "capsules", tr("settings.schema.bar.capsule-opacity.label"),
              tr("settings.schema.bar.capsule-opacity.description"), monitorPath("capsule_opacity"),
              SliderSetting{ovr.widgetCapsuleOpacity.value_or(bar.widgetCapsuleOpacity), 0.0F, 1.0F, 0.01F, false},
              "pill alpha", true
          );
          e.visibleWhen = monitorCapsuleOn;
          entries.push_back(std::move(e));
        }
        entries.push_back(makeEntry(
            section, "widget-list", tr("settings.schema.bar.start-widgets.label"),
            tr("settings.schema.bar.start-widgets.description"), monitorPath("start"),
            ListSetting{.items = ovr.startWidgets.value_or(bar.startWidgets)}, "left"
        ));
        entries.push_back(makeEntry(
            section, "widget-list", tr("settings.schema.bar.center-widgets.label"),
            tr("settings.schema.bar.center-widgets.description"), monitorPath("center"),
            ListSetting{.items = ovr.centerWidgets.value_or(bar.centerWidgets)}, "middle"
        ));
        entries.push_back(makeEntry(
            section, "widget-list", tr("settings.schema.bar.end-widgets.label"),
            tr("settings.schema.bar.end-widgets.description"), monitorPath("end"),
            ListSetting{.items = ovr.endWidgets.value_or(bar.endWidgets)}, "right"
        ));
        {
          std::vector<std::string> clickThroughPath = root;
          clickThroughPath.emplace_back("dead_zone");
          clickThroughPath.emplace_back("click_through");
          auto e = makeEntry(
              section, "dead-zone", tr("settings.schema.bar.dead-zone-click-through.label"),
              tr("settings.schema.bar.dead-zone-click-through.description"), std::move(clickThroughPath),
              ToggleSetting{ovr.deadZone.clickThrough.value_or(bar.deadZone.clickThrough)},
              "bar empty margin dead zone click through pass input region"
          );
          e.visibleWhen = [monitorAutoHideOn](const Config& c) { return !monitorAutoHideOn(c); };
          entries.push_back(std::move(e));
        }
        for (const auto gesture : noctalia::bar::allGestures()) {
          const std::string key(noctalia::bar::gestureConfigKey(gesture));
          std::vector<std::string> gesturePath = root;
          gesturePath.emplace_back("dead_zone");
          gesturePath.emplace_back("actions");
          gesturePath.emplace_back(key);

          const auto& overrideActions = ovr.deadZone.actions;
          std::string configured;
          if (overrideActions.has_value()) {
            const auto overridden = overrideActions->find(key);
            if (overridden != overrideActions->end()) {
              configured = overridden->second;
            }
          }
          const auto inherited = bar.deadZone.actions.find(key);
          entries.push_back(makeEntry(
              section, "dead-zone", tr(noctalia::bar::gestureLabelKey(gesture)),
              tr("settings.schema.bar.dead-zone-action.description"), std::move(gesturePath),
              GestureActionSetting{
                  .gestureKey = key,
                  .configured = std::move(configured),
                  .defaultAction =
                      inherited != bar.deadZone.actions.end() ? inherited->second : deadZoneDefault(gesture),
              },
              "bar empty margin dead zone action command gesture " + key
          ));
        }
      }
    }
  }

} // namespace settings
