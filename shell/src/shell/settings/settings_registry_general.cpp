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

  void registry_detail::add_appearance_settings(RegistryContext& ctx) {
    using i18n::tr;
    const auto& cfg = ctx.cfg;
    const auto& env = ctx.env;
    auto& entries = ctx.entries;
    auto& shellRows = ctx.shellRows;

    // Appearance
    auto themeRows = generateSchemaEntries(noctalia::config::schema::themeSchema(), cfg.theme, {"theme"});
    shellRows = generateSchemaEntries(noctalia::config::schema::shellSchema(), cfg.shell, {"shell"});
    for (const auto* key : {"mode", "shell_mode", "source"}) {
      appendRow(entries, themeRows, key);
    }
    if (cfg.theme.source == PaletteSource::Builtin) {
      entries.push_back(makeEntry(
          SettingsSection::Appearance, "theme", tr("settings.schema.appearance.builtin-palette.label"),
          tr("settings.schema.appearance.builtin-palette.description"), {"theme", "builtin"},
          builtinPaletteSelect(cfg.theme.builtinPalette, shellThemeMode(cfg.theme)), "builtin palette colors"
      ));
    } else if (cfg.theme.source == PaletteSource::Wallpaper) {
      entries.push_back(makeEntry(
          SettingsSection::Appearance, "theme", tr("settings.schema.appearance.wallpaper-generation-scheme.label"),
          tr("settings.schema.appearance.wallpaper-generation-scheme.description"), {"theme", "wallpaper_scheme"},
          wallpaperSchemeSelect(cfg.theme.wallpaperScheme), "wallpaper palette generator scheme material you m3 colors"
      ));
    } else if (cfg.theme.source == PaletteSource::Community) {
      SettingControl communityPaletteControl =
          TextSetting{.value = cfg.theme.communityPalette, .placeholder = "Oxocarbon", .browseFileExtensions = {}};
      if (!env.communityPalettes.empty()) {
        communityPaletteControl = SearchPickerSetting{
            .options = env.communityPalettes,
            .selectedValue = cfg.theme.communityPalette,
            .placeholder = tr("settings.schema.appearance.community-palette.search-placeholder"),
            .emptyText = tr("ui.controls.search-picker.empty"),
            .preferredHeight = 240.0F,
        };
      }
      entries.push_back(makeEntry(
          SettingsSection::Appearance, "theme", tr("settings.schema.appearance.community-palette.label"),
          tr("settings.schema.appearance.community-palette.description"), {"theme", "community_palette"},
          std::move(communityPaletteControl), "community palette colors"
      ));
    } else if (cfg.theme.source == PaletteSource::Custom) {
      SettingControl customPaletteControl =
          TextSetting{.value = cfg.theme.customPalette, .placeholder = "", .browseFileExtensions = {}};
      if (!env.customPalettes.empty()) {
        customPaletteControl = SearchPickerSetting{
            .options = env.customPalettes,
            .selectedValue = cfg.theme.customPalette,
            .placeholder = tr("settings.schema.appearance.custom-palette.search-placeholder"),
            .emptyText = tr("ui.controls.search-picker.empty"),
            .preferredHeight = 240.0F,
        };
      }
      entries.push_back(makeEntry(
          SettingsSection::Appearance, "theme", tr("settings.schema.appearance.custom-palette.label"),
          tr("settings.schema.appearance.custom-palette.description"), {"theme", "custom_palette"},
          std::move(customPaletteControl), "custom palette colors"
      ));
    }
    entries.push_back(makeEntry(
        SettingsSection::Appearance, "theme", tr("settings.schema.appearance.palette-inspector.label"),
        tr("settings.schema.appearance.palette-inspector.description"), {},
        ButtonSetting{
            .label = tr("settings.schema.appearance.palette-inspector.button"),
            .action = {},
            .glyph = "color-swatch",
        },
        "palette inspector template tokens colors swatches hex terminal ansi"
    ));
    appendGroup(entries, themeRows, "theme");
    {
      SettingControl fontFamilyControl =
          TextSetting{.value = cfg.shell.fontFamily, .placeholder = "sans-serif", .browseFileExtensions = {}};
      if (!env.fontFamilies.empty()) {
        fontFamilyControl = SearchPickerSetting{
            .options = env.fontFamilies,
            .selectedValue = cfg.shell.fontFamily,
            .placeholder = "sans-serif",
            .emptyText = tr("ui.controls.search-picker.empty"),
            .preferredHeight = 280.0F,
        };
      }
      entries.push_back(makeEntry(
          SettingsSection::TextScale, "text", tr("settings.schema.appearance.font-family.label"),
          tr("settings.schema.appearance.font-family.description"), {"shell", "font_family"},
          std::move(fontFamilyControl), "typeface"
      ));
    }
    entries.push_back(makeEntry(
        SettingsSection::TextScale, "text", tr("settings.schema.appearance.language.label"),
        tr("settings.schema.appearance.language.description"), {"shell", "lang"}, languageSelect(cfg.shell.lang),
        "locale translation", true
    ));
    appendRow(entries, shellRows, "corner_radius_scale");
    appendRow(entries, shellRows, "app_icon_colorize");
    {
      const SettingVisibility colorizeOn = [](const Config& c) { return c.shell.appIconColorize; };
      ShellConfig colorizeShell = cfg.shell;
      colorizeShell.appIconColorize = true;
      const ColorSpec pickerColor =
          cfg.shell.appIconColor.value_or(*effectiveShellAppIconColorizationTint(colorizeShell));
      auto e = makeEntry(
          SettingsSection::Appearance, "icons", tr("settings.schema.appearance.app-icon-color.label"),
          tr("settings.schema.appearance.app-icon-color.description"), {"shell", "app_icon_color"},
          colorSpecPicker(pickerColor), "color role dock tray application icons"
      );
      e.visibleWhen = colorizeOn;
      entries.push_back(std::move(e));
    }
    appendRow(entries, shellRows, "show_close_buttons");
    auto accessibilityRows =
        generateSchemaEntries(noctalia::config::schema::accessibilitySchema(), cfg.accessibility, {"accessibility"});
    appendRow(entries, accessibilityRows, "ui_scale");
    // On andromeda the compositor's appearance.increase_contrast row replaces this one.
    if (env.compositorSettings == nullptr) {
      entries.push_back(makeEntry(
          SettingsSection::TextScale, "accessibility", tr("settings.schema.accessibility.high-contrast.label"),
          tr("settings.schema.accessibility.high-contrast.description"), {"accessibility", "high_contrast"},
          ToggleSetting{cfg.accessibility.highContrast}, "accessibility high contrast visually impaired"
      ));
    }
    auto animationRows = generateSchemaEntries(
        noctalia::config::schema::animationSchema(), cfg.shell.animation, {"shell", "animation"}
    );
    appendGroup(entries, animationRows, "motion");
    for (const auto* key : {"button_borders", "input_borders", "popup_borders", "card_borders"}) {
      appendRow(entries, shellRows, key);
    }
    entries.push_back(makeEntry(
        SettingsSection::Appearance, "surfaces", tr("settings.schema.shared.shadow-direction.label"),
        tr("settings.schema.appearance.global-shadow-direction.description"), {"shell", "shadow", "direction"},
        enumSelect(kShadowDirections, cfg.shell.shadow.direction), "shadow direction"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Appearance, "surfaces", tr("settings.schema.shared.shadow-alpha.label"),
        tr("settings.schema.appearance.global-shadow-alpha.description"), {"shell", "shadow", "alpha"},
        sliderFor(cfg.shell.shadow.alpha, noctalia::config::schema::kUnitRange, false), "shadow opacity", true
    ));
    appendRow(entries, shellRows, "popup_shadows");
  }

  void registry_detail::add_wallpaper_settings(RegistryContext& ctx) {
    using i18n::tr;
    const auto& cfg = ctx.cfg;
    const auto& env = ctx.env;
    auto& entries = ctx.entries;

    // Wallpaper
    if (env.compositorSettings != nullptr) {
      addCompositorEntries(entries, *env.compositorSettings);
    }
    entries.push_back(makeEntry(
        SettingsSection::Wallpaper, "general", tr("settings.schema.shared.enabled.label"),
        tr("settings.schema.wallpaper.enabled.description"), {"wallpaper", "enabled"},
        ToggleSetting{cfg.wallpaper.enabled}, "background image"
    ));
    auto wallpaperRows = generateSchemaEntries(noctalia::config::schema::wallpaperSchema(), cfg.wallpaper, {"wallpaper"});
    auto automationRows = generateSchemaEntries(
        noctalia::config::schema::wallpaperAutomationRowsSchema(), cfg.wallpaper.automation, {"wallpaper", "automation"}
    );
    appendRow(entries, wallpaperRows, "fill_mode");
    entries.push_back(makeEntry(
        SettingsSection::Wallpaper, "general", tr("settings.schema.wallpaper.fill-color.label"),
        tr("settings.schema.wallpaper.fill-color.description"), {"wallpaper", "fill_color"},
        colorSpecPicker(cfg.wallpaper.fillColor, true), "background solid color"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Wallpaper, "directories", tr("settings.schema.wallpaper.directory.label"),
        tr("settings.schema.wallpaper.directory.description"), {"wallpaper", "directory"},
        TextSetting{
            .value = cfg.wallpaper.directory,
            .placeholder = FileUtils::defaultPicturesDirectory().string(),
            .browseMode = TextSettingBrowseMode::SelectFolder,
            .browseFileExtensions = {}
        },
        "folder path"
    ));
    appendGroup(entries, wallpaperRows, "directories");
    for (const auto& outputOpt : env.availableOutputs) {
      const std::string& connector = outputOpt.value;
      if (connector.empty()) {
        continue;
      }
      const std::vector<std::string> root = {"wallpaper", "monitor", connector};
      auto monitorPath = [&](std::string key) {
        std::vector<std::string> p = root;
        p.push_back(std::move(key));
        return p;
      };
      constexpr SettingsSection section = SettingsSection::Wallpaper;
      const WallpaperMonitorOverride* ovr = nullptr;
      for (const auto& candidate : cfg.wallpaper.monitorOverrides) {
        if (candidate.match == connector) {
          ovr = &candidate;
          break;
        }
      }
      auto perMonOn = SettingVisibility{[](const Config& c) { return c.wallpaper.perMonitorDirectories; }};
      {
        auto e = makeEntry(
            section, "monitors", connector, tr("settings.schema.wallpaper.fill-color.label"), monitorPath("fill_color"),
            colorSpecPicker(
                ovr != nullptr ? ovr->fillColor : std::optional<ColorSpec>{}, true, tr("common.states.inherit")
            ),
            "monitor background solid color", true
        );
        entries.push_back(std::move(e));
      }
      {
        auto e = makeEntry(
            section, "monitors", connector, tr("settings.schema.wallpaper.monitor-directory.label"),
            monitorPath("directory"),
            TextSetting{
                .value = ovr != nullptr && ovr->directory.has_value() ? *ovr->directory : "",
                .placeholder = FileUtils::defaultPicturesDirectory().string(),
                .browseMode = TextSettingBrowseMode::SelectFolder,
                .browseFileExtensions = {}
            },
            "monitor folder"
        );
        e.visibleWhen = perMonOn;
        entries.push_back(std::move(e));
      }
      {
        auto e = makeEntry(
            section, "monitors", connector, tr("settings.schema.wallpaper.monitor-directory-light.label"),
            monitorPath("directory_light"),
            TextSetting{
                .value = ovr != nullptr && ovr->directoryLight.has_value() ? *ovr->directoryLight : "",
                .placeholder = tr("settings.schema.wallpaper.monitor-directory-light.placeholder"),
                .browseMode = TextSettingBrowseMode::SelectFolder,
                .browseFileExtensions = {}
            },
            "monitor light folder", true
        );
        e.visibleWhen = perMonOn;
        entries.push_back(std::move(e));
      }
      {
        auto e = makeEntry(
            section, "monitors", connector, tr("settings.schema.wallpaper.monitor-directory-dark.label"),
            monitorPath("directory_dark"),
            TextSetting{
                .value = ovr != nullptr && ovr->directoryDark.has_value() ? *ovr->directoryDark : "",
                .placeholder = tr("settings.schema.wallpaper.monitor-directory-dark.placeholder"),
                .browseMode = TextSettingBrowseMode::SelectFolder,
                .browseFileExtensions = {}
            },
            "monitor dark folder", true
        );
        e.visibleWhen = perMonOn;
        entries.push_back(std::move(e));
      }
    }
    {
      MultiSelectSetting transitions;
      transitions.options.reserve(std::size(kWallpaperTransitions));
      for (const auto& opt : kWallpaperTransitions) {
        transitions.options.push_back(SelectOption{std::string(opt.key), tr(opt.labelKey)});
      }
      transitions.selectedValues.reserve(cfg.wallpaper.transitions.size());
      for (const auto& t : cfg.wallpaper.transitions) {
        transitions.selectedValues.emplace_back(enumToKey(kWallpaperTransitions, t));
      }
      entries.push_back(makeEntry(
          SettingsSection::Wallpaper, "transition", tr("settings.schema.wallpaper.transitions.label"),
          tr("settings.schema.wallpaper.transitions.description"), {"wallpaper", "transition"}, std::move(transitions),
          "effects animation pool"
      ));
    }
    appendGroup(entries, wallpaperRows, "transition");
    appendRow(entries, automationRows, "enabled");
    appendRow(entries, automationRows, "interval_seconds");
    entries.push_back(makeEntry(
        SettingsSection::Wallpaper, "automation", tr("settings.schema.wallpaper.automation-order.label"),
        tr("settings.schema.wallpaper.automation-order.description"), {"wallpaper", "automation", "order"},
        asSegmented(enumSelect(kWallpaperAutomationOrders, cfg.wallpaper.automation.order)), "rotate slideshow"
    ));
    appendGroup(entries, automationRows, "automation");
  }

  void registry_detail::add_templates_settings(RegistryContext& ctx) {
    using i18n::tr;
    const auto& cfg = ctx.cfg;
    const auto& env = ctx.env;
    auto& entries = ctx.entries;

    // Templates
    const auto builtInTemplatesOn =
        SettingVisibility{[](const Config& c) { return c.theme.templates.enableBuiltinTemplates; }};
    const auto communityTemplatesOn =
        SettingVisibility{[](const Config& c) { return c.theme.templates.enableCommunityTemplates; }};
    auto templateRows = generateSchemaEntries(
        noctalia::config::schema::themeTemplatesSchema(), cfg.theme.templates, {"theme", "templates"}
    );
    appendRow(entries, templateRows, "enable_builtin_templates");
    {
      auto e = makeEntry(
          SettingsSection::Appearance, "templates", tr("settings.schema.templates.builtin-ids.label"),
          tr("settings.schema.templates.builtin-ids.description"), {"theme", "templates", "builtin_ids"},
          TemplateGridSetting{
              .options = noctalia::theme::availableTemplates(),
              .selectedValues = cfg.theme.templates.builtinIds,
              .emptyText = tr("settings.schema.templates.builtin-ids.empty"),
          },
          "theme templates apps foot walker gtk"
      );
      e.visibleWhen = builtInTemplatesOn;
      entries.push_back(std::move(e));
    }
    appendRow(entries, templateRows, "enable_community_templates");
    {
      const std::size_t enabledCount = cfg.theme.templates.communityIds.size();
      auto e = makeEntry(
          SettingsSection::Appearance, "templates", tr("settings.schema.templates.community-ids.label"),
          enabledCount == 0 ? tr("settings.schema.templates.community-ids.description")
                            : tr("settings.templates.store.enabled-count", "count", std::to_string(enabledCount)),
          {},
          ButtonSetting{
              .label = tr("settings.templates.store.browse"),
              .glyph = "apps",
          },
          "theme templates community apps discord fuzzel vscode walker store browse"
      );
      e.visibleWhen = communityTemplatesOn;
      entries.push_back(std::move(e));
    }
  }

  void registry_detail::add_dock_settings(RegistryContext& ctx) {
    using i18n::tr;
    const auto& cfg = ctx.cfg;
    const auto& env = ctx.env;
    auto& entries = ctx.entries;

    // Dock and per-monitor dock pages share the same controls. Override pages display effective values while
    // writing to their own table, so untouched fields continue to inherit from the global dock.
    const auto addDockSettings = [&](const DockConfig& dock, std::vector<std::string> root,
                                     const std::optional<std::string>& monitorTable) {
      const auto path = [&root](std::string key) {
        auto result = root;
        result.push_back(std::move(key));
        return result;
      };
      const auto currentDock = [monitorTable](const Config& current) {
        if (monitorTable) {
          if (const auto* override = findDockMonitorOverride(current.dock, *monitorTable)) {
            return resolveDockMonitorOverride(current.dock, *override);
          }
        }
        return current.dock;
      };

      entries.push_back(makeEntry(
          SettingsSection::Dock, "general", tr("settings.schema.shared.enabled.label"),
          tr("settings.schema.dock.enabled.description"), path("enabled"), ToggleSetting{dock.enabled}, "launcher apps"
      ));
      auto dockRows = generateSchemaEntries(noctalia::config::schema::dockSchema(), dock, root);
      appendRow(entries, dockRows, "active_monitor_only");
      entries.push_back(makeEntry(
          SettingsSection::Dock, "behavior", tr("settings.schema.shared.auto-hide.label"),
          tr("settings.schema.dock.auto-hide.description"), path("auto_hide"),
          autoHideModeSelect(barAutoHideMode(dock.autoHide, dock.smartAutoHide), path("smart_auto_hide")),
          "autohide smart workspace"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "behavior", tr("settings.schema.shared.reserve-space.label"),
          tr("settings.schema.dock.reserve-space.description"), path("reserve_space"), ToggleSetting{dock.reserveSpace},
          "exclusive zone"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "behavior", tr("settings.schema.dock.layer.label"),
          tr("settings.schema.dock.layer.description"), path("layer"),
          asSegmented(plainSelect(
              {{"top", "settings.options.layer.top"}, {"overlay", "settings.options.layer.overlay"}}, dock.layer
          )),
          "layer shell z-order"
      ));
      appendGroup(entries, dockRows, "behavior");
      appendRow(entries, dockRows, "launcher_position");
      const SettingVisibility dockLauncherEnabled = [currentDock](const Config& current) {
        const auto effective = currentDock(current);
        return effective.launcherPosition == DockLauncherPosition::Start
            || effective.launcherPosition == DockLauncherPosition::End;
      };
      {
        auto entry = makeEntry(
            SettingsSection::Dock, "layout", tr("settings.schema.dock.launcher-icon.label"),
            tr("settings.schema.dock.launcher-icon.description"), path("launcher_icon"),
            TextSetting{.value = dock.launcherIcon, .placeholder = "grid-dots", .browseFileExtensions = {}},
            "launcher apps icon glyph"
        );
        entry.visibleWhen = dockLauncherEnabled;
        entries.push_back(std::move(entry));
      }
      {
        auto entry = makeEntry(
            SettingsSection::Dock, "layout", tr("settings.schema.dock.launcher-custom-image.label"),
            tr("settings.schema.dock.launcher-custom-image.description"), path("launcher_custom_image"),
            TextSetting{
                .value = dock.launcherCustomImage,
                .placeholder = tr("settings.schema.dock.launcher-custom-image.placeholder"),
                .browseMode = TextSettingBrowseMode::OpenFile,
                .browseFileExtensions = DirectoryScanner::imageExtensionFilter(true),
                .browseFallbackDirectory = "/usr/share/icons",
            },
            "launcher apps image picture logo"
        );
        entry.visibleWhen = dockLauncherEnabled;
        entries.push_back(std::move(entry));
      }
      {
        auto entry = makeEntry(
            SettingsSection::Dock, "layout", tr("settings.schema.dock.launcher-custom-image-colorize.label"),
            tr("settings.schema.dock.launcher-custom-image-colorize.description"),
            path("launcher_custom_image_colorize"), ToggleSetting{dock.launcherCustomImageColorize},
            "launcher apps image tint color"
        );
        entry.visibleWhen = dockLauncherEnabled;
        entries.push_back(std::move(entry));
      }
      entries.push_back(makeEntry(
          SettingsSection::Dock, "layout", tr("settings.schema.shared.position.label"),
          tr("settings.schema.dock.position.description"), path("position"),
          asSegmented(enumSelect(kDockEdges, dock.position)), "edge"
      ));
      appendRow(entries, dockRows, "icon_size");
      entries.push_back(makeEntry(
          SettingsSection::Dock, "layout", tr("settings.schema.shared.main-axis-padding.label"),
          tr("settings.schema.dock.main-axis-padding.description"), path("main_axis_padding"),
          sliderFor(dock.mainAxisPadding, noctalia::config::schema::kDockPaddingRange, true), "inset"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "layout", tr("settings.schema.shared.cross-axis-padding.label"),
          tr("settings.schema.dock.cross-axis-padding.description"), path("cross_axis_padding"),
          sliderFor(dock.crossAxisPadding, noctalia::config::schema::kDockPaddingRange, true), "inset"
      ));
      appendRow(entries, dockRows, "item_spacing");
      entries.push_back(makeEntry(
          SettingsSection::Dock, "layout", tr("settings.schema.shared.ends-margin.label"),
          tr("settings.schema.dock.ends-margin.description"), path("margin_ends"),
          sliderFor(dock.marginEnds, noctalia::config::schema::kDockMarginEndsRange, true), "gap inset"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "layout", tr("settings.schema.shared.edge-margin.label"),
          tr("settings.schema.dock.edge-margin.description"), path("margin_edge"),
          sliderFor(dock.marginEdge, noctalia::config::schema::kDockMarginEdgeRange, true), "gap inset"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "shape", tr("settings.schema.shared.corner-radius.label"),
          tr("settings.schema.dock.corner-radius.description"), path("radius"),
          sliderFor(dock.radius, noctalia::config::schema::kDockRadiusRange, true), "rounded"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "shape", tr("settings.schema.shared.corner-top-left.label"),
          tr("settings.schema.dock.corner-top-left.description"), path("radius_top_left"),
          sliderFor(dock.radiusTopLeft, noctalia::config::schema::kDockRadiusRange, true), "rounded corner", true
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "shape", tr("settings.schema.shared.corner-top-right.label"),
          tr("settings.schema.dock.corner-top-right.description"), path("radius_top_right"),
          sliderFor(dock.radiusTopRight, noctalia::config::schema::kDockRadiusRange, true), "rounded corner", true
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "shape", tr("settings.schema.shared.corner-bottom-left.label"),
          tr("settings.schema.dock.corner-bottom-left.description"), path("radius_bottom_left"),
          sliderFor(dock.radiusBottomLeft, noctalia::config::schema::kDockRadiusRange, true), "rounded corner", true
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "shape", tr("settings.schema.shared.corner-bottom-right.label"),
          tr("settings.schema.dock.corner-bottom-right.description"), path("radius_bottom_right"),
          sliderFor(dock.radiusBottomRight, noctalia::config::schema::kDockRadiusRange, true), "rounded corner", true
      ));
      {
        auto entry = makeEntry(
            SettingsSection::Dock, "shape", tr("settings.schema.dock.concave-edge-corners.label"),
            tr("settings.schema.dock.concave-edge-corners.description"), path("concave_edge_corners"),
            ToggleSetting{dock.concaveEdgeCorners}, "rounded corner carve"
        );
        entry.visibleWhen = [currentDock](const Config& current) { return currentDock(current).marginEdge == 0; };
        entries.push_back(std::move(entry));
      }
      entries.push_back(makeEntry(
          SettingsSection::Dock, "shape", tr("settings.schema.dock.border.label"),
          tr("settings.schema.dock.border.description"), path("border"), colorSpecPicker(dock.border), "outline color",
          true
      ));
      appendRow(entries, dockRows, "border_width");
      entries.push_back(makeEntry(
          SettingsSection::Dock, "effects", tr("settings.schema.shared.background-opacity.label"),
          tr("settings.schema.dock.background-opacity.description"), path("background_opacity"),
          sliderFor(dock.backgroundOpacity, noctalia::config::schema::kUnitRange, false),
          "alpha transparency translucent opacity"
      ));
      if (env.backgroundEffectBlurSupported) {
        entries.push_back(makeEntry(
            SettingsSection::Dock, "effects", tr("settings.schema.dock.compositor-blur.label"),
            tr("settings.schema.dock.compositor-blur.description"), path("compositor_blur"),
            ToggleSetting{dock.compositorBlur}, "blur background effect"
        ));
      }
      entries.push_back(makeEntry(
          SettingsSection::Dock, "effects", tr("settings.schema.shared.shadow.label"),
          tr("settings.schema.dock.shadow.description"), path("shadow"), ToggleSetting{dock.shadow}, "shadow"
      ));
      appendRow(entries, dockRows, "magnification");
      appendRow(entries, dockRows, "magnification_scale");
      appendGroup(entries, dockRows, "focus-styling");
      entries.push_back(makeEntry(
          SettingsSection::Dock, "pinned-apps", tr("settings.schema.dock.pinned-apps.label"),
          tr("settings.schema.dock.pinned-apps.description"), path("pinned"), ListSetting{.items = dock.pinned},
          "pinned apps dock"
      ));
    };

    addDockSettings(cfg.dock, {"dock"}, std::nullopt);
    for (const auto& override : cfg.dock.monitorOverrides) {
      addDockSettings(
          resolveDockMonitorOverride(cfg.dock, override), {"dock", "monitor", override.tableName}, override.tableName
      );
    }
  }

  void registry_detail::add_panels_settings(RegistryContext& ctx) {
    using i18n::tr;
    const auto& cfg = ctx.cfg;
    const auto& env = ctx.env;
    auto& entries = ctx.entries;
    auto& panelRows = ctx.panelRows;

    // Panels
    {
      SelectSetting anchorBarSelect;
      for (const auto& name : barNames(cfg)) {
        anchorBarSelect.options.push_back(SelectOption{name, name});
      }
      anchorBarSelect.selectedValue = cfg.shell.panelAnchorBar;
      anchorBarSelect.allowEmptySelection = true;
      entries.push_back(makeEntry(
          SettingsSection::Panels, "general", tr("settings.schema.panels.panel-anchor-bar.label"),
          tr("settings.schema.panels.panel-anchor-bar.description"), {"shell", "panel_anchor_bar"},
          std::move(anchorBarSelect), "anchor attach bar panel wallpaper launcher"
      ));
    }
    entries.push_back(makeEntry(
        SettingsSection::Panels, "general", tr("settings.schema.panels.floating-layer.label"),
        tr("settings.schema.panels.floating-layer.description"), {"shell", "panel", "floating_layer"},
        asSegmented(plainSelect(
            {{"top", "settings.options.layer.top"}, {"overlay", "settings.options.layer.overlay"}},
            cfg.shell.panel.floatingLayer
        )),
        "floating detached panel layer shell z-order input method popup fullscreen"
    ));
    panelRows =
        generateSchemaEntries(noctalia::config::schema::panelSchema(), cfg.shell.panel, {"shell", "panel"});
    appendRow(entries, panelRows, "transparency_mode");
    appendRow(entries, panelRows, "borders");
    entries.push_back(makeEntry(
        SettingsSection::Panels, "effects", tr("settings.schema.panels.border-color.label"),
        tr("settings.schema.panels.border-color.description"), {"shell", "panel", "border_color"},
        colorSpecPicker(cfg.shell.panel.borderColor), "outline border color theme accent"
    ));
    appendRow(entries, panelRows, "border_width");
    entries.push_back(makeEntry(
        SettingsSection::Panels, "effects", tr("settings.schema.shared.shadow.label"),
        tr("settings.schema.panels.shadow.description"), {"shell", "panel", "shadow"},
        ToggleSetting{cfg.shell.panel.shadow}, "shadow depth"
    ));
    appendGroup(entries, panelRows, "effects");
    // Position select for a panel. Floating pins it on screen; attached honors only
    // positions on the bar's edge (see shell::attachedPositionAlign).
    const auto panelPositionEntry = [&](SettingsSection section, std::string group, std::string_view panelKey,
                                        std::string_view labelKey, std::string_view descKey,
                                        const std::string& current) {
      return makeEntry(
          section, std::move(group), tr(labelKey), tr(descKey),
          {std::string("shell"), std::string("panel"), std::string(panelKey) + "_position"},
          plainSelect(
              {{"auto", "settings.options.panel-position.auto"},
               {"center", "settings.options.screen-position.center"},
               {"top_left", "settings.options.screen-position.top-left"},
               {"top_center", "settings.options.screen-position.top-center"},
               {"top_right", "settings.options.screen-position.top-right"},
               {"center_left", "settings.options.screen-position.center-left"},
               {"center_right", "settings.options.screen-position.center-right"},
               {"bottom_left", "settings.options.screen-position.bottom-left"},
               {"bottom_center", "settings.options.screen-position.bottom-center"},
               {"bottom_right", "settings.options.screen-position.bottom-right"}},
              current
          ),
          "panel position screen anchor corner edge floating attached align bottom right"
      );
    };

    // Click override: when on, a click on the panel's bar widget opens it near the widget
    // instead of at its Position.
    const auto panelOpenNearClickEntry = [](SettingsSection section, std::string group, std::string_view panelKey,
                                            std::string_view labelKey, std::string_view descKey, bool nearClick,
                                            PanelPlacement ShellConfig::PanelConfig::* placement,
                                            std::string ShellConfig::PanelConfig::* position) {
      auto e = makeEntry(
          section, std::move(group), tr(labelKey), tr(descKey),
          {"shell", "panel", std::string("open_near_click_") + std::string(panelKey)}, ToggleSetting{nearClick},
          "open near click trigger widget along bar position anchor"
      );
      e.visibleWhen = [placement, position](const Config& c) {
        const auto& panel = c.shell.panel;
        return panel.*placement == PanelPlacement::Attached
            || (panel.*placement == PanelPlacement::Floating && panel.*position == "auto");
      };
      return e;
    };

    appendRow(entries, panelRows, "launcher_placement");
    entries.push_back(panelPositionEntry(
        SettingsSection::Panels, "launcher", "launcher", "settings.schema.panels.position-launcher.label",
        "settings.schema.panels.position-launcher.description", cfg.shell.panel.launcherPosition
    ));
    entries.push_back(panelOpenNearClickEntry(
        SettingsSection::Panels, "launcher", "launcher", "settings.schema.panels.open-near-click-launcher.label",
        "settings.schema.panels.open-near-click-launcher.description", cfg.shell.panel.openNearClickLauncher,
        &ShellConfig::PanelConfig::launcherPlacement, &ShellConfig::PanelConfig::launcherPosition
    ));
    const SettingVisibility launcherFull = [](const Config& c) {
      return c.shell.launcher.layout == ShellConfig::LauncherLayout::Full;
    };
    const SettingVisibility launcherSimple = [](const Config& c) {
      return c.shell.launcher.layout == ShellConfig::LauncherLayout::Simple;
    };
    auto launcherRows = generateSchemaEntries(
        noctalia::config::schema::launcherSchema(), cfg.shell.launcher, {"shell", "launcher"}
    );
    appendRow(entries, launcherRows, "layout");
    {
      auto width = sliderFor(cfg.shell.launcher.width, noctalia::config::schema::kLauncherWidthRange, true);
      width.valueSuffix = "px";
      auto e = makeEntry(
          SettingsSection::Launcher, "launcher", tr("settings.schema.panels.launcher-width.label"),
          tr("settings.schema.panels.launcher-width.description"), {"shell", "launcher", "width"}, std::move(width),
          "launcher panel width size"
      );
      e.visibleWhen = launcherFull;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Launcher, "launcher", tr("settings.schema.panels.launcher-search-position.label"),
          tr("settings.schema.panels.launcher-search-position.description"), {"shell", "launcher", "search_position"},
          asSegmented(enumSelect(ShellConfig::kLauncherSearchPositions, cfg.shell.launcher.searchPosition)),
          "launcher search toolbar position top bottom"
      );
      e.visibleWhen = launcherFull;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Launcher, "launcher", tr("settings.schema.panels.launcher-default-view.label"),
          tr("settings.schema.panels.launcher-default-view.description"), {"shell", "launcher", "default_view"},
          enumSelect(ShellConfig::kLauncherDefaultViews, cfg.shell.launcher.defaultView),
          "launcher default view recent pinned all apps categories"
      );
      e.visibleWhen = launcherFull;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Launcher, "launcher", tr("settings.schema.panels.launcher-categories.label"),
          tr("settings.schema.panels.launcher-categories.description"), {"shell", "launcher", "categories"},
          ToggleSetting{cfg.shell.launcher.categories}, "launcher categories chips filter recent all"
      );
      e.visibleWhen = launcherSimple;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Launcher, "launcher", tr("settings.schema.panels.launcher-category-icons-only.label"),
          tr("settings.schema.panels.launcher-category-icons-only.description"),
          {"shell", "launcher", "category_icons_only"}, ToggleSetting{cfg.shell.launcher.categoryIconsOnly},
          "launcher app category icons labels text"
      );
      e.visibleWhen = [](const Config& c) {
        return c.shell.launcher.layout == ShellConfig::LauncherLayout::Full || c.shell.launcher.categories;
      };
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Launcher, "launcher", tr("settings.schema.panels.launcher-show-navigation-labels.label"),
          tr("settings.schema.panels.launcher-show-navigation-labels.description"),
          {"shell", "launcher", "show_navigation_labels"}, ToggleSetting{cfg.shell.launcher.showNavigationLabels},
          "launcher navigation tabs labels text recent pinned all categories providers icons"
      );
      e.visibleWhen = launcherFull;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Launcher, "launcher", tr("settings.schema.panels.launcher-show-settings-button.label"),
          tr("settings.schema.panels.launcher-show-settings-button.description"),
          {"shell", "launcher", "show_settings_button"}, ToggleSetting{cfg.shell.launcher.showSettingsButton},
          "launcher settings shortcut button"
      );
      e.visibleWhen = launcherFull;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Launcher, "launcher", tr("settings.schema.panels.launcher-show-session-button.label"),
          tr("settings.schema.panels.launcher-show-session-button.description"),
          {"shell", "launcher", "show_session_button"}, ToggleSetting{cfg.shell.launcher.showSessionButton},
          "launcher session power shortcut button"
      );
      e.visibleWhen = launcherFull;
      entries.push_back(std::move(e));
    }
    for (const auto* key : {"show_icons", "show_app_origin_indicator", "app_grid", "compact"}) {
      appendRow(entries, launcherRows, key);
    }
    appendGroup(entries, launcherRows, "behavior");
    appendGroup(entries, launcherRows, "history");
    entries.push_back(makeEntry(
        SettingsSection::Launcher, "history", tr("settings.schema.panels.launcher-sort-by-usage.label"),
        tr("settings.schema.panels.launcher-sort-by-usage.description"), {"shell", "launcher", "sort_by_usage"},
        ToggleSetting{cfg.shell.launcher.sortByUsage, cfg.shell.launcher.trackUsage},
        "launcher sort usage recently used frequency"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Launcher, "pinned-apps", tr("settings.schema.panels.launcher-pinned-apps.label"),
        tr("settings.schema.panels.launcher-pinned-apps.description"), {"shell", "launcher", "pinned"},
        ListSetting{.items = cfg.shell.launcher.pinned}, "launcher pinned apps"
    ));
    appendGroup(entries, launcherRows, "providers");
    for (const auto& provider : kLauncherProviderSettings) {
      const auto it = std::ranges::find(cfg.shell.launcher.providers, provider.name, &LauncherProviderConfig::name);
      const LauncherProviderConfig* stored = it != cfg.shell.launcher.providers.end() ? &*it : nullptr;

      const std::string prefix = stored != nullptr ? stored->prefix : std::string();
      const bool global =
          stored != nullptr ? stored->global.value_or(provider.globalByDefault) : provider.globalByDefault;

      const std::string prefixKey = std::format("settings.schema.panels.launcher-prefix-{}", provider.name);
      const std::string globalKey = std::format("settings.schema.panels.launcher-global-{}", provider.name);

      entries.push_back(makeEntry(
          SettingsSection::Launcher, "providers", tr(prefixKey + ".label"), tr(prefixKey + ".description"),
          {"shell", "launcher", "providers", std::string(provider.name), "prefix"},
          TextSetting{.value = prefix, .placeholder = std::string(provider.prefixPlaceholder)},
          std::format("launcher {} prefix trigger", provider.name)
      ));
      entries.push_back(makeEntry(
          SettingsSection::Launcher, "providers", tr(globalKey + ".label"), tr(globalKey + ".description"),
          {"shell", "launcher", "providers", std::string(provider.name), "global"}, ToggleSetting{global},
          std::format("launcher {} global search unprefixed", provider.name)
      ));
      if (provider.name == "panels") {
        ListSetting ignoredPanels;
        ignoredPanels.items = cfg.shell.launcher.panels.ignored;
        const auto knownPanelIds = panel_catalog::allKnownIds();
        ignoredPanels.suggestedOptions.reserve(knownPanelIds.size());
        for (const auto& panelId : knownPanelIds) {
          ignoredPanels.suggestedOptions.push_back(SelectOption{panelId, panel_catalog::describe(panelId).title});
        }
        entries.push_back(makeEntry(
            SettingsSection::Launcher, "providers", tr("settings.schema.panels.launcher-ignored-panels.label"),
            tr("settings.schema.panels.launcher-ignored-panels.description"),
            {"shell", "launcher", "panels", "ignored"}, std::move(ignoredPanels),
            "launcher panels ignore hide exclude noise polkit setup wizard test"
        ));
      }
    }
    appendRow(entries, panelRows, "clipboard_placement");
    entries.push_back(panelPositionEntry(
        SettingsSection::Panels, "clipboard", "clipboard", "settings.schema.panels.position-clipboard.label",
        "settings.schema.panels.position-clipboard.description", cfg.shell.panel.clipboardPosition
    ));
    entries.push_back(panelOpenNearClickEntry(
        SettingsSection::Panels, "clipboard", "clipboard", "settings.schema.panels.open-near-click-clipboard.label",
        "settings.schema.panels.open-near-click-clipboard.description", cfg.shell.panel.openNearClickClipboard,
        &ShellConfig::PanelConfig::clipboardPlacement, &ShellConfig::PanelConfig::clipboardPosition
    ));
    appendRow(entries, panelRows, "polkit_placement");
    entries.push_back(panelPositionEntry(
        SettingsSection::Panels, "polkit", "polkit", "settings.schema.panels.position-polkit.label",
        "settings.schema.panels.position-polkit.description", cfg.shell.panel.polkitPosition
    ));
    appendGroup(entries, panelRows, "wallpaper");
    entries.push_back(panelPositionEntry(
        SettingsSection::Panels, "wallpaper", "wallpaper", "settings.schema.panels.position-wallpaper.label",
        "settings.schema.panels.position-wallpaper.description", cfg.shell.panel.wallpaperPosition
    ));
    entries.push_back(panelOpenNearClickEntry(
        SettingsSection::Panels, "wallpaper", "wallpaper", "settings.schema.panels.open-near-click-wallpaper.label",
        "settings.schema.panels.open-near-click-wallpaper.description", cfg.shell.panel.openNearClickWallpaper,
        &ShellConfig::PanelConfig::wallpaperPlacement, &ShellConfig::PanelConfig::wallpaperPosition
    ));
    ctx.panelPositionEntry = panelPositionEntry;
    ctx.panelOpenNearClickEntry = panelOpenNearClickEntry;
  }

  void registry_detail::add_controlcenter_settings(RegistryContext& ctx) {
    using i18n::tr;
    const auto& cfg = ctx.cfg;
    const auto& env = ctx.env;
    auto& entries = ctx.entries;
    auto& panelRows = ctx.panelRows;
    const auto& panelPositionEntry = ctx.panelPositionEntry;
    const auto& panelOpenNearClickEntry = ctx.panelOpenNearClickEntry;

    // Control Center
    appendRow(entries, panelRows, "control_center_placement");
    entries.push_back(panelPositionEntry(
        SettingsSection::Panels, "control-center", "control_center",
        "settings.schema.panels.position-control-center.label",
        "settings.schema.panels.position-control-center.description", cfg.shell.panel.controlCenterPosition
    ));
    entries.push_back(panelOpenNearClickEntry(
        SettingsSection::Panels, "control-center", "control_center",
        "settings.schema.panels.open-near-click-control-center.label",
        "settings.schema.panels.open-near-click-control-center.description", cfg.shell.panel.openNearClickControlCenter,
        &ShellConfig::PanelConfig::controlCenterPlacement, &ShellConfig::PanelConfig::controlCenterPosition
    ));
    auto controlCenterRows =
        generateSchemaEntries(noctalia::config::schema::controlCenterSchema(), cfg.controlCenter, {"control_center"});
    appendGroup(entries, controlCenterRows, "layout");
    appendGroup(entries, controlCenterRows, "navigation");
    {
      MultiSelectSetting tabs;
      const auto catalog = ControlCenterPanel::hideableTabCatalog();
      tabs.options.reserve(catalog.size());
      tabs.selectedValues.reserve(catalog.size());
      for (const auto& tab : catalog) {
        tabs.options.push_back(SelectOption{std::string(tab.key), tr(tab.titleKey)});
        if (!std::ranges::contains(cfg.controlCenter.hiddenTabs, tab.key)) {
          tabs.selectedValues.emplace_back(tab.key);
        }
      }
      tabs.persistUnselected = true;
      entries.push_back(makeEntry(
          SettingsSection::ControlCenter, "navigation", tr("settings.schema.panels.control-center-tabs.label"),
          tr("settings.schema.panels.control-center-tabs.description"), {"control_center", "hidden_tabs"},
          std::move(tabs), "tabs sections visible hide show display brightness media audio network power"
      ));
    }
    entries.push_back(makeEntry(
        SettingsSection::ControlCenter, "home", tr("settings.schema.panels.home-shortcuts.label"),
        tr("settings.schema.panels.home-shortcuts.description"), {"control_center", "shortcuts"},
        ShortcutListSetting{
            .items = cfg.controlCenter.shortcuts, .suggestedOptions = controlCenterShortcutOptions(cfg), .maxItems = 6
        },
        "quick settings shortcuts toggles wifi bluetooth caffeine night light dnd power media weather clipboard"
    ));
    appendGroup(entries, controlCenterRows, "home");
  }

  void registry_detail::add_desktop_settings(RegistryContext& ctx) {
    using i18n::tr;
    const auto& cfg = ctx.cfg;
    const auto& env = ctx.env;
    auto& entries = ctx.entries;

    // Desktop
    auto desktopWidgetRows = generateSchemaEntries(
        noctalia::config::schema::desktopWidgetsSchema(), cfg.desktopWidgets, {"desktop_widgets"}
    );
    appendGroup(entries, desktopWidgetRows, "widgets");
    auto screenCornerRows = generateSchemaEntries(
        noctalia::config::schema::screenCornersSchema(), cfg.shell.screenCorners, {"shell", "screen_corners"}
    );
    appendGroup(entries, screenCornerRows, "screen-corners");

    // On Andromeda the compositor owns hot corners; the shell's were moved to it (HotCorners::migrateToCompositor).
    if (env.compositorSettings == nullptr) {
      entries.push_back(makeEntry(
          SettingsSection::Windows, "hot-corners", tr("settings.schema.desktop.hot-corners-enabled.label"),
          tr("settings.schema.desktop.hot-corners-enabled.description"), {"hot_corners", "enabled"},
          ToggleSetting{cfg.hotCorners.enabled}, "hot corners trigger mouse edge screen"
      ));
      {
        auto delay = sliderFor(
            static_cast<std::int64_t>(cfg.hotCorners.delayMs), noctalia::config::schema::kHotCornersDelayMsRange, true
        );
        delay.valueSuffix = "ms";
        SettingEntry e = makeEntry(
            SettingsSection::Windows, "hot-corners", tr("settings.schema.desktop.hot-corners-delay-ms.label"),
            tr("settings.schema.desktop.hot-corners-delay-ms.description"), {"hot_corners", "delay_ms"},
            std::move(delay), "hot corners delay hold ms timeout"
        );
        e.visibleWhen = [](const Config& conf) { return conf.hotCorners.enabled; };
        entries.push_back(std::move(e));
      }

      auto hotCornerActionSelect = [](const std::string& current) {
        return plainSelect(
            {{"none", "settings.options.hot-corners.none"},
             {"launcher", "settings.options.hot-corners.launcher"},
             {"control_center", "settings.options.hot-corners.control-center"},
             {"window_switcher", "settings.options.hot-corners.window-switcher"},
             {"command", "settings.options.hot-corners.command"}},
            current
        );
      };

      auto addCornerEntry = [&](const std::string& key, const std::string& labelKey, const std::string& currentAction,
                                const std::string& currentCommand) {
        SettingEntry e = makeEntry(
            SettingsSection::Windows, "hot-corners", tr(labelKey + ".label"), tr(labelKey + ".description"),
            {"hot_corners", key, "action"}, hotCornerActionSelect(currentAction), "hot corners " + key
        );
        e.visibleWhen = [](const Config& conf) { return conf.hotCorners.enabled; };
        entries.push_back(std::move(e));

        SettingEntry c = makeEntry(
            SettingsSection::Windows, "hot-corners", tr(labelKey + "-command.label"),
            tr(labelKey + "-command.description"), {"hot_corners", key, "command"},
            TextSetting{.value = currentCommand, .placeholder = "Run command..."}, "hot corners command execute " + key
        );
        c.visibleWhen = [key](const Config& conf) {
          if (!conf.hotCorners.enabled) {
            return false;
          }
          const HotCornersConfig::Corner* corner = nullptr;
          if (key == "top_left") {
            corner = &conf.hotCorners.topLeft;
          } else if (key == "top_right") {
            corner = &conf.hotCorners.topRight;
          } else if (key == "bottom_left") {
            corner = &conf.hotCorners.bottomLeft;
          } else if (key == "bottom_right") {
            corner = &conf.hotCorners.bottomRight;
          }
          return corner != nullptr && corner->action == "command";
        };
        entries.push_back(std::move(c));
      };

      addCornerEntry(
          "top_left", "settings.schema.desktop.hot-corners-top-left", cfg.hotCorners.topLeft.action,
          cfg.hotCorners.topLeft.command
      );
      addCornerEntry(
          "top_right", "settings.schema.desktop.hot-corners-top-right", cfg.hotCorners.topRight.action,
          cfg.hotCorners.topRight.command
      );
      addCornerEntry(
          "bottom_left", "settings.schema.desktop.hot-corners-bottom-left", cfg.hotCorners.bottomLeft.action,
          cfg.hotCorners.bottomLeft.command
      );
      addCornerEntry(
          "bottom_right", "settings.schema.desktop.hot-corners-bottom-right", cfg.hotCorners.bottomRight.action,
          cfg.hotCorners.bottomRight.command
      );
    }
  }

} // namespace settings
