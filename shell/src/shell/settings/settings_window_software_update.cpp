#include "core/process/process.h"
#include "dbus/andromeda/software_update_service.h"
#include "shell/settings/settings_content_software_update.h"
#include "shell/settings/settings_window.h"

#include <string>
#include <vector>

void SettingsWindow::addSoftwareUpdateContent(float scale) {
  if (m_selectedSection != "software-update") {
    m_softwareUpdateRead = false;
    return;
  }
  SoftwareUpdateService* service = m_softwareUpdateProvider ? m_softwareUpdateProvider() : nullptr;
  // Reading Update1 D-Bus activates it: only while the page is open, once per visit.
  if (service != nullptr && !m_softwareUpdateRead) {
    m_softwareUpdateRead = true;
    service->refresh();
  }
  const std::string group = m_detail.starts_with("update:") ? m_detail.substr(7) : std::string();
  settings::addSoftwareUpdate(
      *m_contentContainer, scale, service, group,
      m_hookFailuresProvider ? m_hookFailuresProvider() : std::vector<std::pair<std::string, std::string>>{},
      settings::SoftwareUpdateActions{
          .check = [service] { service->check(); },
          .markNewsRead = [service] { service->markNewsRead(); },
          .upgrade = [service] { service->upgrade(); },
          .scheduleTonight = [service](bool enable) { service->scheduleTonight(enable); },
          .restart = [this] {
            if (m_restartSession) {
              m_restartSession();
            }
          },
          .openLink = [](const std::string& url) { (void)process::runAsync(std::vector<std::string>{"xdg-open", url}); },
          .openGroup = [this](const std::string& g) { openDetail("update:" + g); },
      }
  );
}

void SettingsWindow::onSoftwareUpdateChanged() {
  if (isOpen() && m_selectedSection == "software-update" && m_searchQuery.empty()) {
    requestContentRebuild();
  }
}
