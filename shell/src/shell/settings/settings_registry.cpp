#include "shell/settings/settings_registry.h"
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
  namespace {

    constexpr int kBarMarginMax = 4096;
    constexpr float kBarCornerRadiusMax = 80.0F;

    // Launcher providers that expose a configurable prefix, keyed by the config name
    // (the lowercased provider id). Placeholders and global-search defaults mirror the
    // provider's own defaultPrefix()/defaultIncludeInGlobalSearch().
    struct LauncherProviderSettingSpec {
      std::string_view name;
      std::string_view prefixPlaceholder;
      bool globalByDefault = false;
    };

    constexpr auto kLauncherProviderSettings = std::to_array<LauncherProviderSettingSpec>({
        {.name = "calculator", .prefixPlaceholder = "calc", .globalByDefault = true},
        {.name = "emoji", .prefixPlaceholder = "emo"},
        {.name = "panels", .prefixPlaceholder = "pan"},
        {.name = "session", .prefixPlaceholder = "session"},
        {.name = "wallpaper", .prefixPlaceholder = "wall"},
        {.name = "windows", .prefixPlaceholder = "win"},
    });

    [[nodiscard]] SliderSetting barCornerSlider(std::int32_t value) {
      SliderSetting s{value, 0.0F, kBarCornerRadiusMax, 1.0F, true};
      return s;
    }

    // What the dead zone does for a gesture nobody has bound, shown as the row's placeholder so an
    // empty field never reads as "does nothing".
    [[nodiscard]] std::string deadZoneDefault(noctalia::bar::Gesture gesture) {
      for (const auto& binding : noctalia::bar::deadZoneGestureDefaults()) {
        if (binding.gesture == gesture) {
          return std::string(binding.action);
        }
      }
      return {};
    }

    [[nodiscard]] SliderSetting barReservedSlider(double value, double maxValue, double step, bool integer) {
      return SliderSetting{value, 0.0F, maxValue, step, integer};
    }

    [[nodiscard]] std::vector<KeyChord>
    effectiveKeybindItems(const std::vector<KeyChord>& configured, KeybindAction action) {
      if (!configured.empty()) {
        return configured;
      }
      return defaultKeybindSet(action);
    }

    constexpr std::array<SettingsSectionDescriptor, 34> kSettingsSections{{
        {SettingsSection::Appearance, SettingsCategory::Appearance, "appearance", "palette"},
        {SettingsSection::WindowStyle, SettingsCategory::Appearance, "window-style", "app-window"},
        {SettingsSection::Wallpaper, SettingsCategory::Appearance, "wallpaper", "paint"},
        {SettingsSection::TextScale, SettingsCategory::Appearance, "text-scale", "typography"},
        {SettingsSection::Motion, SettingsCategory::Appearance, "motion", "sparkles"},
        {SettingsSection::Bar, SettingsCategory::Desktop, "bar", "crop-3-2", false},
        {SettingsSection::Dock, SettingsCategory::Desktop, "dock", "layout-bottombar-inactive"},
        {SettingsSection::Panels, SettingsCategory::Desktop, "panels", "layout-bottombar"},
        {SettingsSection::Launcher, SettingsCategory::Desktop, "launcher", "rocket"},
        {SettingsSection::ControlCenter, SettingsCategory::Desktop, "control-center", "adjustments"},
        {SettingsSection::Notifications, SettingsCategory::Desktop, "notifications", "bell"},
        {SettingsSection::Desktop, SettingsCategory::Desktop, "desktop", "layout-board"},
        {SettingsSection::Layout, SettingsCategory::Windows, "layout", "layout-grid"},
        {SettingsSection::Overview, SettingsCategory::Windows, "overview", "layout-dashboard"},
        {SettingsSection::Windows, SettingsCategory::Windows, "windows", "pointer"},
        {SettingsSection::Niri, SettingsCategory::Windows, "niri", "niri"},
        {SettingsSection::Displays, SettingsCategory::Pinned, "displays", "device-desktop", true, true},
        {SettingsSection::Input, SettingsCategory::Devices, "input", "keyboard", true, true},
        {SettingsSection::Devices, SettingsCategory::Devices, "devices", "device-mobile", true, true},
        {SettingsSection::Shortcuts, SettingsCategory::Shortcuts, "shortcuts", "command", true, true},
        {SettingsSection::Keybinds, SettingsCategory::Shortcuts, "keybinds", "keyboard"},
        {SettingsSection::Services, SettingsCategory::Pinned, "services", "volume"},
        {SettingsSection::DateTime, SettingsCategory::System, "date-time", "clock", true, true},
        {SettingsSection::Language, SettingsCategory::System, "language", "language", true, true},
        {SettingsSection::DefaultApps, SettingsCategory::System, "default-apps", "apps", true, true},
        {SettingsSection::Power, SettingsCategory::Pinned, "power", "bolt"},
        {SettingsSection::Security, SettingsCategory::System, "security", "shield-lock"},
        {SettingsSection::Location, SettingsCategory::System, "location", "map-pin"},
        {SettingsSection::Calendar, SettingsCategory::System, "calendar", "calendar"},
        {SettingsSection::Screenshot, SettingsCategory::System, "screenshot", "screenshot"},
        {SettingsSection::System, SettingsCategory::System, "system", "activity-heartbeat"},
        {SettingsSection::Shell, SettingsCategory::System, "shell", "adjustments-horizontal"},
        {SettingsSection::Hooks, SettingsCategory::Advanced, "hooks", "link"},
        {SettingsSection::Plugins, SettingsCategory::Advanced, "plugins", "puzzle", true, true},
    }};

    struct SettingsCategoryDescriptor {
      SettingsCategory category;
      std::string_view id;
      std::string_view glyph;
    };

    constexpr std::array<SettingsCategoryDescriptor, 8> kSettingsCategories{{
        {SettingsCategory::Pinned, "pinned", ""},
        {SettingsCategory::Appearance, "appearance", "brush"},
        {SettingsCategory::Desktop, "desktop", "layout-2"},
        {SettingsCategory::Windows, "windows", "window"},
        {SettingsCategory::Devices, "devices", "devices"},
        {SettingsCategory::Shortcuts, "shortcuts", "keyboard-show"},
        {SettingsCategory::System, "system", "settings"},
        {SettingsCategory::Advanced, "advanced", "tool"},
    }};

    constexpr std::array<std::pair<std::string_view, std::string_view>, 3> kMergedSectionIds{{
        {"templates", "appearance"},
        {"osd", "notifications"},
        {"andromeda", "overview"},
    }};

    const SettingsSectionDescriptor& descriptorFor(SettingsSection section) {
      const auto* it = std::ranges::find(kSettingsSections, section, &SettingsSectionDescriptor::section);
      if (it == kSettingsSections.end()) {
        std::abort();
      }
      return *it;
    }

    // Builds a slider whose bounds come from the shared schema Range, the same
    // constant the parser clamps with. This keeps the UI range and the config clamp
    // one source. `integerValue` (write as int64) stays explicit: it is a UI/write
    // choice, not implied by the range's numeric type (e.g. transition_duration).
    template <typename V, typename T>
    SliderSetting sliderFor(V value, const noctalia::config::schema::Range<T>& range, bool integerValue) {
      return SliderSetting{
          static_cast<double>(value), static_cast<double>(range.min.value()), static_cast<double>(range.max.value()),
          static_cast<double>(range.step.value()), integerValue
      };
    }

    SelectSetting asSegmented(SelectSetting setting) {
      setting.segmented = true;
      return setting;
    }

    StepperSetting barMarginStepper(std::int32_t value) {
      return StepperSetting{
          .value = static_cast<int>(value),
          .minValue = 0,
          .maxValue = kBarMarginMax,
          .step = 1,
          .valueSuffix = "px",
      };
    }

    StepperSetting barPanelOverlapStepper(std::int32_t value) {
      return StepperSetting{
          .value = static_cast<int>(value),
          .minValue = -2,
          .maxValue = 3,
          .step = 1,
          .valueSuffix = "px",
      };
    }

    std::optional<int> radiusStepperValue(const std::optional<double>& value) {
      if (!value.has_value()) {
        return std::nullopt;
      }
      return std::clamp(static_cast<int>(std::lround(*value)), 0, 80);
    }

    int radiusStepperFallback(const std::optional<double>& value) { return radiusStepperValue(value).value_or(8); }

    template <typename T, std::size_t N> SelectSetting enumSelect(const EnumOption<T> (&options)[N], T selected) {
      std::vector<SelectOption> opts;
      opts.reserve(N);
      std::string selectedValue;
      for (const auto& option : options) {
        std::string key(option.key);
        if (option.value == selected) {
          selectedValue = key;
        }
        opts.push_back(SelectOption{std::move(key), i18n::tr(option.labelKey)});
      }
      if (selectedValue.empty() && N > 0) {
        selectedValue = std::string(options[0].key);
      }
      return SelectSetting{std::move(opts), std::move(selectedValue)};
    }

    SelectSetting
    plainSelect(std::initializer_list<std::pair<std::string_view, std::string_view>> items, std::string_view selected) {
      std::vector<SelectOption> opts;
      opts.reserve(items.size());
      for (const auto& [value, labelKey] : items) {
        opts.push_back(SelectOption{std::string(value), i18n::tr(labelKey)});
      }
      return SelectSetting{std::move(opts), std::string(selected)};
    }

    [[nodiscard]] std::string_view barAutoHideMode(bool autoHide, bool smartAutoHide) {
      if (smartAutoHide) {
        return "smart";
      }
      if (autoHide) {
        return "on";
      }
      return "off";
    }

    [[nodiscard]] SelectSetting autoHideModeSelect(std::string_view mode, std::vector<std::string> smartPath) {
      auto select = asSegmented(plainSelect(
          {{"off", "settings.options.bar.auto-hide.off"},
           {"on", "settings.options.bar.auto-hide.on"},
           {"smart", "settings.options.bar.auto-hide.smart"}},
          mode
      ));
      select.linkedPath = std::move(smartPath);
      select.groupedCommit = [](std::string_view value, const std::vector<std::string>& primaryPath) {
        auto companionPath = primaryPath;
        companionPath.back() = "smart_auto_hide";
        return std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>{
            {primaryPath, ConfigOverrideValue{value == "on"}},
            {std::move(companionPath), ConfigOverrideValue{value == "smart"}},
        };
      };
      return select;
    }

    ColorSwatchPreview palettePreviewFromPalette(const noctalia::theme::FixedPalette& palette) {
      return ColorSwatchPreview{
          .surface = fixedColorSpec(palette.surface),
          .swatches = {
              fixedColorSpec(palette.primary),
              fixedColorSpec(palette.secondary),
              fixedColorSpec(palette.tertiary),
              fixedColorSpec(palette.error),
          },
      };
    }

    ColorSwatchPreview builtinPalettePreview(const noctalia::theme::BuiltinPalette& palette, ThemeMode mode) {
      return palettePreviewFromPalette(mode == ThemeMode::Light ? palette.light.palette : palette.dark.palette);
    }

    SelectSetting builtinPaletteSelect(std::string_view selected, ThemeMode mode) {
      std::vector<SelectOption> opts;
      opts.reserve(noctalia::theme::builtinPalettes().size());
      for (const auto& palette : noctalia::theme::builtinPalettes()) {
        opts.push_back(
            SelectOption{
                .value = std::string(palette.name),
                .label = std::string(palette.name),
                .description = {},
                .preview = builtinPalettePreview(palette, mode),
            }
        );
      }
      return SelectSetting{
          .options = std::move(opts), .selectedValue = std::string(selected), .preferredWidth = 240.0F
      };
    }

    SelectSetting wallpaperSchemeSelect(std::string_view selected) {
      return plainSelect(
          {{"m3-content", "theme.scheme.m3-content"},
           {"m3-tonal-spot", "theme.scheme.m3-tonal-spot"},
           {"m3-fruit-salad", "theme.scheme.m3-fruit-salad"},
           {"m3-rainbow", "theme.scheme.m3-rainbow"},
           {"m3-monochrome", "theme.scheme.m3-monochrome"},
           {"vibrant", "theme.scheme.vibrant"},
           {"faithful", "theme.scheme.faithful"},
           {"soft", "theme.scheme.soft"},
           {"dysfunctional", "theme.scheme.dysfunctional"},
           {"muted", "theme.scheme.muted"}},
          selected
      );
    }

    std::vector<SelectOption> controlCenterShortcutOptions(const Config& cfg) {
      auto catalog = ShortcutRegistry::catalog();
      std::vector<SelectOption> opts;
      opts.reserve(catalog.size());
      for (auto& shortcut : catalog) {
        if (!ShortcutRegistry::isAvailable(shortcut.type, cfg)) {
          continue;
        }
        std::string label = shortcut.literalLabel ? std::move(shortcut.labelKey) : i18n::tr(shortcut.labelKey);
        opts.push_back(SelectOption{std::move(shortcut.type), std::move(label)});
      }
      return opts;
    }

    SelectSetting languageSelect(std::string_view selected) {
      std::vector<SelectOption> opts;
      opts.reserve(i18n::kSupportedLanguages.size() + 1);
      opts.push_back(SelectOption{"", i18n::tr("common.states.auto")});
      for (const auto& language : i18n::kSupportedLanguages) {
        opts.push_back(SelectOption{std::string(language.code), std::string(language.displayName)});
      }
      return SelectSetting{std::move(opts), std::string(selected)};
    }

    ColorSpecPickerSetting
    colorSpecPicker(const std::optional<ColorSpec>& selected, bool allowNone, std::string noneLabel = {}) {
      return ColorSpecPickerSetting{
          .roles = {},
          .selectedValue = optionalColorSpecConfigValue(selected),
          .allowNone = allowNone,
          .allowCustomColor = true,
          .noneLabel = std::move(noneLabel),
      };
    }

    ColorSpecPickerSetting
    colorSpecPicker(const ColorSpec& selected, bool allowNone = false, std::string noneLabel = {}) {
      return colorSpecPicker(std::optional<ColorSpec>{selected}, allowNone, std::move(noneLabel));
    }

    std::string pathText(const std::vector<std::string>& path) {
      std::string out;
      for (const auto& part : path) {
        if (!out.empty()) {
          out.push_back('.');
        }
        out += part;
      }
      return out;
    }

    SettingEntry makeEntry(
        SettingsSection section, std::string group, std::string title, std::string subtitle,
        std::vector<std::string> path, SettingControl control, const std::string& tags = {}, bool advanced = false
    ) {
      std::string searchText = std::string(settingsSectionId(section))
          + " "
          + group
          + " "
          + title
          + " "
          + subtitle
          + " "
          + pathText(path)
          + " "
          + tags;
      if (advanced) {
        searchText += " advanced";
      }
      return SettingEntry{
          .section = section,
          .group = std::move(group),
          .title = std::move(title),
          .subtitle = std::move(subtitle),
          .path = std::move(path),
          .control = std::move(control),
          .advanced = advanced,
          .searchText = StringUtils::toLower(searchText),
      };
    }

    // Rows for the fields of `schema` that have a RowSpec, in schema order. The current value is read back
    // through the field's own write(), so a row shows exactly what the config file would hold.
    template <typename Struct>
    std::vector<SettingEntry> generateSchemaEntries(
        const noctalia::config::schema::Schema<Struct>& schema, const Struct& values,
        const std::vector<std::string>& prefix
    ) {
      using noctalia::config::schema::RowSpec;
      std::vector<SettingEntry> entries;
      for (const auto& field : schema) {
        if (!field.row) {
          continue;
        }
        const RowSpec& row = *field.row;
        toml::table written;
        field.write(written, values);
        const auto value = std::as_const(written)[field.key];
        SettingControl control;
        switch (row.kind) {
        case RowSpec::Kind::Toggle:
          control = ToggleSetting{value.value_or(false)};
          break;
        case RowSpec::Kind::Slider: {
          SliderSetting slider{
              noctalia::config::schema::finiteDouble(value).value_or(0.0), row.range.min.value(),
              row.range.max.value(), row.range.step.value(), row.integer
          };
          slider.valueSuffix = row.suffix;
          control = std::move(slider);
          break;
        }
        case RowSpec::Kind::Select: {
          SelectSetting select;
          for (const auto& choice : row.choices) {
            select.options.push_back(SelectOption{std::string(choice.value), i18n::tr(choice.labelKey)});
          }
          select.selectedValue = value.value_or(std::string{});
          select.segmented = row.segmented;
          select.preferredWidth = row.width;
          control = std::move(select);
          break;
        }
        case RowSpec::Kind::Text:
          control = TextSetting{
              .value = value.value_or(std::string{}),
              .placeholder = row.placeholderKey.empty() ? std::string(row.placeholder) : i18n::tr(row.placeholderKey),
              .width = row.width,
              .browseMode = row.browse == RowSpec::Browse::Folder ? TextSettingBrowseMode::SelectFolder
                  : row.browse == RowSpec::Browse::File           ? TextSettingBrowseMode::OpenFile
                                                                  : TextSettingBrowseMode::None,
          };
          break;
        case RowSpec::Kind::Stepper:
          control = StepperSetting{
              .value = static_cast<int>(value.value_or(std::int64_t{0})),
              .minValue = static_cast<int>(row.range.min.value()),
              .maxValue = static_cast<int>(row.range.max.value()),
              .step = static_cast<int>(row.range.step.value_or(1.0)),
              .valueSuffix = std::string(row.suffix),
          };
          break;
        }
        std::vector<std::string> path = prefix;
        path.emplace_back(field.key);
        const std::string label = row.label.empty() ? std::string(row.section) + "." + i18n::keySegment(field.key)
                                                    : std::string(row.label);
        entries.push_back(makeEntry(
            settingsSectionFromId(row.section).value(), std::string(row.group),
            i18n::tr("settings.schema." + label + ".label"), i18n::tr("settings.schema." + label + ".description"),
            std::move(path), std::move(control), std::string(row.tags), row.advanced
        ));
      }
      return entries;
    }

    // Moves the rows of `group` from `generated` to the end of `entries`, in order. Each call site sits where
    // the group's hand-written rows were, so custom rows of the same group keep their place.
    void appendGroup(std::vector<SettingEntry>& entries, std::vector<SettingEntry>& generated, std::string_view group) {
      std::vector<SettingEntry> rest;
      for (auto& entry : generated) {
        (entry.group == group ? entries : rest).push_back(std::move(entry));
      }
      generated = std::move(rest);
    }

    // Moves the row of config key `key` from `generated` to the end of `entries`. For rows whose page order
    // differs from the schema order, or that sit between hand-written rows of their group.
    void appendRow(std::vector<SettingEntry>& entries, std::vector<SettingEntry>& generated, std::string_view key) {
      const auto it = std::ranges::find_if(generated, [key](const SettingEntry& e) { return e.path.back() == key; });
      // A key without a RowSpec has no row; settings_rows_generated reports it as missing.
      if (it == generated.end()) {
        return;
      }
      entries.push_back(std::move(*it));
      generated.erase(it);
    }

  } // namespace

  const BarConfig* findBar(const Config& cfg, std::string_view name) {
    for (const auto& bar : cfg.bars) {
      if (bar.name == name) {
        return &bar;
      }
    }
    return nullptr;
  }

  const BarMonitorOverride* findMonitorOverride(const BarConfig& bar, std::string_view match) {
    for (const auto& ovr : bar.monitorOverrides) {
      if (ovr.match == match) {
        return &ovr;
      }
    }
    return nullptr;
  }

  const DockMonitorOverride* findDockMonitorOverride(const DockConfig& dock, std::string_view tableName) {
    const auto it = std::ranges::find(dock.monitorOverrides, tableName, &DockMonitorOverride::tableName);
    return it != dock.monitorOverrides.end() ? &*it : nullptr;
  }

  std::vector<std::string> barNames(const Config& cfg) {
    std::vector<std::string> names;
    names.reserve(cfg.bars.size());
    for (const auto& bar : cfg.bars) {
      names.push_back(bar.name);
    }
    return names;
  }

  std::string normalizedSettingQuery(std::string_view query) { return StringUtils::toLower(query); }

  bool matchesNormalizedSettingQuery(const SettingEntry& entry, std::string_view normalizedQuery) {
    if (normalizedQuery.empty()) {
      return true;
    }
    return entry.searchText.contains(normalizedQuery);
  }

  bool matchesSettingQuery(const SettingEntry& entry, std::string_view query) {
    return matchesNormalizedSettingQuery(entry, normalizedSettingQuery(query));
  }

  bool isBarMonitorOverrideSettingPath(const std::vector<std::string>& path) {
    return path.size() >= 5 && path[0] == "bar" && path[2] == "monitor";
  }

  bool isDockMonitorOverrideSettingPath(const std::vector<std::string>& path) {
    return path.size() >= 4 && path[0] == "dock" && path[1] == "monitor";
  }

  bool settingEntryMatchesBarNavigation(
      const SettingEntry& entry, std::string_view selectedBarName, std::string_view selectedMonitorOverride
  ) {
    if (entry.section != SettingsSection::Bar || entry.path.size() < 2 || entry.path[0] != "bar") {
      return false;
    }
    if (selectedBarName.empty() || entry.path[1] != selectedBarName) {
      return false;
    }
    const bool monitorEntry = isBarMonitorOverrideSettingPath(entry.path);
    if (selectedMonitorOverride.empty()) {
      return !monitorEntry;
    }
    return monitorEntry && entry.path[3] == selectedMonitorOverride;
  }

  bool settingEntryMatchesDockNavigation(const SettingEntry& entry, std::string_view selectedMonitorOverride) {
    if (entry.section != SettingsSection::Dock || entry.path.empty() || entry.path[0] != "dock") {
      return false;
    }
    const bool monitorEntry = isDockMonitorOverrideSettingPath(entry.path);
    if (selectedMonitorOverride.empty()) {
      return !monitorEntry;
    }
    return monitorEntry && entry.path[2] == selectedMonitorOverride;
  }

  std::string barSettingContentSectionKey(const SettingEntry& entry) {
    if (entry.section == SettingsSection::Dock) {
      std::string key = "dock";
      if (isDockMonitorOverrideSettingPath(entry.path)) {
        key += ":monitor:" + entry.path[2];
      }
      return key;
    }
    if (entry.section != SettingsSection::Bar || entry.path.size() < 2) {
      return std::string(settingsSectionId(entry.section));
    }
    std::string key = "bar:" + entry.path[1];
    if (isBarMonitorOverrideSettingPath(entry.path)) {
      key += ":monitor:" + entry.path[3];
    }
    return key;
  }

  std::span<const SettingsSectionDescriptor> settingsSectionDescriptors() { return kSettingsSections; }

  std::string_view settingsSectionId(SettingsSection section) { return descriptorFor(section).id; }

  std::string settingsSectionLabelKey(SettingsSection section) {
    return "settings.navigation.sections." + std::string(settingsSectionId(section));
  }

  std::string_view sectionGlyph(SettingsSection section) { return descriptorFor(section).glyph; }

  std::optional<SettingsSection> settingsSectionFromId(std::string_view id) {
    const auto* it = std::ranges::find(kSettingsSections, id, &SettingsSectionDescriptor::id);
    if (it == kSettingsSections.end()) {
      return std::nullopt;
    }
    return it->section;
  }

  std::string canonicalSettingsSectionId(std::string_view id) {
    const auto it = std::ranges::find(kMergedSectionIds, id, &std::pair<std::string_view, std::string_view>::first);
    return std::string(it != kMergedSectionIds.end() ? it->second : id);
  }

  SettingsCategory settingsSectionCategory(SettingsSection section) { return descriptorFor(section).category; }

  std::string_view settingsCategoryId(SettingsCategory category) {
    return std::ranges::find(kSettingsCategories, category, &SettingsCategoryDescriptor::category)->id;
  }

  std::string_view settingsCategoryGlyph(SettingsCategory category) {
    return std::ranges::find(kSettingsCategories, category, &SettingsCategoryDescriptor::category)->glyph;
  }

  namespace {

    constexpr std::string_view kCompositorPathRoot = "andromeda";

    std::vector<std::string> compositorPath(std::string_view key) {
      std::vector<std::string> path{std::string(kCompositorPathRoot)};
      for (const auto part : std::views::split(key, '.')) {
        path.emplace_back(std::string_view(part));
      }
      return path;
    }

    // "appearance.blur.radius" -> "settings.schema.andromeda.appearance-blur-radius"
    std::string compositorLabelKey(std::string_view key) {
      std::string id(key);
      std::ranges::replace(id, '.', '-');
      std::ranges::replace(id, '_', '-');
      return "settings.schema.andromeda." + id;
    }

    double compositorNumber(const SettingsControl& compositor, std::string_view key) {
      const std::string text = compositor.value(key);
      double value = 0.0;
      std::from_chars(text.data(), text.data() + text.size(), value);
      return value;
    }

    // Compositor settings, shown only once the compositor's settings manager has reported them. Options that only
    // apply in one mode (a layout's own options, blur strength with blur off) are left out until they apply; the
    // registry rebuilds whenever the compositor reports a change.
    void addCompositorEntries(std::vector<SettingEntry>& entries, const SettingsControl& compositor) {
      using i18n::tr;
      const auto isOn = [&](std::string_view key) { return compositor.value(key) == "true"; };
      const auto add = [&](SettingsSection section, std::string_view group, std::string_view key,
                           SettingControl control, std::string tags, bool advanced = false) {
        const std::string labelKey = compositorLabelKey(key);
        entries.push_back(makeEntry(
            section, std::string(group), tr(labelKey + ".label"), tr(labelKey + ".description"), compositorPath(key),
            std::move(control), std::move(tags), advanced
        ));
      };
      const auto toggle = [&](SettingsSection section, std::string_view group, std::string_view key, std::string tags,
                              bool advanced = false) {
        add(section, group, key, ToggleSetting{isOn(key)}, std::move(tags), advanced);
      };
      const auto slider = [&](SettingsSection section, std::string_view group, std::string_view key, double min,
                              double max, double step, bool integer, std::string tags, bool advanced = false) {
        add(section, group, key, SliderSetting(compositorNumber(compositor, key), min, max, step, integer),
            std::move(tags), advanced);
      };
      const auto stepper = [&](SettingsSection section, std::string_view group, std::string_view key, int min, int max,
                               int step, std::string suffix, std::string tags, bool advanced = false) {
        add(section, group, key,
            StepperSetting{
                .value = static_cast<int>(compositorNumber(compositor, key)),
                .minValue = min,
                .maxValue = max,
                .step = step,
                .valueSuffix = std::move(suffix),
            },
            std::move(tags), advanced);
      };
      const auto choice = [&](SettingsSection section, std::string_view group, std::string_view key,
                              std::initializer_list<std::string_view> values, bool segmented, std::string tags) {
        const std::string labelKey = compositorLabelKey(key);
        std::vector<SelectOption> options;
        for (const std::string_view value : values) {
          std::string optionId(value);
          std::ranges::replace(optionId, '_', '-');
          options.push_back(SelectOption{std::string(value), tr(labelKey + ".options." + optionId)});
        }
        SelectSetting select{std::move(options), compositor.value(key)};
        select.segmented = segmented;
        add(section, group, key, std::move(select), std::move(tags));
      };

      using enum SettingsSection;
      stepper(WindowStyle, "window-borders", "appearance.border_width", 0, 100, 1, "px", "window border width outline");
      stepper(WindowStyle, "window-borders", "appearance.corner_radius", 0, 100, 1, "px", "window rounded corners");
      stepper(
          WindowStyle, "window-borders", "appearance.outer_border_width", 0, 100, 1, "px", "window outer border", true
      );
      toggle(WindowStyle, "window-shadow", "appearance.shadow.enabled", "window drop shadow");
      if (isOn("appearance.shadow.enabled")) {
        slider(WindowStyle, "window-shadow", "appearance.shadow.softness", 0, 200, 1, true, "window shadow blur size");
        stepper(
            WindowStyle, "window-shadow", "appearance.shadow.offset_x", -200, 200, 1, "px", "window shadow offset", true
        );
        stepper(
            WindowStyle, "window-shadow", "appearance.shadow.offset_y", -200, 200, 1, "px", "window shadow offset", true
        );
      }
      toggle(WindowStyle, "window-blur", "appearance.blur.enabled", "window background blur transparency");
      if (isOn("appearance.blur.enabled")) {
        slider(WindowStyle, "window-blur", "appearance.blur.radius", 0, 100, 1, true, "blur strength radius");
        slider(WindowStyle, "window-blur", "appearance.blur.passes", 0, 8, 1, true, "blur quality passes");
        toggle(WindowStyle, "window-blur", "appearance.blur.optimized", "blur optimized performance", true);
        slider(WindowStyle, "window-blur", "appearance.blur.noise", 0.0, 1.0, 0.01, false, "blur noise grain", true);
        slider(
            WindowStyle, "window-blur", "appearance.blur.brightness", 0.0, 2.0, 0.05, false, "blur brightness", true
        );
        slider(WindowStyle, "window-blur", "appearance.blur.contrast", 0.0, 2.0, 0.05, false, "blur contrast", true);
        slider(
            WindowStyle, "window-blur", "appearance.blur.saturation", 0.0, 2.0, 0.05, false, "blur saturation", true
        );
      }
      toggle(WindowStyle, "window-decorations", "appearance.prefer_no_csd", "client side decorations title bar csd");
      slider(
          WindowStyle, "window-decorations", "appearance.drag_opacity", 0.0, 1.0, 0.05, false,
          "window drag move opacity transparency"
      );
      toggle(WindowStyle, "window-decorations", "appearance.opaque_fullscreen", "fullscreen opaque transparency", true);

      toggle(TextScale, "accessibility", "appearance.increase_contrast", "accessibility high contrast visually impaired");
      toggle(
          TextScale, "accessibility", "appearance.reduce_transparency",
          "accessibility reduce transparency glass solid opaque"
      );
      if (!isOn("appearance.reduce_transparency")) {
        slider(
            TextScale, "accessibility", "appearance.glass_opacity", 0.0, 1.0, 0.05, false,
            "glass opacity transparency material"
        );
      }
      toggle(Motion, "motion", "appearance.reduce_motion", "accessibility reduce motion animations");

      toggle(Motion, "window-motion", "animation.enabled", "window animations compositor");
      if (isOn("animation.enabled")) {
        stepper(Motion, "window-motion", "animation.duration_ms", 1, 10000, 25, "ms", "animation duration speed");
        toggle(Motion, "window-motion", "animation.windows_in.enabled", "window open animation");
        if (isOn("animation.windows_in.enabled")) {
          choice(
              Motion, "window-motion", "animation.windows_in.style", {"popin", "zoom", "slide", "fade", "none"}, false,
              "window open animation style"
          );
        }
        toggle(Motion, "window-motion", "animation.windows_out.enabled", "window close animation");
        if (isOn("animation.windows_out.enabled")) {
          choice(
              Motion, "window-motion", "animation.windows_out.style", {"fade", "slide", "popin", "zoom"}, false,
              "window close animation style"
          );
        }
        toggle(Motion, "window-motion", "animation.windows_move.enabled", "window move resize animation");
        toggle(Motion, "window-motion", "animation.workspaces.enabled", "workspace switch animation");
        toggle(Motion, "window-motion", "animation.overview.enabled", "overview animation");
        toggle(Motion, "window-motion", "animation.scratchpad.enabled", "scratchpad animation");
        toggle(Motion, "window-motion", "animation.border.enabled", "border focus color animation");
        toggle(Motion, "window-motion", "animation.layers.enabled", "panel layer surface animation", true);
      }
      toggle(Motion, "dim-unfocused", "animation.dim_unfocused.enabled", "dim inactive unfocused windows");
      if (isOn("animation.dim_unfocused.enabled")) {
        slider(Motion, "dim-unfocused", "animation.dim_unfocused.dim", 0.0, 1.0, 0.05, false, "dim amount");
      }

      const std::string mode = compositor.value("layout.mode");
      choice(Layout, "layout", "layout.mode", {"scrolling", "dwindle", "master"}, true, "tiling layout mode");
      stepper(Layout, "layout", "layout.gap", 0, 500, 1, "px", "gaps spacing between windows");
      if (mode == "scrolling") {
        choice(
            Layout, "layout", "layout.scrolling.center_focused", {"never", "always", "on_overflow"}, false,
            "center focused column"
        );
        toggle(Layout, "layout", "layout.scrolling.center_underfull_strip", "center columns");
      } else if (mode == "dwindle") {
        toggle(Layout, "layout", "layout.dwindle.preserve_split", "split direction");
      } else if (mode == "master") {
        choice(Layout, "layout", "layout.master.position", {"left", "right", "center"}, true, "master stack position");
        slider(Layout, "layout", "layout.master.default_width_fraction", 0.1, 0.9, 0.05, false, "master width ratio");
        toggle(Layout, "layout", "layout.master.new_on_top", "new window stack order");
        toggle(Layout, "layout", "layout.master.new_becomes_master", "new window master");
      }
      toggle(Layout, "workspaces", "workspaces.back_and_forth", "workspace back and forth previous");
      toggle(Layout, "workspaces", "workspaces.empty_above", "workspace empty");

      slider(Overview, "overview", "overview.zoom", 0.1, 0.75, 0.05, false, "overview zoom scale size");
      toggle(Overview, "overview", "overview.background_blur", "overview blur wallpaper");
      toggle(Overview, "overview", "overview.workspace_wallpaper", "overview wallpaper preview");
      toggle(Overview, "overview", "overview.shortcuts", "overview keyboard shortcut badges");

      toggle(Windows, "focus", "input.focus.follows_mouse", "focus follows mouse hover pointer");
      toggle(Windows, "focus", "input.cursor.follows_focus", "cursor warp follows focus");
      toggle(Windows, "focus", "general.focus_on_activate", "focus activation request urgent");
      toggle(Windows, "focus", "general.show_cheatsheet", "keybind cheatsheet startup help");

      choice(Shortcuts, "modifier", "general.mod_key", {"Super", "Alt", "Ctrl", "Shift"}, true, "mod key modifier");

      // Hot corners run a compositor action; the shell's own panels are reached through shell: actions.
      constexpr std::array<std::pair<std::string_view, std::string_view>, 5> kCornerActions{{
          {"shell:panel-toggle launcher", "launcher"},
          {"shell:panel-toggle control-center", "control-center"},
          {"shell:window-switcher", "window-switcher"},
          {"overview-toggle", "overview"},
          {"cheatsheet-toggle", "cheatsheet"},
      }};
      for (const std::string_view corner : {"top_left", "top_right", "bottom_left", "bottom_right"}) {
        const std::string prefix = "hot_corners." + std::string(corner);
        toggle(Windows, "hot-corners", prefix + ".enabled", "hot corner screen edge pointer " + std::string(corner));
        if (!isOn(prefix + ".enabled")) {
          continue;
        }
        const std::string actionKey = prefix + ".action";
        const std::string current = compositor.value(actionKey);
        std::vector<SelectOption> options;
        for (const auto& [value, id] : kCornerActions) {
          options.push_back(
              SelectOption{std::string(value), tr("settings.schema.andromeda.hot-corner-actions." + std::string(id))}
          );
        }
        if (!current.empty()
            && std::ranges::none_of(kCornerActions, [&](const auto& a) { return a.first == current; })) {
          options.push_back(SelectOption{current, current});
        }
        add(Windows, "hot-corners", actionKey, SelectSetting{std::move(options), current}, "hot corner action");
        stepper(Windows, "hot-corners", prefix + ".delay_ms", 0, 10000, 50, "ms", "hot corner delay");
      }
    }

  } // namespace

  std::optional<std::string> compositorSettingKey(const std::vector<std::string>& path) {
    if (path.size() < 2 || path.front() != kCompositorPathRoot) {
      return std::nullopt;
    }
    return StringUtils::join(std::vector<std::string>(path.begin() + 1, path.end()), ".");
  }

  std::vector<SettingEntry> buildSettingsRegistry(
      const Config& cfg, const BarConfig* selectedBar, const BarMonitorOverride* selectedMonitorOverride,
      const RegistryEnvironment& env
  ) {
    (void)selectedBar;
    (void)selectedMonitorOverride;
    using i18n::tr;
    const auto positionSelect = [](std::string_view selected) {
      return asSegmented(plainSelect(
          {{"top", "settings.options.edge.top"},
           {"bottom", "settings.options.edge.bottom"},
           {"left", "settings.options.edge.left"},
           {"right", "settings.options.edge.right"}},
          selected
      ));
    };
    std::vector<SettingEntry> entries;

    // Appearance
    auto themeRows = generateSchemaEntries(noctalia::config::schema::themeSchema(), cfg.theme, {"theme"});
    auto shellRows = generateSchemaEntries(noctalia::config::schema::shellSchema(), cfg.shell, {"shell"});
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
    auto panelRows =
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

    // Security
    for (const auto* key : {"offline_mode", "external_ip_enabled", "telemetry_enabled", "polkit_agent", "password_style"}) {
      appendRow(entries, shellRows, key);
    }
    const SettingVisibility lockscreenOn = [](const Config& c) { return c.lockscreen.enabled; };
    auto lockscreenRows =
        generateSchemaEntries(noctalia::config::schema::lockscreenSchema(), cfg.lockscreen, {"lockscreen"});
    appendRow(entries, lockscreenRows, "enabled");
    {
      auto e = makeEntry(
          SettingsSection::Security, "lock-screen", tr("settings.schema.lockscreen.lock-before-suspend.label"),
          tr("settings.schema.lockscreen.lock-before-suspend.description"), {"lockscreen", "lock_before_suspend"},
          ToggleSetting{cfg.lockscreen.lockBeforeSuspend}, "lock screen before suspend sleep lid close prepareforsleep"
      );
      e.visibleWhen = lockscreenOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Security, "lock-screen", tr("settings.schema.lockscreen.grace-period.label"),
          tr("settings.schema.lockscreen.grace-period.description"), {"lockscreen", "grace_period_seconds"},
          StepperSetting{
              .value = std::clamp(cfg.lockscreen.gracePeriodSeconds, 0, 60),
              .minValue = 0,
              .maxValue = 60,
              .step = 1,
              .valueSuffix = "s"
          },
          "lock screen grace period passwordless unlock keypress mouse movement"
      );
      e.visibleWhen = lockscreenOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Security, "lock-screen", tr("settings.schema.lockscreen.fingerprint.label"),
          tr("settings.schema.lockscreen.fingerprint.description"), {"lockscreen", "fingerprint"},
          ToggleSetting{cfg.lockscreen.fingerprint}, "lock screen fingerprint fprintd biometric"
      );
      e.visibleWhen = lockscreenOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Security, "lock-screen", tr("settings.schema.lockscreen.face.label"),
          tr("settings.schema.lockscreen.face.description"), {"lockscreen", "face"}, ToggleSetting{cfg.lockscreen.face},
          "lock screen face unlock camera gaze biometric"
      );
      e.visibleWhen = lockscreenOn;
      entries.push_back(std::move(e));
    }
    {
      SelectSetting style{
          {SelectOption{.value = "ryoku", .label = tr("settings.schema.lockscreen.style.ryoku")},
           SelectOption{.value = "classic", .label = tr("settings.schema.lockscreen.style.classic")}},
          cfg.lockscreen.style
      };
      style.segmented = true;
      auto e = makeEntry(
          SettingsSection::Security, "lock-screen", tr("settings.schema.lockscreen.style.label"),
          tr("settings.schema.lockscreen.style.description"), {"lockscreen", "style"}, std::move(style),
          "lock screen style ryoku classic look theme"
      );
      e.visibleWhen = lockscreenOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Security, "lock-screen", tr("settings.schema.lockscreen.allow-empty-password.label"),
          tr("settings.schema.lockscreen.allow-empty-password.description"), {"lockscreen", "allow_empty_password"},
          ToggleSetting{cfg.lockscreen.allowEmptyPassword}, "lock screen empty password security key pam"
      );
      e.visibleWhen = lockscreenOn;
      entries.push_back(std::move(e));
    }
    if (env.screencopySupported) {
      MultiSelectSetting transitions;
      transitions.options.reserve(std::size(kLockscreenTransitions));
      for (const auto& opt : kLockscreenTransitions) {
        transitions.options.push_back(SelectOption{std::string(opt.key), tr(opt.labelKey)});
      }
      transitions.selectedValues.reserve(cfg.lockscreen.transitions.size());
      for (const auto& transition : cfg.lockscreen.transitions) {
        transitions.selectedValues.emplace_back(enumToKey(kLockscreenTransitions, transition));
      }
      auto transitionEffects = makeEntry(
          SettingsSection::Security, "lock-screen", tr("settings.schema.lockscreen.transitions.label"),
          tr("settings.schema.lockscreen.transitions.description"), {"lockscreen", "transition"},
          std::move(transitions), "lock screen effects animation pool"
      );
      transitionEffects.visibleWhen = lockscreenOn;
      entries.push_back(std::move(transitionEffects));

      auto transitionDuration = makeEntry(
          SettingsSection::Security, "lock-screen", tr("settings.schema.lockscreen.transition-duration.label"),
          tr("settings.schema.lockscreen.transition-duration.description"), {"lockscreen", "transition_duration"},
          sliderFor(
              cfg.lockscreen.transitionDurationMs, noctalia::config::schema::kLockscreenTransitionDurationRange, true
          ),
          "lock screen transition animation duration"
      );
      transitionDuration.visibleWhen = lockscreenOn;
      entries.push_back(std::move(transitionDuration));

      auto edgeSmoothness = makeEntry(
          SettingsSection::Security, "lock-screen", tr("settings.schema.lockscreen.edge-smoothness.label"),
          tr("settings.schema.lockscreen.edge-smoothness.description"), {"lockscreen", "edge_smoothness"},
          sliderFor(cfg.lockscreen.edgeSmoothness, noctalia::config::schema::kUnitRange, false),
          "lock screen transition feathering", true
      );
      edgeSmoothness.visibleWhen = lockscreenOn;
      entries.push_back(std::move(edgeSmoothness));
    }
    {
      const SettingVisibility lockscreenWallpaperOn = [](const Config& c) {
        return c.lockscreen.enabled;
      };
      auto e = makeEntry(
          SettingsSection::Security, "lock-screen", tr("settings.schema.lockscreen.wallpaper.label"),
          tr("settings.schema.lockscreen.wallpaper.description"), {"lockscreen", "wallpaper"},
          TextSetting{
              .value = cfg.lockscreen.wallpaper,
              .placeholder = tr("settings.schema.lockscreen.wallpaper.placeholder"),
              .browseMode = TextSettingBrowseMode::OpenFile,
              .browseFileExtensions = DirectoryScanner::imageExtensionFilter(true),
              .browseFallbackDirectory = wallpaper::resolveGlobalWallpaperDirectory(
                  cfg.wallpaper,
                  wallpaper::effectiveThemeMode(
                      shellThemeMode(cfg.theme), shellThemeMode(cfg.theme) == ThemeMode::Light
                  )
              ),
          },
          "lock screen background image custom"
      );
      e.visibleWhen = lockscreenWallpaperOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Security, "lock-screen", tr("settings.schema.lockscreen.blur-intensity.label"),
          tr("settings.schema.lockscreen.blur-intensity.description"), {"lockscreen", "blur_intensity"},
          sliderFor(cfg.lockscreen.blurIntensity, noctalia::config::schema::kUnitRange, false), "lock screen blur"
      );
      e.visibleWhen = lockscreenOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Security, "lock-screen", tr("settings.schema.lockscreen.tint-intensity.label"),
          tr("settings.schema.lockscreen.tint-intensity.description"), {"lockscreen", "tint_intensity"},
          sliderFor(cfg.lockscreen.tintIntensity, noctalia::config::schema::kUnitRange, false), "lock screen tint"
      );
      e.visibleWhen = lockscreenOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Security, "lock-screen", tr("settings.schema.lockscreen.monitors.label"),
          tr("settings.schema.lockscreen.monitors.description"), {"lockscreen", "monitors"},
          ListSetting{.items = cfg.lockscreen.monitors, .suggestedOptions = env.availableOutputs},
          "lock screen monitor output connector"
      );
      e.visibleWhen = lockscreenOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Security, "lock-screen", tr("settings.schema.lockscreen.widgets.label"),
          tr("settings.schema.lockscreen.widgets.description"), {"lockscreen_widgets", "enabled"},
          ToggleSetting{cfg.lockscreenWidgets.enabled}, "lock screen widgets layout"
      );
      e.visibleWhen = lockscreenOn;
      entries.push_back(std::move(e));
    }
    auto privacyRows =
        generateSchemaEntries(noctalia::config::schema::privacySchema(), cfg.shell.privacy, {"shell", "privacy"});
    appendGroup(entries, privacyRows, "privacy");
    if (env.greeterSyncAvailable) {
      entries.push_back(makeEntry(
          SettingsSection::Security, "greeter", tr("settings.schema.shell.greeter-sync-privilege-command.label"),
          tr("settings.schema.shell.greeter-sync-privilege-command.description"),
          {"shell", "greeter_sync", "privilege_command"},
          TextSetting{
              .value = cfg.shell.greeterSync.privilegeCommand,
              .placeholder = "pkexec",
              .browseFileExtensions = {},
          },
          "greeter sync pkexec run0 ghostty terminal sudo polkit wrapper"
      ));
    }
    // Shell
    entries.push_back(makeEntry(
        SettingsSection::Shell, "general", tr("settings.schema.shell.avatar-path.label"),
        tr("settings.schema.shell.avatar-path.description"), {"shell", "avatar_path"},
        TextSetting{
            .value = env.shellAvatarPath,
            .placeholder = tr("settings.schema.shell.avatar-path.placeholder"),
            .browseMode = TextSettingBrowseMode::OpenFile,
            .browseFileExtensions = DirectoryScanner::imageExtensionFilter(true)
        },
        "image picture"
    ));
    for (const auto* key : {"settings_window_translucent", "settings_expand_all_groups", "time_format", "date_format",
                            "readline_shortcuts"}) {
      appendRow(entries, shellRows, key);
    }
    entries.push_back(makeEntry(
        SettingsSection::Shell, "keyboard-layout", tr("settings.schema.shell.keyboard-layout-custom-labels.label"),
        tr("settings.schema.shell.keyboard-layout-custom-labels.description"),
        {"shell", "keyboard_layout", "custom_labels"},
        StringMapSetting{
            .entries = cfg.shell.keyboardLayout.customLabels,
            .suggestedKeys = env.keyboardLayoutNames,
            .keyPlaceholder = tr("settings.widgets.map-placeholders.layout-name"),
            .valuePlaceholder = tr("settings.widgets.map-placeholders.value"),
        },
        "keyboard layout nickname alias label"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Shell, "general", tr("settings.schema.shell.launch-apps-as-systemd-services.label"),
        env.systemdUserManaged ? tr("settings.schema.shell.launch-apps-as-systemd-services.description")
                               : tr("settings.schema.shell.launch-apps-as-systemd-services.requires-systemd-session"),
        {"shell", "launch_apps_as_systemd_services"},
        ToggleSetting{
            .checked = cfg.shell.launchAppsAsSystemdServices,
            // Keep a leftover `true` switchable off even when the session cannot honor it.
            .enabled = env.systemdUserManaged || cfg.shell.launchAppsAsSystemdServices
        }
    ));
    {
      auto e = makeEntry(
          SettingsSection::Shell, "general", tr("settings.schema.shell.launch-apps-custom-command.label"),
          tr("settings.schema.shell.launch-apps-custom-command.description"), {"shell", "launch_apps_custom_command"},
          TextSetting{
              .value = cfg.shell.launchAppsCustomCommand,
              .placeholder = tr("settings.schema.shell.launch-apps-custom-command.placeholder"),
              .width = 320.0F,
              .browseFileExtensions = {},
          },
          "app command custom launcher dock taskbar"
      );
      e.visibleWhen = [managed = env.systemdUserManaged](const Config& c) {
        return !(c.shell.launchAppsAsSystemdServices && managed);
      };
      entries.push_back(std::move(e));
    }
    const SettingVisibility clipboardOn = [](const Config& c) { return c.shell.clipboardEnabled; };
    appendRow(entries, shellRows, "clipboard_enabled");
    // Deliberately not gated on clipboardOn: this keeps the live selection
    // alive, which is part of the transport that stays active when history
    // retention is turned off.
    appendRow(entries, shellRows, "clipboard_keep_from_closed_apps");
    {
      auto e = makeEntry(
          SettingsSection::Shell, "clipboard", tr("settings.schema.shell.clipboard-history-max-entries.label"),
          tr("settings.schema.shell.clipboard-history-max-entries.description"),
          {"shell", "clipboard_history_max_entries"},
          StepperSetting{
              .value = cfg.shell.clipboardHistoryMaxEntries,
              .minValue = static_cast<int>(noctalia::config::schema::kClipboardHistoryMaxEntriesRange.min.value()),
              .maxValue = static_cast<int>(noctalia::config::schema::kClipboardHistoryMaxEntriesRange.max.value()),
              .step = static_cast<int>(noctalia::config::schema::kClipboardHistoryMaxEntriesRange.step.value())
          },
          "clipboard history limit entries"
      );
      e.visibleWhen = clipboardOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Shell, "clipboard", tr("settings.schema.shell.clipboard-confirm-clear-history.label"),
          tr("settings.schema.shell.clipboard-confirm-clear-history.description"),
          {"shell", "clipboard_confirm_clear_history"}, ToggleSetting{cfg.shell.clipboardConfirmClearHistory},
          "clipboard history clear confirm pinned"
      );
      e.visibleWhen = clipboardOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Shell, "clipboard", tr("settings.schema.shell.clipboard-auto-paste.label"),
          tr("settings.schema.shell.clipboard-auto-paste.description"), {"shell", "clipboard_auto_paste"},
          enumSelect(kClipboardAutoPasteModes, cfg.shell.clipboardAutoPaste), "clipboard paste"
      );
      e.visibleWhen = clipboardOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Shell, "clipboard", tr("settings.schema.shell.clipboard-image-action.label"),
          tr("settings.schema.shell.clipboard-image-action.description"), {"shell", "clipboard_image_action_command"},
          TextSetting{
              .value = cfg.shell.clipboardImageActionCommand,
              .placeholder = tr("settings.schema.shell.clipboard-image-action.placeholder"),
              .width = 320.0F,
              .browseFileExtensions = {}
          },
          "clipboard image action annotation editor external gimp satty gradia"
      );
      e.visibleWhen = clipboardOn;
      entries.push_back(std::move(e));
    }
    auto screenshotRows = generateSchemaEntries(
        noctalia::config::schema::screenshotSchema(), cfg.shell.screenshot, {"shell", "screenshot"}
    );
    for (const auto* key : {"freeze_screen", "confirm_region", "remember_last_region", "region_modifier", "show_cursor"}) {
      appendRow(entries, screenshotRows, key);
    }
    appendGroup(entries, screenshotRows, "screenshot-capture");
    appendGroup(entries, screenshotRows, "screenshot-annotation");
    for (const auto* key : {"copy_to_clipboard", "save_to_file", "directory", "filename_pattern", "format"}) {
      appendRow(entries, screenshotRows, key);
    }
    {
      auto e = makeEntry(
          SettingsSection::Screenshot, "screenshot-output", tr("settings.schema.shell.screenshot-quality.label"),
          tr("settings.schema.shell.screenshot-quality.description"), {"shell", "screenshot", "quality"},
          sliderFor(cfg.shell.screenshot.quality, noctalia::config::schema::kScreenshotQualityRange, true),
          "screenshot capture save file quality compression lossless jpeg webp jxl"
      );
      e.visibleWhen = [](const Config& c) { return c.shell.screenshot.format != ScreenshotFormat::Png; };
      entries.push_back(std::move(e));
    }
    appendRow(entries, screenshotRows, "pipe_to_command");
    {
      auto e = makeEntry(
          SettingsSection::Screenshot, "screenshot-output", tr("settings.schema.shell.screenshot-pipe-command.label"),
          tr("settings.schema.shell.screenshot-pipe-command.description"), {"shell", "screenshot", "pipe_command"},
          TextSetting{
              .value = cfg.shell.screenshot.pipeCommand,
              .placeholder = tr("settings.schema.shell.screenshot-pipe-command.placeholder"),
              .width = 320.0F,
              .browseFileExtensions = {}
          },
          "screenshot capture run pipe command stdin png"
      );
      e.visibleWhen = [](const Config& c) { return c.shell.screenshot.pipeToCommand; };
      entries.push_back(std::move(e));
    }
    auto windowSwitcherRows = generateSchemaEntries(
        noctalia::config::schema::windowSwitcherSchema(), cfg.shell.windowSwitcher, {"shell", "window_switcher"}
    );
    appendGroup(entries, windowSwitcherRows, "window-switcher");
    // Keybinds
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.validate.label"),
        tr("settings.schema.keybinds.validate.description"), {"keybinds", "validate"},
        KeybindListSetting{
            .items = effectiveKeybindItems(cfg.keybinds.validate, KeybindAction::Validate), .maxItems = 4
        },
        "keybind shortcut hotkey enter accept submit confirm"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.cancel.label"),
        tr("settings.schema.keybinds.cancel.description"), {"keybinds", "cancel"},
        KeybindListSetting{.items = effectiveKeybindItems(cfg.keybinds.cancel, KeybindAction::Cancel), .maxItems = 4},
        "keybind shortcut hotkey escape close dismiss"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.left.label"),
        tr("settings.schema.keybinds.left.description"), {"keybinds", "left"},
        KeybindListSetting{.items = effectiveKeybindItems(cfg.keybinds.left, KeybindAction::Left), .maxItems = 4},
        "keybind shortcut hotkey arrow move"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.right.label"),
        tr("settings.schema.keybinds.right.description"), {"keybinds", "right"},
        KeybindListSetting{.items = effectiveKeybindItems(cfg.keybinds.right, KeybindAction::Right), .maxItems = 4},
        "keybind shortcut hotkey arrow move"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.up.label"),
        tr("settings.schema.keybinds.up.description"), {"keybinds", "up"},
        KeybindListSetting{.items = effectiveKeybindItems(cfg.keybinds.up, KeybindAction::Up), .maxItems = 4},
        "keybind shortcut hotkey arrow move"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.down.label"),
        tr("settings.schema.keybinds.down.description"), {"keybinds", "down"},
        KeybindListSetting{.items = effectiveKeybindItems(cfg.keybinds.down, KeybindAction::Down), .maxItems = 4},
        "keybind shortcut hotkey arrow move"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.tab-previous.label"),
        tr("settings.schema.keybinds.tab-previous.description"), {"keybinds", "tab_previous"},
        KeybindListSetting{
            .items = effectiveKeybindItems(cfg.keybinds.tabPrevious, KeybindAction::TabPrevious), .maxItems = 4
        },
        "keybind shortcut hotkey shift tab focus pane"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.tab-next.label"),
        tr("settings.schema.keybinds.tab-next.description"), {"keybinds", "tab_next"},
        KeybindListSetting{.items = effectiveKeybindItems(cfg.keybinds.tabNext, KeybindAction::TabNext), .maxItems = 4},
        "keybind shortcut hotkey tab focus pane"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.copy.label"),
        tr("settings.schema.keybinds.copy.description"), {"keybinds", "copy"},
        KeybindListSetting{.items = effectiveKeybindItems(cfg.keybinds.copy, KeybindAction::Copy), .maxItems = 4},
        "keybind shortcut hotkey copy clipboard"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.save.label"),
        tr("settings.schema.keybinds.save.description"), {"keybinds", "save"},
        KeybindListSetting{.items = effectiveKeybindItems(cfg.keybinds.save, KeybindAction::Save), .maxItems = 4},
        "keybind shortcut hotkey save file"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.delete.label"),
        tr("settings.schema.keybinds.delete.description"), {"keybinds", "delete"},
        KeybindListSetting{
            .items = effectiveKeybindItems(cfg.keybinds.deleteEntry, KeybindAction::Delete), .maxItems = 4
        },
        "keybind shortcut hotkey delete remove clear"
    ));

    // Niri-specific integrations
    if (env.niriOverviewTypeToLaunchSupported || env.niriBackdropSupported) {
      if (env.niriOverviewTypeToLaunchSupported) {
        entries.push_back(makeEntry(
            SettingsSection::Niri, "overview", tr("settings.schema.shell.niri-overview-type-to-launch.label"),
            tr("settings.schema.shell.niri-overview-type-to-launch.description"),
            {"shell", "niri_overview_type_to_launch_enabled"}, ToggleSetting{cfg.shell.niriOverviewTypeToLaunchEnabled},
            "niri overview type launch launcher search keyboard focus"
        ));
      }
      if (env.niriOverviewCloseOnLaunchSupported) {
        entries.push_back(makeEntry(
            SettingsSection::Niri, "overview", tr("settings.schema.shell.niri-overview-close-on-launch.label"),
            tr("settings.schema.shell.niri-overview-close-on-launch.description"),
            {"shell", "niri_overview_close_on_launch_enabled"},
            ToggleSetting{cfg.shell.niriOverviewCloseOnLaunchEnabled}, "niri overview close app launch launcher"
        ));
      }
      if (env.niriBackdropSupported) {
        entries.push_back(makeEntry(
            SettingsSection::Niri, "backdrop", tr("settings.schema.shared.enabled.label"),
            tr("settings.schema.backdrop.enabled.description"), {"backdrop", "enabled"},
            ToggleSetting{cfg.backdrop.enabled}, "wallpaper backdrop"
        ));
        entries.push_back(makeEntry(
            SettingsSection::Niri, "backdrop", tr("settings.schema.backdrop.blur-intensity.label"),
            tr("settings.schema.backdrop.blur-intensity.description"), {"backdrop", "blur_intensity"},
            sliderFor(cfg.backdrop.blurIntensity, noctalia::config::schema::kUnitRange, false), "wallpaper"
        ));
        entries.push_back(makeEntry(
            SettingsSection::Niri, "backdrop", tr("settings.schema.backdrop.tint-intensity.label"),
            tr("settings.schema.backdrop.tint-intensity.description"), {"backdrop", "tint_intensity"},
            sliderFor(cfg.backdrop.tintIntensity, noctalia::config::schema::kUnitRange, false), "wallpaper"
        ));
      }
    }

    // Andromeda-specific integrations
    if (env.andromedaOverviewTypeToLaunchSupported) {
      entries.push_back(makeEntry(
          SettingsSection::Services, "audio", tr("settings.schema.shell.andromeda-audio-provider.label"),
          tr("settings.schema.shell.andromeda-audio-provider.description"),
          {"shell", "andromeda_audio_provider_enabled"}, ToggleSetting{cfg.shell.andromedaAudioProviderEnabled},
          "andromeda audio spectrum shader effects provider"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Overview, "overview", tr("settings.schema.shell.andromeda-overview-type-to-launch.label"),
          tr("settings.schema.shell.andromeda-overview-type-to-launch.description"),
          {"shell", "andromeda_overview_type_to_launch_enabled"},
          ToggleSetting{cfg.shell.andromedaOverviewTypeToLaunchEnabled},
          "andromeda overview type launch launcher search keyboard focus"
      ));
    }
    if (env.andromedaOverviewCloseOnLaunchSupported) {
      entries.push_back(makeEntry(
          SettingsSection::Overview, "overview", tr("settings.schema.shell.andromeda-overview-close-on-launch.label"),
          tr("settings.schema.shell.andromeda-overview-close-on-launch.description"),
          {"shell", "andromeda_overview_close_on_launch_enabled"},
          ToggleSetting{cfg.shell.andromedaOverviewCloseOnLaunchEnabled}, "andromeda overview close app launch launcher"
      ));
    }

    // System
    if (env.batteryAvailable) {
      if (env.systemBatteryAvailable) {
        entries.push_back(makeEntry(
            SettingsSection::System, "battery", tr("settings.schema.system.battery-warning-threshold.label"),
            tr("settings.schema.system.battery-warning-threshold.description"), {"battery", "warning_threshold"},
            sliderFor(cfg.battery.warningThreshold, noctalia::config::schema::kBatteryWarningThresholdRange, true),
            "battery low warning threshold notification"
        ));
      }
      for (const auto& device : env.batteryDeviceOptions) {
        int value = 0;
        if (const auto it = env.batteryWarningThresholds.find(device.value); it != env.batteryWarningThresholds.end()) {
          value = it->second;
        }
        entries.push_back(makeEntry(
            SettingsSection::System, "battery",
            tr("settings.schema.system.battery-device-warning-threshold.label", "device", device.label),
            tr("settings.schema.system.battery-device-warning-threshold.description"),
            {"battery", "device", device.value, "warning_threshold"},
            SliderSetting{std::clamp(value, 0, 100), 0.0F, 100.0F, 1.0F, true},
            std::string("battery device low warning threshold notification ") + device.label + " " + device.value
        ));
      }
    }
    appendRow(entries, shellRows, "screen_time_enabled");
    const SettingVisibility monitorOn = [](const Config& c) { return c.system.monitor.enabled; };
    auto systemMonitorRows = generateSchemaEntries(
        noctalia::config::schema::systemMonitorRowsSchema(), cfg.system.monitor, {"system", "monitor"}
    );
    appendRow(entries, systemMonitorRows, "enabled");
    {
      // The slider goes down to 0, which disables the metric (no polling, no dGPU wakeups).
      constexpr float kPollMin = SystemConfig::MonitorConfig::kDisabledPollSeconds;
      constexpr float kPollMax = SystemConfig::MonitorConfig::kMaxPollSeconds;
      constexpr float kPollStep = 1.0F;
      const auto& mon = cfg.system.monitor;
      auto addPoll = [&](std::string_view labelKey, std::string_view descKey, std::vector<std::string> path,
                         float value) {
        const float clampedValue = std::clamp(value, kPollMin, kPollMax);
        SliderSetting slider{clampedValue, kPollMin, kPollMax, kPollStep, true};
        slider.valueSuffix = "s";
        auto entry = makeEntry(
            SettingsSection::System, "monitor-polling", tr(labelKey), tr(descKey), std::move(path), std::move(slider),
            "system monitor", true
        );
        entry.visibleWhen = monitorOn;
        entries.push_back(std::move(entry));
      };
      addPoll(
          "settings.schema.services.system-monitor.cpu-poll.label",
          "settings.schema.services.system-monitor.cpu-poll.description", {"system", "monitor", "cpu_poll_seconds"},
          mon.cpuPollSeconds
      );
      addPoll(
          "settings.schema.services.system-monitor.gpu-poll.label",
          "settings.schema.services.system-monitor.gpu-poll.description", {"system", "monitor", "gpu_poll_seconds"},
          mon.gpuPollSeconds
      );
      addPoll(
          "settings.schema.services.system-monitor.memory-poll.label",
          "settings.schema.services.system-monitor.memory-poll.description",
          {"system", "monitor", "memory_poll_seconds"}, mon.memoryPollSeconds
      );
      addPoll(
          "settings.schema.services.system-monitor.network-poll.label",
          "settings.schema.services.system-monitor.network-poll.description",
          {"system", "monitor", "network_poll_seconds"}, mon.networkPollSeconds
      );
      addPoll(
          "settings.schema.services.system-monitor.disk-poll.label",
          "settings.schema.services.system-monitor.disk-poll.description", {"system", "monitor", "disk_poll_seconds"},
          mon.diskPollSeconds
      );

      // One dual-thumb range row per metric: low thumb = activity threshold, high thumb = critical.
      auto addThresholdPair = [&](std::string_view baseKey, std::string_view statLabelKey, double activityValue,
                                  double criticalValue, noctalia::sysmon::ThresholdProfile profile, bool integerValue,
                                  std::string valueSuffix) {
        const std::vector<std::string> activityPath = {
            "system", "monitor", std::string(baseKey) + "_activity_threshold"
        };
        const std::vector<std::string> criticalPath = {
            "system", "monitor", std::string(baseKey) + "_critical_threshold"
        };
        const std::string statLabel = tr(statLabelKey);

        RangeSliderSetting range;
        range.lowValue = activityValue;
        range.highValue = criticalValue;
        range.minValue = profile.minValue;
        range.maxValue = profile.maxValue;
        range.step = profile.step;
        range.integerValue = integerValue;
        range.valueSuffix = std::move(valueSuffix);
        range.highPath = criticalPath;

        auto entry = makeEntry(
            SettingsSection::System, "monitor-thresholds",
            tr("settings.schema.services.system-monitor.threshold.label", "stat", statLabel),
            tr("settings.schema.services.system-monitor.threshold.description"), activityPath, std::move(range),
            "system monitor threshold activity critical", true
        );
        entry.visibleWhen = monitorOn;
        entries.push_back(std::move(entry));
      };

      using noctalia::sysmon::Stat;
      addThresholdPair(
          "cpu_usage", "settings.schema.services.system-monitor.stats.cpu-usage", mon.cpuUsageActivityThreshold,
          mon.cpuUsageCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::CpuUsage), true, "%"
      );
      addThresholdPair(
          "cpu_temp", "settings.schema.services.system-monitor.stats.cpu-temp", mon.cpuTempActivityThreshold,
          mon.cpuTempCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::CpuTemp), true, "°C"
      );
      addThresholdPair(
          "cpu_freq", "settings.schema.services.system-monitor.stats.cpu-freq", mon.cpuFreqActivityThreshold,
          mon.cpuFreqCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::CpuFreq), false, "GHz"
      );
      addThresholdPair(
          "gpu_usage", "settings.schema.services.system-monitor.stats.gpu-usage", mon.gpuUsageActivityThreshold,
          mon.gpuUsageCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::GpuUsage), true, "%"
      );
      addThresholdPair(
          "gpu_temp", "settings.schema.services.system-monitor.stats.gpu-temp", mon.gpuTempActivityThreshold,
          mon.gpuTempCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::GpuTemp), true, "°C"
      );
      addThresholdPair(
          "gpu_vram", "settings.schema.services.system-monitor.stats.gpu-vram", mon.gpuVramActivityThreshold,
          mon.gpuVramCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::GpuVram), true, "%"
      );
      addThresholdPair(
          "ram_pct", "settings.schema.services.system-monitor.stats.ram-usage", mon.ramPctActivityThreshold,
          mon.ramPctCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::RamPct), true, "%"
      );
      addThresholdPair(
          "swap_pct", "settings.schema.services.system-monitor.stats.swap-usage", mon.swapPctActivityThreshold,
          mon.swapPctCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::SwapPct), true, "%"
      );
      addThresholdPair(
          "disk_used_pct", "settings.schema.services.system-monitor.stats.disk-used-pct",
          mon.diskUsedPctActivityThreshold, mon.diskUsedPctCriticalThreshold,
          noctalia::sysmon::thresholdProfile(Stat::DiskUsedPct), true, "%"
      );
      addThresholdPair(
          "disk_used", "settings.schema.services.system-monitor.stats.disk-used", mon.diskUsedActivityThreshold,
          mon.diskUsedCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::DiskUsed), true, "%"
      );
      addThresholdPair(
          "disk_free_pct", "settings.schema.services.system-monitor.stats.disk-free-pct",
          mon.diskFreePctActivityThreshold, mon.diskFreePctCriticalThreshold,
          noctalia::sysmon::thresholdProfile(Stat::DiskFreePct), true, "%"
      );
      addThresholdPair(
          "disk_free", "settings.schema.services.system-monitor.stats.disk-free", mon.diskFreeActivityThreshold,
          mon.diskFreeCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::DiskFree), true, "%"
      );
      addThresholdPair(
          "net_rx", "settings.schema.services.system-monitor.stats.network-rx", mon.netRxActivityThreshold,
          mon.netRxCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::NetRx), false, "MB/s"
      );
      addThresholdPair(
          "net_tx", "settings.schema.services.system-monitor.stats.network-tx", mon.netTxActivityThreshold,
          mon.netTxCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::NetTx), false, "MB/s"
      );
    }

    // Location: single source of "where am I"; shared by weather, night light, and theme auto mode.
    auto locationRows = generateSchemaEntries(noctalia::config::schema::locationSchema(), cfg.location, {"location"});
    appendRow(entries, locationRows, "auto_locate");
    const SettingVisibility autoLocateOff = [](const Config& c) { return !c.location.autoLocate; };
    {
      auto e = makeEntry(
          SettingsSection::Location, "location", tr("settings.schema.services.location-address.label"),
          tr("settings.schema.services.location-address.description"), {"location", "address"},
          TextSetting{
              .value = cfg.location.address,
              .placeholder = tr("settings.schema.services.location-address.placeholder"),
              .browseFileExtensions = {}
          },
          "location address city geocode"
      );
      e.visibleWhen = autoLocateOff;
      entries.push_back(std::move(e));
    }
    // Manual coordinates: shown only when no network location is configured (auto-locate off
    // and no address). The address gate is build-time; the auto-locate gate is evaluated live.
    const SettingVisibility manualLocationHidden = [](const Config&) { return false; };
    const SettingVisibility& manualLocationControlsVisible =
        cfg.location.address.empty() ? autoLocateOff : manualLocationHidden;
    {
      auto e = makeEntry(
          SettingsSection::Location, "location", tr("settings.schema.services.latitude.label"),
          tr("settings.schema.services.latitude.description"), {"location", "latitude"},
          OptionalNumberSetting{cfg.location.latitude, -90.0, 90.0, "52.5200"}, "coordinate location sunrise sunset",
          true
      );
      e.visibleWhen = manualLocationControlsVisible;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Location, "location", tr("settings.schema.services.longitude.label"),
          tr("settings.schema.services.longitude.description"), {"location", "longitude"},
          OptionalNumberSetting{cfg.location.longitude, -180.0, 180.0, "13.4050"}, "coordinate location sunrise sunset",
          true
      );
      e.visibleWhen = manualLocationControlsVisible;
      entries.push_back(std::move(e));
    }

    // Custom scheduling: explicit sunrise/sunset times for night light and theme auto mode.
    {
      auto e = makeEntry(
          SettingsSection::Location, "location", tr("settings.schema.services.custom-schedule.label"),
          tr("settings.schema.services.custom-schedule.description"), {"location", "custom_schedule"},
          ToggleSetting{cfg.location.customSchedule}, "schedule custom time sunrise sunset"
      );
      entries.push_back(std::move(e));
    }
    // Keep both required times editable before activation so the schedule can be valid when enabled.
    appendRow(entries, locationRows, "sunset");
    appendRow(entries, locationRows, "sunrise");

    // Weather: consumes the resolved location.
    auto weatherRows = generateSchemaEntries(noctalia::config::schema::weatherSchema(), cfg.weather, {"weather"});
    appendRow(entries, weatherRows, "enabled");
    const SettingVisibility weatherOn = [](const Config& c) { return c.weather.enabled; };
    {
      auto e = makeEntry(
          SettingsSection::Location, "weather", tr("settings.schema.shell.show-location.label"),
          tr("settings.schema.shell.show-location.description"), {"shell", "show_location"},
          ToggleSetting{cfg.shell.showLocation}, "weather"
      );
      e.visibleWhen = weatherOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Location, "weather", tr("settings.schema.services.weather-unit.label"),
          tr("settings.schema.services.weather-unit.description"), {"weather", "unit"},
          asSegmented(plainSelect(
              {{"metric", "settings.options.weather.unit.metric"},
               {"imperial", "settings.options.weather.unit.imperial"}},
              cfg.weather.unit
          )),
          "temperature"
      );
      e.visibleWhen = weatherOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Location, "weather", tr("settings.schema.services.weather-effects.label"),
          tr("settings.schema.services.weather-effects.description"), {"weather", "effects"},
          ToggleSetting{cfg.weather.effects}, "forecast visuals"
      );
      e.visibleWhen = weatherOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Location, "weather", tr("settings.schema.services.weather-refresh-interval.label"),
          tr("settings.schema.services.weather-refresh-interval.description"), {"weather", "refresh_minutes"},
          sliderFor(cfg.weather.refreshMinutes, noctalia::config::schema::kRefreshMinutesRange, true), "forecast"
      );
      e.visibleWhen = weatherOn;
      entries.push_back(std::move(e));
    }

    if (!env.gammaControlAvailable) {
      entries.push_back(makeEntry(
          SettingsSection::Location, "night-light", tr("settings.schema.services.night-light.label"),
          tr("settings.schema.services.night-light.requires-gamma-control"), {"nightlight", "enabled"},
          ToggleSetting{.checked = cfg.nightlight.enabled, .enabled = false}, "nightlight"
      ));
    } else {
      entries.push_back(makeEntry(
          SettingsSection::Location, "night-light", tr("settings.schema.services.night-light.label"),
          tr("settings.schema.services.night-light.description"), {"nightlight", "enabled"},
          ToggleSetting{cfg.nightlight.enabled}, "nightlight"
      ));
      const SettingVisibility nightLightOn = [](const Config& c) { return c.nightlight.enabled; };
      {
        auto e = makeEntry(
            SettingsSection::Location, "night-light", tr("settings.schema.services.force-night-light.label"),
            tr("settings.schema.services.force-night-light.description"), {"nightlight", "force"},
            ToggleSetting{cfg.nightlight.force}, "nightlight"
        );
        e.visibleWhen = nightLightOn;
        entries.push_back(std::move(e));
      }
      // Both sliders span the same range; the day > night invariant is enforced at commit time
      // via SliderSetting::linkedCommit, which pushes the other temperature when needed.
      const auto tempMin = static_cast<double>(NightLightConfig::kTemperatureMin);
      const auto tempMax = static_cast<double>(NightLightConfig::kTemperatureMax);
      const auto tempStep = static_cast<double>(NightLightConfig::kTemperatureGap);

      SliderSetting daySlider{static_cast<double>(cfg.nightlight.dayTemperature), tempMin, tempMax, tempStep, true};
      daySlider.linkedCommit = [curNight = cfg.nightlight.nightTemperature](double v) {
        std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides;
        const auto newDay = std::clamp(
            static_cast<std::int32_t>(std::lround(v)), NightLightConfig::kTemperatureMin,
            NightLightConfig::kTemperatureMax
        );
        if (newDay - NightLightConfig::kTemperatureGap < curNight) {
          std::int32_t pushedNight =
              std::max(NightLightConfig::kTemperatureMin, newDay - NightLightConfig::kTemperatureGap);
          if (pushedNight + NightLightConfig::kTemperatureGap > newDay) {
            // Day was below kTemperatureMin + kTemperatureGap; bump day up too. The slider value
            // refresh comes through the rebuilt registry on the next config reload.
            const std::int32_t bumpedDay =
                std::min(NightLightConfig::kTemperatureMax, pushedNight + NightLightConfig::kTemperatureGap);
            overrides.emplace_back(
                std::vector<std::string>{"nightlight", "temperature_day"}, static_cast<std::int64_t>(bumpedDay)
            );
          }
          overrides.emplace_back(
              std::vector<std::string>{"nightlight", "temperature_night"}, static_cast<std::int64_t>(pushedNight)
          );
        }
        return overrides;
      };
      {
        auto e = makeEntry(
            SettingsSection::Location, "night-light", tr("settings.schema.services.day-temperature.label"),
            tr("settings.schema.services.day-temperature.description"), {"nightlight", "temperature_day"},
            std::move(daySlider), "nightlight kelvin"
        );
        e.visibleWhen = nightLightOn;
        entries.push_back(std::move(e));
      }

      SliderSetting nightSlider{static_cast<double>(cfg.nightlight.nightTemperature), tempMin, tempMax, tempStep, true};
      nightSlider.linkedCommit = [curDay = cfg.nightlight.dayTemperature](double v) {
        std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides;
        const auto newNight = std::clamp(
            static_cast<std::int32_t>(std::lround(v)), NightLightConfig::kTemperatureMin,
            NightLightConfig::kTemperatureMax
        );
        if (curDay - NightLightConfig::kTemperatureGap < newNight) {
          std::int32_t pushedDay =
              std::min(NightLightConfig::kTemperatureMax, newNight + NightLightConfig::kTemperatureGap);
          if (pushedDay - NightLightConfig::kTemperatureGap < newNight) {
            const std::int32_t bumpedNight =
                std::max(NightLightConfig::kTemperatureMin, pushedDay - NightLightConfig::kTemperatureGap);
            overrides.emplace_back(
                std::vector<std::string>{"nightlight", "temperature_night"}, static_cast<std::int64_t>(bumpedNight)
            );
          }
          overrides.emplace_back(
              std::vector<std::string>{"nightlight", "temperature_day"}, static_cast<std::int64_t>(pushedDay)
          );
        }
        return overrides;
      };
      {
        auto e = makeEntry(
            SettingsSection::Location, "night-light", tr("settings.schema.services.night-temperature.label"),
            tr("settings.schema.services.night-temperature.description"), {"nightlight", "temperature_night"},
            std::move(nightSlider), "nightlight kelvin"
        );
        e.visibleWhen = nightLightOn;
        entries.push_back(std::move(e));
      }
    }

    const SettingVisibility calendarOn = [](const Config& c) { return c.calendar.enabled; };
    auto calendarRows = generateSchemaEntries(noctalia::config::schema::calendarSchema(), cfg.calendar, {"calendar"});
    appendRow(entries, calendarRows, "enabled");
    {
      auto e = makeEntry(
          SettingsSection::Calendar, "general", tr("settings.schema.services.calendar-event-date-format.label"),
          tr("settings.schema.services.calendar-event-date-format.description"), {"calendar", "event_date_format"},
          TextSetting{.value = cfg.calendar.eventDateFormat, .placeholder = "%A %e %B", .browseFileExtensions = {}},
          "calendar date format strftime chrono"
      );
      e.visibleWhen = calendarOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Calendar, "general", tr("settings.schema.services.calendar-event-time-format.label"),
          tr("settings.schema.services.calendar-event-time-format.description"), {"calendar", "event_time_format"},
          TextSetting{.value = cfg.calendar.eventTimeFormat, .placeholder = "%H:%M", .browseFileExtensions = {}},
          "calendar time format strftime chrono"
      );
      e.visibleWhen = calendarOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Calendar, "general", tr("settings.schema.services.calendar-dedupe-events.label"),
          tr("settings.schema.services.calendar-dedupe-events.description"), {"calendar", "dedupe_events"},
          ToggleSetting{cfg.calendar.dedupeEvents}, "calendar events duplicate merge"
      );
      e.visibleWhen = calendarOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Calendar, "general", tr("settings.schema.services.calendar-dedupe-ignore-patterns.label"),
          tr("settings.schema.services.calendar-dedupe-ignore-patterns.description"),
          {"calendar", "dedupe_ignore_patterns"}, ListSetting{.items = cfg.calendar.dedupeIgnorePatterns},
          "calendar events duplicate merge regex title"
      );
      e.visibleWhen = [](const Config& c) { return c.calendar.enabled && c.calendar.dedupeEvents; };
      entries.push_back(std::move(e));
    }
    // Week numbers are a grid decoration, so they stay available when event syncing is off.
    auto calendarTabRows = generateSchemaEntries(
        noctalia::config::schema::calendarTabRowsSchema(), cfg.controlCenter.calendarTab, {"control_center", "calendar"}
    );
    appendGroup(entries, calendarTabRows, "general");
    {
      auto e = makeEntry(
          SettingsSection::Calendar, "general", tr("settings.schema.services.calendar-past-events-threshold.label"),
          tr("settings.schema.services.calendar-past-events-threshold.description"),
          {"control_center", "calendar", "past_events_threshold_minutes"},
          sliderFor(
              cfg.controlCenter.calendarTab.pastEventsThresholdMinutes,
              noctalia::config::schema::kPastEventsThresholdMinutesRange, true
          ),
          "calendar past events threshold minutes hide old finished"
      );
      e.visibleWhen = [](const Config& c) { return c.controlCenter.calendarTab.hidePastEvents; };
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Calendar, "calendar-reminders", tr("settings.schema.services.calendar-reminders.label"),
          tr("settings.schema.services.calendar-reminders.description"), {"calendar", "reminders", "enabled"},
          ToggleSetting{cfg.calendar.reminders.enabled}, "calendar reminder notification alarm upcoming"
      );
      e.visibleWhen = calendarOn;
      entries.push_back(std::move(e));
    }
    const SettingVisibility remindersOn = [](const Config& c) {
      return c.calendar.enabled && c.calendar.reminders.enabled;
    };
    {
      auto e = makeEntry(
          SettingsSection::Calendar, "calendar-reminders",
          tr("settings.schema.services.calendar-reminder-use-event.label"),
          tr("settings.schema.services.calendar-reminder-use-event.description"),
          {"calendar", "reminders", "use_event_reminders"}, ToggleSetting{cfg.calendar.reminders.useEventReminders},
          "calendar reminder valarm override event"
      );
      e.visibleWhen = remindersOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Calendar, "calendar-reminders", tr("settings.schema.services.calendar-reminder-lead.label"),
          tr("settings.schema.services.calendar-reminder-lead.description"),
          {"calendar", "reminders", "default_lead_minutes"},
          sliderFor(
              cfg.calendar.reminders.defaultLeadMinutes, noctalia::config::schema::kReminderLeadMinutesRange, true
          ),
          "calendar reminder lead minutes before default"
      );
      e.visibleWhen = remindersOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Calendar, "calendar-reminders", tr("settings.schema.services.calendar-digest-time.label"),
          tr("settings.schema.services.calendar-digest-time.description"),
          {"calendar", "reminders", "all_day_digest_time"},
          TextSetting{
              .value = cfg.calendar.reminders.allDayDigestTime, .placeholder = "09:00", .browseFileExtensions = {}
          },
          "calendar all day digest time morning"
      );
      e.visibleWhen = remindersOn;
      entries.push_back(std::move(e));
    }
    // Sync cadence belongs with the accounts it drives; the account rows are injected right after it.
    {
      auto e = makeEntry(
          SettingsSection::Calendar, "calendar-accounts",
          tr("settings.schema.services.calendar-refresh-interval.label"),
          tr("settings.schema.services.calendar-refresh-interval.description"), {"calendar", "refresh_minutes"},
          sliderFor(cfg.calendar.refreshMinutes, noctalia::config::schema::kRefreshMinutesRange, true), "calendar sync"
      );
      e.visibleWhen = calendarOn;
      entries.push_back(std::move(e));
    }

    auto audioRows = generateSchemaEntries(noctalia::config::schema::audioSchema(), cfg.audio, {"audio"});
    for (const auto* key : {"enable_overdrive", "auto_switch_hdmi", "enable_sounds"}) {
      appendRow(entries, audioRows, key);
    }
    const std::size_t soundsGatedStart = entries.size();
    entries.push_back(makeEntry(
        SettingsSection::Services, "audio", tr("settings.schema.services.volume-sounds.label"),
        tr("settings.schema.services.volume-sounds.description"), {"audio", "enable_volume_sounds"},
        ToggleSetting{cfg.audio.enableVolumeSounds}, "sound"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Services, "audio", tr("settings.schema.services.notification-sounds.label"),
        tr("settings.schema.services.notification-sounds.description"), {"audio", "enable_notification_sounds"},
        ToggleSetting{cfg.audio.enableNotificationSounds}, "sound"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Services, "audio", tr("settings.schema.services.power-sounds.label"),
        tr("settings.schema.services.power-sounds.description"), {"audio", "enable_power_sounds"},
        ToggleSetting{cfg.audio.enablePowerSounds}, "sound"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Services, "audio", tr("settings.schema.services.screenshot-sounds.label"),
        tr("settings.schema.services.screenshot-sounds.description"), {"audio", "enable_screenshot_sounds"},
        ToggleSetting{cfg.audio.enableScreenshotSounds}, "sound"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Services, "audio", tr("settings.schema.services.sound-volume.label"),
        tr("settings.schema.services.sound-volume.description"), {"audio", "sound_volume"},
        sliderFor(cfg.audio.soundVolume, noctalia::config::schema::kUnitRange, false), "sound"
    ));
    std::vector<SelectOption> soundThemeOptions;
    for (const auto& [value, label] : SoundPlayer::availableThemes()) {
      soundThemeOptions.push_back(SelectOption{value, label});
    }
    if (soundThemeOptions.empty()) {
      soundThemeOptions.push_back(SelectOption{"freedesktop", tr("settings.schema.services.sound-theme.default")});
    }
    entries.push_back(makeEntry(
        SettingsSection::Services, "audio", tr("settings.schema.services.sound-theme.label"),
        tr("settings.schema.services.sound-theme.description"), {"audio", "sound_theme"},
        SelectSetting{std::move(soundThemeOptions), cfg.audio.soundTheme}, "sound theme"
    ));
    for (std::size_t i = soundsGatedStart; i < entries.size(); ++i) {
      SettingVisibility prev = std::move(entries[i].visibleWhen);
      entries[i].visibleWhen = prev
          ? SettingVisibility{[prev = std::move(prev)](const Config& c) { return c.audio.enableSounds && prev(c); }}
          : SettingVisibility{[](const Config& c) { return c.audio.enableSounds; }};
    }
    entries.push_back(makeEntry(
        SettingsSection::Services, "brightness", tr("settings.schema.services.ddcutil.label"),
        env.ddcutilAvailable ? tr("settings.schema.services.ddcutil.description")
                             : tr("settings.schema.services.ddcutil.requires-ddcutil"),
        {"brightness", "enable_ddcutil"},
        ToggleSetting{.checked = cfg.brightness.enableDdcutil, .enabled = env.ddcutilAvailable}, "monitor ddcutil"
    ));
    auto brightnessRows =
        generateSchemaEntries(noctalia::config::schema::brightnessSchema(), cfg.brightness, {"brightness"});
    appendRow(entries, brightnessRows, "minimum_brightness");
    appendRow(entries, brightnessRows, "sync_all_monitors");
    auto bluetoothRows =
        generateSchemaEntries(noctalia::config::schema::bluetoothSchema(), cfg.bluetooth, {"bluetooth"});
    appendGroup(entries, bluetoothRows, "bluetooth");
    entries.push_back(makeEntry(
        SettingsSection::Services, "media", tr("settings.schema.services.mpris-blacklist.label"),
        tr("settings.schema.services.mpris-blacklist.description"), {"shell", "mpris", "blacklist"},
        ListSetting{.items = cfg.shell.mpris.blacklist}, "mpris media player dbus session blacklist"
    ));

    // Power
    appendRow(entries, panelRows, "session_placement");
    entries.push_back(panelPositionEntry(
        SettingsSection::Panels, "session-panel", "session", "settings.schema.panels.position-session.label",
        "settings.schema.panels.position-session.description", cfg.shell.panel.sessionPosition
    ));
    entries.push_back(panelOpenNearClickEntry(
        SettingsSection::Panels, "session-panel", "session", "settings.schema.panels.open-near-click-session.label",
        "settings.schema.panels.open-near-click-session.description", cfg.shell.panel.openNearClickSession,
        &ShellConfig::PanelConfig::sessionPlacement, &ShellConfig::PanelConfig::sessionPosition
    ));
    auto sessionRows =
        generateSchemaEntries(noctalia::config::schema::sessionSchema(), cfg.shell.session, {"shell", "session"});
    appendRow(entries, sessionRows, "grid");
    {
      auto e = makeEntry(
          SettingsSection::Power, "session-panel", tr("settings.schema.power.session-grid-columns.label"),
          tr("settings.schema.power.session-grid-columns.description"), {"shell", "session", "grid_columns"},
          StepperSetting{
              .value = static_cast<int>(cfg.shell.session.gridColumns),
              .minValue = static_cast<int>(noctalia::config::schema::kSessionGridColumnsRange.min.value()),
              .maxValue = static_cast<int>(noctalia::config::schema::kSessionGridColumnsRange.max.value()),
              .step = static_cast<int>(noctalia::config::schema::kSessionGridColumnsRange.step.value()),
          },
          "session panel grid columns per row"
      );
      e.visibleWhen = [](const Config& c) { return c.shell.session.grid; };
      entries.push_back(std::move(e));
    }
    appendRow(entries, sessionRows, "show_shortcuts");
    entries.push_back(makeEntry(
        SettingsSection::Power, "session-panel", tr("settings.schema.power.session-actions.label"),
        tr("settings.schema.power.session-actions.description"), {"shell", "session", "actions"},
        SessionPanelActionsSetting{.items = cfg.shell.session.actions},
        "session panel power menu logout reboot shutdown lock command actions order"
    ));

    // Idle
    entries.push_back(makeEntry(
        SettingsSection::Power, "idle", tr("settings.schema.idle.pre-action-fade.label"),
        tr("settings.schema.idle.pre-action-fade.description"), {"idle", "pre_action_fade_seconds"},
        StepperSetting{
            .value = static_cast<int>(std::lround(std::clamp(cfg.idle.preActionFadeSeconds, 0.0F, 30.0F))),
            .minValue = 0,
            .maxValue = 30,
            .step = 1,
            .valueSuffix = "s"
        },
        "idle fade dim seconds overlay"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Power, "idle", tr("settings.schema.idle.behaviors.label"),
        tr("settings.schema.idle.behaviors.description"), {"idle", "behavior"},
        IdleBehaviorsSetting{.items = cfg.idle.behaviors},
        "idle behavior timeout command resume screen lock dpms suspend lock_and_suspend caffeine"
    ));

    // Hooks
    auto hookGroup = [](HookKind kind) -> std::string {
      switch (kind) {
      case HookKind::Started:
      case HookKind::SessionLocked:
      case HookKind::SessionUnlocked:
      case HookKind::LoggingOut:
      case HookKind::Rebooting:
      case HookKind::ShuttingDown:
        return "lifecycle";
      case HookKind::WallpaperChanged:
      case HookKind::ColorsChanged:
      case HookKind::ThemeModeChanged:
        return "theme";
      case HookKind::WifiEnabled:
      case HookKind::WifiDisabled:
      case HookKind::BluetoothEnabled:
      case HookKind::BluetoothDisabled:
        return "network";
      case HookKind::BatteryCharging:
      case HookKind::BatteryDischarging:
      case HookKind::BatteryPlugged:
      case HookKind::BatteryPercentageChanged:
      case HookKind::PowerProfileChanged:
        return "power";
      case HookKind::Count:
        break;
      }
      return "general";
    };

    auto hookTags = [](HookKind kind) -> std::string {
      std::string tags = "hook command script exec event trigger";
      if (kind == HookKind::BatteryCharging
          || kind == HookKind::BatteryDischarging
          || kind == HookKind::BatteryPlugged
          || kind == HookKind::BatteryPercentageChanged) {
        tags += " battery power";
      }
      if (kind == HookKind::PowerProfileChanged) {
        tags += " power profile performance balanced saver";
      }
      if (kind == HookKind::WallpaperChanged || kind == HookKind::ColorsChanged || kind == HookKind::ThemeModeChanged) {
        tags += " wallpaper colors theme mode light dark auto";
      }
      if (kind == HookKind::WifiEnabled
          || kind == HookKind::WifiDisabled
          || kind == HookKind::BluetoothEnabled
          || kind == HookKind::BluetoothDisabled) {
        tags += " network wifi bluetooth";
      }
      if (kind == HookKind::SessionLocked
          || kind == HookKind::SessionUnlocked
          || kind == HookKind::LoggingOut
          || kind == HookKind::Rebooting
          || kind == HookKind::ShuttingDown
          || kind == HookKind::Started) {
        tags += " session startup";
      }
      return tags;
    };

    for (const auto& kind : kHookKinds) {
      const auto index = static_cast<std::size_t>(kind.value);
      const std::string key(kind.key);
      const std::string baseKey = "settings.schema.hooks.events." + i18n::keySegment(key);
      const std::string hookCmd = cfg.hooks.commands[index].empty() ? "" : cfg.hooks.commands[index][0];
      entries.push_back(makeEntry(
          SettingsSection::Hooks, hookGroup(kind.value), tr(baseKey + ".label"), tr(baseKey + ".description"),
          {"hooks", key},
          TextSetting{
              .value = hookCmd,
              .placeholder = tr("settings.schema.hooks.command-placeholder"),
              .width = 320.0F,
              .browseFileExtensions = {}
          },
          hookTags(kind.value)
      ));
    }

    // Notifications
    auto notificationRows =
        generateSchemaEntries(noctalia::config::schema::notificationSchema(), cfg.notification, {"notification"});
    appendGroup(entries, notificationRows, "general");
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "toasts", tr("settings.schema.notifications.layer.label"),
        tr("settings.schema.notifications.layer.description"), {"notification", "layer"},
        asSegmented(plainSelect(
            {{"top", "settings.options.layer.top"}, {"overlay", "settings.options.layer.overlay"}},
            cfg.notification.layer
        )),
        "toast layer shell z-order"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "toasts", tr("settings.schema.notifications.position.label"),
        tr("settings.schema.notifications.position.description"), {"notification", "position"},
        plainSelect(
            {{"top_right", "settings.options.screen-position.top-right"},
             {"top_left", "settings.options.screen-position.top-left"},
             {"top_center", "settings.options.screen-position.top-center"},
             {"bottom_right", "settings.options.screen-position.bottom-right"},
             {"bottom_left", "settings.options.screen-position.bottom-left"},
             {"bottom_center", "settings.options.screen-position.bottom-center"}},
            cfg.notification.position
        ),
        "toast popup placement anchor"
    ));
    for (const auto* key : {"scale", "width", "offset_x", "offset_y", "max_visible", "background_opacity", "border"}) {
      appendRow(entries, notificationRows, key);
    }
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "toasts", tr("settings.schema.notifications.border-color.label"),
        tr("settings.schema.notifications.border-color.description"), {"notification", "border_color"},
        colorSpecPicker(cfg.notification.borderColor), "outline border color theme accent"
    ));
    appendGroup(entries, notificationRows, "toasts");
    {
      auto e = makeEntry(
          SettingsSection::Notifications, "toasts", tr("settings.schema.notifications.monitors.label"),
          tr("settings.schema.notifications.monitors.description"), {"notification", "monitors"},
          ListSetting{.items = cfg.notification.monitors, .suggestedOptions = env.availableOutputs},
          "monitor output display screen"
      );
      e.visibleWhen = [](const Config& c) { return !c.notification.followFocusedOutput; };
      entries.push_back(std::move(e));
    }
    appendGroup(entries, notificationRows, "history");
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "filtering", tr("settings.schema.notifications.filters.label"),
        tr("settings.schema.notifications.filters.description"), {"notification", "filter"},
        NotificationFiltersSetting{.items = cfg.notification.filters},
        "filter blacklist suppress toast history sound dnd bypass do not disturb app name desktop entry category "
        "urgency"
    ));

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

    // Integrity guard: every override path must resolve to a real schema key, else
    // the entry silently reads/writes a dead override. Build-determined, so checked
    // once per process; warn-only. (Visibility predicates are compiler-checked.)
    {
      static bool verified = false;
      if (!verified) {
        verified = true;
        const Logger log("settings");
        const auto verify = [&](const std::vector<std::string>& path, std::string_view what) {
          if (!path.empty() && !noctalia::config::schema::isKnownConfigPath(path)) {
            std::string dotted;
            for (const auto& seg : path) {
              dotted += (dotted.empty() ? "" : ".") + seg;
            }
            log.warn("settings registry {} path does not resolve to a schema key: {}", what, dotted);
          }
        };
        for (const auto& entry : entries) {
          if (!std::holds_alternative<ButtonSetting>(entry.control)) {
            verify(entry.path, "override");
          }
        }
      }
    }

    return entries;
  }

} // namespace settings
