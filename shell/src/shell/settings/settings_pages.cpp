#include "shell/settings/settings_pages.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>

namespace settings {
  namespace {

    using S = SettingsSection;

    constexpr std::array<S, 4> kAppearance{S::Appearance, S::WindowStyle, S::TextScale, S::Motion};
    constexpr std::array<S, 1> kWallpaper{S::Wallpaper};
    constexpr std::array<S, 1> kDisplays{S::Displays};
    constexpr std::array<S, 1> kNotifications{S::Notifications};
    constexpr std::array<S, 2> kDesktopDock{S::Dock, S::Desktop};
    constexpr std::array<S, 1> kBar{S::Bar};
    constexpr std::array<S, 2> kControlCenter{S::ControlCenter, S::Panels};
    constexpr std::array<S, 1> kLauncher{S::Launcher};
    constexpr std::array<S, 4> kWindows{S::Windows, S::Layout, S::Overview, S::Niri};
    // The input page holds keyboard and pointer options in one block until it is split.
    constexpr std::array<S, 3> kKeyboard{S::Keybinds, S::Shortcuts, S::Input};
    constexpr std::array<S, 1> kDevices{S::Devices};
    constexpr std::array<S, 1> kPower{S::Power};
    constexpr std::array<S, 2> kPrivacy{S::Security, S::Location};
    constexpr std::array<S, 2> kDateTime{S::DateTime, S::Calendar};
    constexpr std::array<S, 1> kLanguage{S::Language};
    constexpr std::array<S, 1> kDefaultApps{S::DefaultApps};
    constexpr std::array<S, 1> kScreenshot{S::Screenshot};
    constexpr std::array<S, 1> kServices{S::Services};
    constexpr std::array<S, 1> kPlugins{S::Plugins};
    constexpr std::array<S, 1> kAutomation{S::Hooks};
    constexpr std::array<S, 2> kAbout{S::System, S::Shell};

    constexpr PageGroup kConn = PageGroup::Connectivity;
    constexpr PageGroup kLook = PageGroup::LookAndFeel;
    constexpr PageGroup kDesk = PageGroup::Desktop;
    constexpr PageGroup kSys = PageGroup::System;

    // IA order (DESIGN.md "Settings information architecture").
    const std::array<SettingsPage, 27> kPages{{
        {"wifi", kConn, "wifi", {}},
        {"bluetooth", kConn, "bluetooth", {}},
        {"network", kConn, "world", {}},
        {"appearance", kLook, "palette", kAppearance},
        {"wallpaper", kLook, "photo", kWallpaper},
        {"displays", kLook, "device-desktop", kDisplays},
        {"sound", kLook, "volume", {}},
        {"notifications", kLook, "bell", kNotifications},
        {"desktop-dock", kDesk, "layout-bottombar", kDesktopDock},
        {"bar", kDesk, "crop-3-2", kBar},
        {"control-center", kDesk, "adjustments", kControlCenter},
        {"launcher", kDesk, "rocket", kLauncher},
        {"windows", kDesk, "window", kWindows},
        {"keyboard", kDesk, "keyboard", kKeyboard},
        {"mouse", kDesk, "mouse", {}},
        {"devices", kDesk, "device-mobile", kDevices},
        {"power", kSys, "bolt", kPower},
        {"privacy", kSys, "shield-lock", kPrivacy},
        {"date-time", kSys, "clock", kDateTime},
        {"language", kSys, "language", kLanguage},
        {"default-apps", kSys, "apps", kDefaultApps},
        {"screenshot", kSys, "screenshot", kScreenshot},
        {"services", kSys, "adjustments-horizontal", kServices},
        {"plugins", kSys, "puzzle", kPlugins},
        {"automation", kSys, "link", kAutomation},
        {"about", kSys, "info-circle", kAbout},
        {"users", kSys, "user", {}, false},
    }};

    // Legacy section ids that no longer name a page of their own.
    struct Alias {
      std::string_view id;
      std::string_view page;
    };
    constexpr std::array<Alias, 8> kAliases{{
        {"dock", "desktop-dock"},
        {"desktop", "desktop-dock"},
        {"panels", "control-center"},
        {"keybinds", "keyboard"},
        {"shortcuts", "keyboard"},
        {"input", "keyboard"},
        {"security", "privacy"},
        {"location", "privacy"},
    }};

