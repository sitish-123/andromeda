#include "core/files/file_watcher.h"
#include "shell/settings/settings_content_default_apps.h"
#include "shell/settings/settings_window.h"
#include "system/desktop_entry.h"

#include <utility>

void SettingsWindow::addDefaultAppsContent(float scale) {
  if (m_selectedSection != "default-apps") {
    if (m_mimeAppsWatchId != 0) {
      m_fileWatcher->unwatch(std::exchange(m_mimeAppsWatchId, 0));
    }
    return;
  }
  if (m_mimeAppsWatchId == 0 && m_fileWatcher != nullptr) {
    m_mimeAppsWatchId = m_fileWatcher->watch(
        default_apps::mimeAppsListPath(), [this]() { requestContentRebuild(); },
        FileWatcher::WatchTrigger::WriteCompleted
    );
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
