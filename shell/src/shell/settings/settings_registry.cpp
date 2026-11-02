#include "shell/settings/settings_registry.h"
#include "shell/settings/settings_pages.h"
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

  namespace {

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
    RegistryContext ctx{.cfg = cfg, .env = env};
    ctx.positionSelect = positionSelect;
    auto& entries = ctx.entries;

    add_appearance_settings(ctx);
    add_wallpaper_settings(ctx);
    add_templates_settings(ctx);
    add_dock_settings(ctx);
    add_panels_settings(ctx);
    add_controlcenter_settings(ctx);
    add_desktop_settings(ctx);
    add_security_settings(ctx);
    add_shell_settings(ctx);
    add_system_settings(ctx);
    add_power_settings(ctx);
    add_notifications_settings(ctx);
    add_osd_settings(ctx);
    add_bar_settings(ctx);
    add_baroverrides_settings(ctx);

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


    for (auto& entry : ctx.entries) {
      if (const auto group = settingGroupOverride(entry); !group.empty()) {
        entry.group = std::string(group);
        if (entry.group.front() == '~' && settingsPageForSection(entry.section).id == "appearance") {
          entry.section = SettingsSection::Appearance;
        }
      }
      if (auto* select = std::get_if<SelectSetting>(&entry.control);
          select != nullptr
          && (entry.path == std::vector<std::string>{"theme", "mode"}
              || entry.path == std::vector<std::string>{"theme", "builtin"})) {
        select->tiles = true;
      }
    }

    // Grouped rows swap into each other's slots so each card lists them in the table's order.
    std::vector<std::size_t> slots;
    for (std::size_t i = 0; i < ctx.entries.size(); ++i) {
      if (settingGroupOrder(ctx.entries[i]) >= 0) {
        slots.push_back(i);
      }
    }
    std::vector<SettingEntry> grouped;
    for (const std::size_t slot : slots) {
      grouped.push_back(std::move(ctx.entries[slot]));
    }
    std::ranges::stable_sort(grouped, {}, [](const SettingEntry& e) { return settingGroupOrder(e); });
    for (std::size_t i = 0; i < slots.size(); ++i) {
      ctx.entries[slots[i]] = std::move(grouped[i]);
    }

    return std::move(ctx.entries);
  }

} // namespace settings