    // A path prefix that moves its rows to another page; the first match wins.
    struct PageRule {
      std::string_view prefix;
      std::string_view page;
    };
    constexpr std::array<PageRule, 7> kPageRules{{
        {"nightlight.", "displays"},
        {"brightness.", "displays"},
        {"audio.", "sound"},
        {"bluetooth.", "bluetooth"},
        {"shell.time_format", "date-time"},
        {"shell.date_format", "date-time"},
        {"shell.keyboard_layout.", "keyboard"},
    }};

    // Rows a typical user changes and that have a visual control. Bar rows are keyed `bar.*.<key>`.
    constexpr auto kMainRows = std::to_array<std::string_view>({
        "theme.mode", "theme.source", "theme.sync_color_scheme", "theme.toolkit_themes", "shell.corner_radius_scale",
        "accessibility.ui_scale", "accessibility.high_contrast", "shell.font_family", "shell.animation.enabled",
        "andromeda.appearance.reduce_motion", "andromeda.appearance.reduce_transparency",
        "andromeda.appearance.glass_opacity", "andromeda.appearance.blur.enabled",
        "andromeda.appearance.shadow.enabled", "andromeda.appearance.corner_radius",
        "wallpaper.enabled", "wallpaper.fill_mode", "wallpaper.directory", "wallpaper.transition",
        "wallpaper.automation.enabled", "wallpaper.automation.interval_seconds",
        "nightlight.enabled", "brightness.sync_all_monitors",
        "audio.enable_sounds", "audio.sound_volume", "audio.auto_switch_hdmi", "bluetooth.connect_when_enabled",
        "notification.enable_daemon", "notification.position", "notification.show_app_name",
        "notification.keep_dismissed_in_history", "osd.enabled", "osd.position",
        "dock.enabled", "dock.position", "dock.icon_size", "dock.magnification", "dock.magnification_scale",
        "dock.auto_hide", "dock.show_running", "dock.show_dots", "dock.pinned", "desktop_widgets.enabled",
        "shell.screen_corners.enabled",
        "bar.*.enabled", "bar.*.position", "bar.*.auto_hide", "bar.*.auto_hide_delay_ms",
        "bar.*.show_on_workspace_switch", "bar.*.reserve_space", "bar.*.layer", "bar.*.islands", "bar.*.thickness",
        "bar.*.scale", "bar.*.show_tooltip", "bar.*.start", "bar.*.center", "bar.*.end",
        "control_center.shortcuts", "control_center.show_shortcut_labels", "control_center.show_session_button",
        "shell.launcher.layout", "shell.launcher.width", "shell.launcher.search_position",
        "shell.launcher.default_view", "shell.launcher.show_icons", "shell.launcher.app_grid",
        "shell.launcher.categories", "shell.launcher.sort_by_usage", "shell.launcher.track_usage",
        "shell.launcher.pinned",
        "andromeda.layout.mode", "andromeda.layout.gap", "andromeda.input.focus.follows_mouse",
        "andromeda.workspaces.back_and_forth", "hot_corners.enabled", "shell.window_switcher.style",
        "andromeda.overview.zoom",
        "andromeda.general.mod_key", "shell.keyboard_layout.custom_labels",
        "shell.session.actions", "idle.behavior",
        "lockscreen.enabled", "lockscreen.lock_before_suspend", "lockscreen.grace_period_seconds",
        "shell.offline_mode", "shell.telemetry_enabled", "location.auto_locate", "location.address",
        "weather.enabled",
        "calendar.enabled", "calendar.reminders.enabled", "control_center.calendar.show_week_numbers",
        "shell.time_format", "shell.date_format",
        "shell.screenshot.save_to_file", "shell.screenshot.copy_to_clipboard", "shell.screenshot.directory",
        "shell.screenshot.format", "shell.screenshot.annotate", "shell.screenshot.show_cursor",
        "shell.screenshot.freeze_screen",
        "shell.clipboard_enabled",
        "shell.avatar_path", "shell.settings_window_translucent",
    });
    // Hooks are scripts the user writes; the whole Automation page is main.
    constexpr std::string_view kHookPrefix = "hooks.";

