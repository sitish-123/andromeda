#include "core/log.h"
#include "core/process/process.h"
#include "util/file_utils.h"
#include "dbus/andromeda/software_update_service.h"
#include "shell/settings/settings_content_software_update.h"
#include "shell/settings/settings_window.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace {
  constexpr Logger kLog("software-update");
} // namespace
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
  // andromeda-update-check.service skips its run while this file exists.
  const std::filesystem::path checkOff = FileUtils::expandXdgBaseDir("$XDG_CONFIG_HOME/andromeda/update-check-off");
  std::error_code ec;
  const bool checkDaily = !std::filesystem::exists(checkOff, ec);
  settings::addSoftwareUpdate(
      *m_contentContainer, scale, service, group,
      m_hookFailuresProvider ? m_hookFailuresProvider() : std::vector<std::pair<std::string, std::string>>{},
      checkDaily,
      settings::SoftwareUpdateActions{
          .check = [service] { service->check(); },
          .markNewsRead = [service] { service->markNewsRead(); },
          .upgrade = [service] { service->upgrade(); },
          .upgradeAndromeda = [service] { service->upgradeAndromeda(); },
          .retry = [service] { service->retry(); },
          .scheduleTonight = [service](bool enable) { service->scheduleTonight(enable); },
          .restart = [this] {
            if (m_restartSession) {
              m_restartSession();
            }
          },
          .openLink = [](const std::string& url) { (void)process::runAsync(std::vector<std::string>{"xdg-open", url}); },
          .openGroup = [this](const std::string& g) { openDetail("update:" + g); },
          .setCheckDaily =
              [this, checkOff](bool on) {
                std::error_code error;
                if (on) {
                  std::filesystem::remove(checkOff, error);
                } else {
                  std::filesystem::create_directories(checkOff.parent_path(), error);
                  std::ofstream file(checkOff);
                  if (!file) {
                    kLog.warn("check daily off: cannot write {}", checkOff.string());
                  }
                }
                requestContentRebuild();
              },
      }
  );
}

void SettingsWindow::onSoftwareUpdateChanged() {
  if (isOpen() && m_selectedSection == "software-update" && m_searchQuery.empty()) {
    requestContentRebuild();
  }
}
