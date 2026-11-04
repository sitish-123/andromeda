#pragma once

#include "shell/settings/settings_registry.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace settings {

  enum class PageGroup : std::uint8_t { Connectivity, LookAndFeel, Desktop, System };

  // One sidebar page of the Settings window. A page shows the registry entries of its sections; a page with no
  // section has no content yet.
  struct SettingsPage {
    std::string_view id;
    PageGroup group;
    std::string_view glyph;
    std::span<const SettingsSection> sections;
    // False for pages reached from elsewhere, such as the account card.
    bool inSidebar = true;
  };

  [[nodiscard]] std::span<const SettingsPage> settingsPages();
  // Pages built by their own content code (no registry rows), such as Wi-Fi.
  [[nodiscard]] bool settingsPageHasOwnContent(std::string_view pageId);
  // Resolves a page id, a legacy section id or a merged-section alias to its page; null when unknown.
  [[nodiscard]] const SettingsPage* settingsPageFromId(std::string_view id);
  // The page that shows `section`; every section belongs to exactly one page.
  [[nodiscard]] const SettingsPage& settingsPageForSection(SettingsSection section);
  [[nodiscard]] bool settingsPageShowsSection(const SettingsPage& page, SettingsSection section);
  // True when `selectedPageId` names a page that shows the section with id `sectionId`.
  [[nodiscard]] bool selectedPageShows(std::string_view selectedPageId, std::string_view sectionId);
  [[nodiscard]] std::string settingsPageLabelKey(const SettingsPage& page);
  [[nodiscard]] std::string settingsPageGroupLabelKey(PageGroup group);

  // True when `entry` belongs on the page with id `selectedPageId` for the current bar and monitor selection.
  [[nodiscard]] bool settingEntryOnPage(
      const SettingEntry& entry, std::string_view selectedPageId, std::string_view selectedBarName,
      std::string_view selectedMonitorOverride
  );
  // Stable identity of a row: its config path, or section and title for a row without one.
  [[nodiscard]] std::string settingEntryKey(const SettingEntry& entry);
  // The page that shows `entry`: its section's page, unless a path rule moves it (night light to Displays).
  [[nodiscard]] const SettingsPage& settingsPageForEntry(const SettingEntry& entry);
  // Expert entries live behind the page's last row. An entry is main only when it is not `advanced` and a typical
  // user changes it (the curated list in settings_pages.cpp); every other entry is expert and stays in search.
  [[nodiscard]] bool settingEntryIsExpert(const SettingEntry& entry);
  // The card group a main row moves to, or empty to keep its own.
  [[nodiscard]] std::string_view settingGroupOverride(const SettingEntry& entry);
  // Position of a grouped main row in its page's order, or -1.
  [[nodiscard]] int settingGroupOrder(const SettingEntry& entry);
  // Main rows drop their caption unless the label alone is ambiguous.
  [[nodiscard]] bool settingEntryShowsCaption(const SettingEntry& entry);

} // namespace settings
