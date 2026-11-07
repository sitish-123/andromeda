#include "shell/settings/settings_pages.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>
#include <variant>

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
    constexpr PageGroup kPers = PageGroup::Personalize;
    constexpr PageGroup kDev = PageGroup::Device;
    constexpr PageGroup kApps = PageGroup::WindowsAndApps;
    constexpr PageGroup kSys = PageGroup::System;
    constexpr PageGroup kMore = PageGroup::More;

    // IA order (.agent/mockups/settings-ia.html).
    const std::array<SettingsPage, 30> kPages{{
        {"wifi", kConn, "wifi", {}},
        {"bluetooth", kConn, "bluetooth", {}},
        {"network", kConn, "world", {}},
        {"appearance", kPers, "palette", kAppearance},
        {"wallpaper", kPers, "photo", kWallpaper},
        {"lock-screen", kPers, "lock", {}},
        {"desktop-dock", kPers, "layout-bottombar", kDesktopDock},
        {"bar", kPers, "crop-3-2", kBar},
        {"notifications", kPers, "bell", kNotifications},
        {"displays", kDev, "device-desktop", kDisplays},
        {"sound", kDev, "volume", {}},
        {"power", kDev, "bolt", kPower},
        {"keyboard", kDev, "keyboard", kKeyboard},
        {"mouse", kDev, "mouse", {}},
        {"devices", kDev, "device-mobile", kDevices},
        {"windows", kApps, "window", kWindows},
        {"launcher", kApps, "rocket", kLauncher},
        {"control-center", kApps, "adjustments", kControlCenter},
        {"screenshot", kApps, "screenshot", kScreenshot},
        {"default-apps", kApps, "apps", kDefaultApps},
        {"privacy", kSys, "shield-lock", kPrivacy},
        {"date-time", kSys, "clock", kDateTime},
        {"language", kSys, "language", kLanguage},
        {"software-update", kSys, "download", {}},
        {"about", kSys, "info-circle", kAbout},
        {"services", kMore, "adjustments-horizontal", kServices},
        {"plugins", kMore, "puzzle", kPlugins},
        {"automation", kMore, "link", kAutomation},
        {"problems", kMore, "alert-triangle", {}},
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
    constexpr std::array<PageRule, 9> kPageRules{{
        {"lockscreen.", "lock-screen"},
        {"lockscreen_widgets.", "lock-screen"},
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
        "shell.automount_drives", "theme.mode", "theme.builtin", "theme.community_palette", "theme.custom_palette", "theme.from_wallpaper", "andromeda.appearance.reduce_transparency", "accessibility.ui_scale",
        "shell.font_family",
        "wallpaper.enabled", "wallpaper.fill_mode", "wallpaper.directory", "wallpaper.transition",
        "wallpaper.automation.enabled", "wallpaper.automation.interval_seconds",
        "brightness.sync_all_monitors", "brightness.automatic",
        "audio.enable_sounds", "audio.sound_volume", "audio.auto_switch_hdmi", "bluetooth.connect_when_enabled",
        "notification.enable_daemon", "notification.show_app_name",
        "notification.keep_dismissed_in_history", "osd.enabled", "osd.position",
        "dock.enabled", "dock.icon_size", "dock.magnification", "dock.position", "dock.auto_hide", "dock.show_running",
        "desktop_widgets.enabled", "shell.screen_corners.enabled",
        "bar.*.enabled", "bar.*.position", "bar.*.auto_hide", "bar.*.auto_hide_delay_ms",
        "bar.*.show_on_workspace_switch", "bar.*.reserve_space", "bar.*.layer", "bar.*.islands", "bar.*.thickness",
        "bar.*.scale", "bar.*.show_tooltip", "bar.*.start", "bar.*.center", "bar.*.end",
        "control_center.shortcuts", "control_center.show_shortcut_labels", "control_center.show_session_button",
        "shell.launcher.layout", "shell.launcher.width", "shell.launcher.search_position",
        "shell.launcher.default_view", "shell.launcher.show_icons", "shell.launcher.app_grid",
        "shell.launcher.categories", "shell.launcher.sort_by_usage", "shell.launcher.track_usage",
        "shell.launcher.pinned",
        "andromeda.layout.mode", "andromeda.layout.gap", "andromeda.input.focus.follows_mouse",
        "andromeda.input.window_snapping", "andromeda.layout.scrolling.center_focused",
        "andromeda.layout.scrolling.center_underfull_strip", "andromeda.layout.dwindle.preserve_split",
        "andromeda.layout.master.position", "andromeda.layout.master.default_width_fraction",
        "andromeda.layout.master.new_on_top", "andromeda.layout.master.new_becomes_master",
        "andromeda.workspaces.back_and_forth", "andromeda.workspaces.empty_above", "andromeda.workspaces.per-workspace",
        "hot_corners.enabled",
        "shell.window_switcher.style", "andromeda.overview.style", "andromeda.overview.titles", "andromeda.overview.zoom",
        "shell.session.actions", "idle.behavior", "andromeda.general.optimize_background_apps",
        "andromeda.general.freeze_background_apps", "andromeda.general.freeze_hidden_apps",
        "lockscreen.enabled", "lockscreen.lock_before_suspend", "lockscreen.grace_period_seconds",
        "lockscreen.face", "lockscreen.face_on_request", "lockscreen.fingerprint", "lockscreen.theme", "lockscreen.layout", "lockscreen.clock_style",
        "lockscreen.notification_content",
        "shell.animation.enabled", "shell.animation.speed", "accessibility.high_contrast",
        "shell.window_switcher.mru", "shell.window_switcher.show_all_outputs",
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
    // Rows that share one uncaptioned card (a group starting with '~' has no caption), in the mockup's order.
    struct GroupRule {
      std::string_view path;
      std::string_view group;
    };
    constexpr auto kGroupRules = std::to_array<GroupRule>({
        {"theme.mode", "~mode"},
        {"theme.builtin", "~colour"},
        {"theme.from_wallpaper", "~colour"},
        {"andromeda.appearance.reduce_transparency", "~colour"},
        {"accessibility.ui_scale", "~text"},
        {"shell.font_family", "~text"},
        {"dock.icon_size", "~dock"},
        {"dock.magnification", "~dock"},
        {"dock.position", "~dock"},
        {"dock.auto_hide", "~dock"},
        {"dock.show_running", "~dock"},
        {"lockscreen.enabled", "~lock"},
        {"lockscreen.lock_before_suspend", "~lock"},
        {"lockscreen.grace_period_seconds", "~lock"},
        {"lockscreen.face", "unlock"},
        {"lockscreen.face_on_request", "unlock"},
        {"lockscreen.fingerprint", "unlock"},
        {"lockscreen.theme", "lock-look"},
        {"lockscreen.layout", "lock-look"},
        {"lockscreen.clock_style", "lock-look"},
        {"lockscreen.notification_content", "lock-look"},
        {"desktop_widgets.enabled", "desktop"},
        {"shell.screen_corners.enabled", "desktop"},
    });
    // Hooks are scripts the user writes; the whole Automation page is main.
    constexpr std::string_view kHookPrefix = "hooks.";

    std::string mainRowKey(const SettingEntry& entry) {
      if (entry.path.empty()) {
        const auto* toggle = std::get_if<ToggleSetting>(&entry.control);
        return toggle != nullptr ? toggle->id : std::string{};
      }
      std::string key;
      for (std::size_t i = 0; i < entry.path.size(); ++i) {
        key += (i == 0 ? "" : ".");
        key += (i == 1 && entry.path[0] == "bar") ? std::string("*") : entry.path[i];
      }
      return key;
    }

  } // namespace

  const SettingsPage& settingsPageForEntry(const SettingEntry& entry) {
    // The lock screen editor button has no path; it shares the group of the lock rows.
    if (entry.section == SettingsSection::Security && (entry.group == "lock-screen" || entry.group == "~lock")) {
      return *settingsPageFromId("lock-screen");
    }
    const std::string key = settingEntryKey(entry);
    for (const auto& rule : kPageRules) {
      if (key.starts_with(rule.prefix)) {
        return *settingsPageFromId(rule.page);
      }
    }
    return settingsPageForSection(entry.section);
  }

  int settingGroupOrder(const SettingEntry& entry) {
    const std::string key = mainRowKey(entry);
    const auto it = std::ranges::find(kGroupRules, key, &GroupRule::path);
    return it == kGroupRules.end() ? -1 : static_cast<int>(it - kGroupRules.begin());
  }

  std::string_view settingGroupOverride(const SettingEntry& entry) {
    const std::string key = mainRowKey(entry);
    const auto it = std::ranges::find(kGroupRules, key, &GroupRule::path);
    return it == kGroupRules.end() ? std::string_view{} : it->group;
  }

  bool settingEntryIsExpert(const SettingEntry& entry) {
    if (entry.advanced) {
      return true;
    }
    // Editor buttons (desktop widgets, lock screen) open whole tools; they never hide in Advanced.
    if (entry.path.empty() && std::holds_alternative<ButtonSetting>(entry.control)) {
      return false;
    }
    const std::string key = mainRowKey(entry);
    return !key.starts_with(kHookPrefix) && !std::ranges::contains(kMainRows, key);
  }

  bool settingEntryShowsCaption(const SettingEntry& entry) {
    return settingEntryIsExpert(entry);
  }

  std::span<const SettingsPage> settingsPages() { return kPages; }

  bool settingsPageHasOwnContent(std::string_view pageId) { return pageId == "wifi" || pageId == "bluetooth" || pageId == "network" || pageId == "sound" || pageId == "mouse" || pageId == "users" || pageId == "problems" || pageId == "software-update"; }

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
    constexpr std::array<std::string_view, 6> kIds{"connectivity", "personalize", "device", "apps", "system", "more"};
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
