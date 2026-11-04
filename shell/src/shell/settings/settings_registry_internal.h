#pragma once

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
  namespace registry_detail {

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


    [[nodiscard]] SliderSetting barCornerSlider(std::int32_t value);
    [[nodiscard]] std::string deadZoneDefault(noctalia::bar::Gesture gesture);
    [[nodiscard]] SliderSetting barReservedSlider(double value, double maxValue, double step, bool integer);
    [[nodiscard]] std::vector<KeyChord>
    effectiveKeybindItems(const std::vector<KeyChord>& configured, KeybindAction action);
    SelectSetting asSegmented(SelectSetting setting);
    StepperSetting barMarginStepper(std::int32_t value);
    StepperSetting barPanelOverlapStepper(std::int32_t value);
    std::optional<int> radiusStepperValue(const std::optional<double>& value);
    SelectSetting
    plainSelect(std::initializer_list<std::pair<std::string_view, std::string_view>> items, std::string_view selected);
    [[nodiscard]] std::string_view barAutoHideMode(bool autoHide, bool smartAutoHide);
    [[nodiscard]] SelectSetting autoHideModeSelect(std::string_view mode, std::vector<std::string> smartPath);
    ColorSwatchPreview palettePreviewFromPalette(const noctalia::theme::FixedPalette& palette);
    ColorSwatchPreview builtinPalettePreview(const noctalia::theme::BuiltinPalette& palette, ThemeMode mode);
    SelectSetting builtinPaletteSelect(std::string_view selected, ThemeMode mode);
    SelectSetting wallpaperSchemeSelect(std::string_view selected);
    std::vector<SelectOption> controlCenterShortcutOptions(const Config& cfg);
    SelectSetting languageSelect(std::string_view selected);
    ColorSpecPickerSetting
    colorSpecPicker(const std::optional<ColorSpec>& selected, bool allowNone, std::string noneLabel = {});
    ColorSpecPickerSetting
    colorSpecPicker(const ColorSpec& selected, bool allowNone = false, std::string noneLabel = {});
    std::string pathText(const std::vector<std::string>& path);
    SettingEntry makeEntry(
        SettingsSection section, std::string group, std::string title, std::string subtitle,
        std::vector<std::string> path, SettingControl control, const std::string& tags = {}, bool advanced = false
    );
    void appendGroup(std::vector<SettingEntry>& entries, std::vector<SettingEntry>& generated, std::string_view group);
    void appendRow(std::vector<SettingEntry>& entries, std::vector<SettingEntry>& generated, std::string_view key);

    inline int radiusStepperFallback(const std::optional<double>& value) { return radiusStepperValue(value).value_or(8); }

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


    struct RegistryContext {
      const Config& cfg;
      const RegistryEnvironment& env;
      std::vector<SettingEntry> entries;
      std::function<SelectSetting(std::string_view)> positionSelect;
      std::vector<SettingEntry> shellRows;
      std::vector<SettingEntry> panelRows;
      std::function<SettingEntry(
          SettingsSection, std::string, std::string_view, std::string_view, std::string_view, const std::string&
      )>
          panelPositionEntry;
      std::function<SettingEntry(
          SettingsSection, std::string, std::string_view, std::string_view, std::string_view, bool,
          PanelPlacement ShellConfig::PanelConfig::*, std::string ShellConfig::PanelConfig::*
      )>
          panelOpenNearClickEntry;
    };

    void addCompositorEntries(std::vector<SettingEntry>& entries, const SettingsControl& compositor);
    void add_appearance_settings(RegistryContext& ctx);
    void add_wallpaper_settings(RegistryContext& ctx);
    void add_templates_settings(RegistryContext& ctx);
    void add_dock_settings(RegistryContext& ctx);
    void add_panels_settings(RegistryContext& ctx);
    void add_controlcenter_settings(RegistryContext& ctx);
    void add_desktop_settings(RegistryContext& ctx);
    void add_security_settings(RegistryContext& ctx);
    void add_shell_settings(RegistryContext& ctx);
    void add_system_settings(RegistryContext& ctx);
    void add_power_settings(RegistryContext& ctx);
    void add_notifications_settings(RegistryContext& ctx);
    void add_osd_settings(RegistryContext& ctx);
    void add_bar_settings(RegistryContext& ctx);
    void add_baroverrides_settings(RegistryContext& ctx);

  } // namespace registry_detail
} // namespace settings
