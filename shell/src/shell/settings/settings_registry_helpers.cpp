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
  namespace registry_detail {

    constexpr int kBarMarginMax = 4096;
    constexpr float kBarCornerRadiusMax = 80.0F;

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
    colorSpecPicker(const std::optional<ColorSpec>& selected, bool allowNone, std::string noneLabel) {
      return ColorSpecPickerSetting{
          .roles = {},
          .selectedValue = optionalColorSpecConfigValue(selected),
          .allowNone = allowNone,
          .allowCustomColor = true,
          .noneLabel = std::move(noneLabel),
      };
    }


    ColorSpecPickerSetting
    colorSpecPicker(const ColorSpec& selected, bool allowNone, std::string noneLabel) {
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
        std::vector<std::string> path, SettingControl control, const std::string& tags, bool advanced
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


  } // namespace registry_detail
} // namespace settings
