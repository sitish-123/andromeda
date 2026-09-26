#include "shell/settings/settings_content_default_apps.h"
#include "shell/settings/settings_window.h"
#include "system/desktop_entry.h"

#include <filesystem>

void SettingsWindow::addDefaultAppsContent(float scale) {
  if (m_selectedSection != "default-apps") {
    return;
  }
  const auto entries = desktopEntriesSnapshot();
  settings::addSettingsDefaultApps(
      *m_contentContainer,
      settings::SettingsDefaultAppsContext{
          .scale = scale,
          .entries = *entries,
          .currentDefault = [](
                                default_apps::Category category
                            ) { return default_apps::currentDefault(default_apps::mimeAppsListPath(), category); },
          .setDefault =
              [this](default_apps::Category category, std::string desktopId) {
                default_apps::setDefault(default_apps::mimeAppsListPath(), category, desktopId);
                requestContentRebuild();
              },
      }
  );
}

void SettingsWindow::refreshDefaultAppsIfChanged() {
  if (m_selectedSection != "default-apps") {
    return;
  }
  std::error_code error;
  const auto written = std::filesystem::last_write_time(default_apps::mimeAppsListPath(), error);
  if (error) {
    return;
  }
  if (!m_mimeAppsListWritten.has_value() || *m_mimeAppsListWritten != written) {
    m_mimeAppsListWritten = written;
    requestContentRebuild();
  }
}
