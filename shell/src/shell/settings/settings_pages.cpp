#include "shell/settings/settings_pages.h"

#include <algorithm>
#include <array>
#include <cstdlib>

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

  } // namespace

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

  std::string settingsPageLabelKey(const SettingsPage& page) { return "settings.pages." + std::string(page.id); }

  bool settingEntryOnPage(
      const SettingEntry& entry, std::string_view selectedPageId, std::string_view selectedBarName,
      std::string_view selectedMonitorOverride
  ) {
    const auto* page = settingsPageFromId(selectedPageId);
    if (page == nullptr || !settingsPageShowsSection(*page, entry.section)) {
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
