#include "shell/settings/settings_pages.h"
#include "shell/settings/settings_window.h"

#include <utility>

void SettingsWindow::applyNavEntry(const NavEntry& entry) {
  clearTransientSettingsState();
  m_searchQuery.clear();
  m_pluginSearchQuery.clear();
  m_pluginSearchDebounceTimer.stop();
  m_searchDebounceTimer.stop();
  m_selectedSection = entry.page;
  if (!entry.bar.empty()) {
    m_selectedBarName = entry.bar;
  }
  m_selectedMonitorOverride = entry.monitor;
  m_expertOpen = entry.expert;
  m_detail = entry.detail;
  m_wifiJoinSsid.clear();
  m_wifiPassword.clear();
  m_expertQuery.clear();
  m_contentScrollState.offset = 0.0F;
}

void SettingsWindow::navigateTo(
    std::string page, std::string bar, bool expert, std::string highlight, std::string monitor
) {
  NavEntry next{.page = std::move(page), .bar = std::move(bar), .monitor = std::move(monitor), .expert = expert};
  applyNavEntry(next);
  m_highlightKey = std::move(highlight);
  pushNavEntry(std::move(next));
}

void SettingsWindow::openDetail(std::string detail) {
  NavEntry next{
      .page = m_selectedSection, .bar = m_selectedBarName, .monitor = m_selectedMonitorOverride, .detail = std::move(detail)
  };
  applyNavEntry(next);
  m_highlightKey.clear();
  pushNavEntry(std::move(next));
}

void SettingsWindow::pushNavEntry(NavEntry entry) {
  if (m_navHistory.empty() || m_navHistory[m_navIndex] != entry) {
    m_navHistory.resize(m_navHistory.empty() ? 0 : m_navIndex + 1);
    m_navHistory.push_back(std::move(entry));
    m_navIndex = m_navHistory.size() - 1;
  }
  if (isOpen()) {
    requestSceneRebuild();
  }
}

void SettingsWindow::navigateHistory(int delta) {
  const auto target = static_cast<std::ptrdiff_t>(m_navIndex) + delta;
  if (target < 0 || target >= static_cast<std::ptrdiff_t>(m_navHistory.size())) {
    return;
  }
  m_navIndex = static_cast<std::size_t>(target);
  applyNavEntry(m_navHistory[m_navIndex]);
  m_highlightKey.clear();
  requestSceneRebuild();
}

void SettingsWindow::openSearchResult(const settings::SettingEntry& entry) {
  std::string bar;
  std::string monitor;
  if (entry.section == settings::SettingsSection::Bar && entry.path.size() >= 2) {
    bar = entry.path[1];
    if (settings::isBarMonitorOverrideSettingPath(entry.path)) {
      monitor = entry.path[3];
    }
  } else if (settings::isDockMonitorOverrideSettingPath(entry.path)) {
    monitor = entry.path[2];
  }
  navigateTo(
      std::string(settings::settingsPageForEntry(entry).id), std::move(bar), settings::settingEntryIsExpert(entry),
      settings::settingEntryKey(entry), std::move(monitor)
  );
}