    std::string mainRowKey(const SettingEntry& entry) {
      std::string key;
      for (std::size_t i = 0; i < entry.path.size(); ++i) {
        key += (i == 0 ? "" : ".");
        key += (i == 1 && entry.path[0] == "bar") ? std::string("*") : entry.path[i];
      }
      return key;
    }

  } // namespace

  const SettingsPage& settingsPageForEntry(const SettingEntry& entry) {
    const std::string key = settingEntryKey(entry);
    for (const auto& rule : kPageRules) {
      if (key.starts_with(rule.prefix)) {
        return *settingsPageFromId(rule.page);
      }
    }
    return settingsPageForSection(entry.section);
  }

  bool settingEntryIsExpert(const SettingEntry& entry) {
    if (entry.advanced || entry.path.empty()) {
      return true;
    }
    const std::string key = mainRowKey(entry);
    return !key.starts_with(kHookPrefix) && !std::ranges::contains(kMainRows, key);
  }

  bool settingEntryShowsCaption(const SettingEntry& entry) {
    return settingEntryIsExpert(entry);
  }

  std::span<const SettingsPage> settingsPages() { return kPages; }

  const SettingsPage* settingsPageFromId(std::string_view id) {
    if (const auto* it = std::ranges::find(kPages, id, &SettingsPage::id); it != kPages.end()) {
      return &*it;
    }
    if (const auto* alias = std::ranges::find(kAliases, id, &Alias::id); alias != kAliases.end()) {
      return settingsPageFromId(alias->page);
    }
    if (const auto section = settingsSectionFromId(id); section.has_value()) {
      return &settingsPageForSection(*section);
    }
    const std::string canonical = canonicalSettingsSectionId(id);
    return canonical == id ? nullptr : settingsPageFromId(canonical);
  }

  const SettingsPage& settingsPageForSection(SettingsSection section) {
    for (const auto& page : kPages) {
      if (settingsPageShowsSection(page, section)) {
        return page;
      }
    }
    std::abort(); // a section without a page is a table bug
  }

  bool settingsPageShowsSection(const SettingsPage& page, SettingsSection section) {
    return std::ranges::contains(page.sections, section);
  }

  bool selectedPageShows(std::string_view selectedPageId, std::string_view sectionId) {
    const auto section = settingsSectionFromId(sectionId);
    const auto* page = settingsPageFromId(selectedPageId);
    return section.has_value() && page != nullptr && settingsPageShowsSection(*page, *section);
  }

  std::string settingsPageGroupLabelKey(PageGroup group) {
    constexpr std::array<std::string_view, 4> kIds{"connectivity", "look", "desktop", "system"};
    return "settings.pages.group-" + std::string(kIds[static_cast<std::size_t>(group)]);
  }

  std::string settingsPageLabelKey(const SettingsPage& page) { return "settings.pages." + std::string(page.id); }

  bool settingEntryOnPage(
      const SettingEntry& entry, std::string_view selectedPageId, std::string_view selectedBarName,
      std::string_view selectedMonitorOverride
  ) {
    const auto* page = settingsPageFromId(selectedPageId);
    if (page == nullptr || &settingsPageForEntry(entry) != page) {
      return false;
    }
    if (entry.section == SettingsSection::Bar) {
      return settingEntryMatchesBarNavigation(entry, selectedBarName, selectedMonitorOverride);
    }
    if (entry.section == SettingsSection::Dock) {
      return settingEntryMatchesDockNavigation(entry, selectedMonitorOverride);
    }
    return true;
  }

  std::string settingEntryKey(const SettingEntry& entry) {
    if (entry.path.empty()) {
      return std::string(settingsSectionId(entry.section)) + "/" + entry.title;
    }
    std::string key;
    for (const auto& part : entry.path) {
      key += key.empty() ? part : "." + part;
    }
    return key;
  }

} // namespace settings
