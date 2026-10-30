#include "shell/settings/settings_content_devices.h"
#include "shell/settings/settings_window.h"

void SettingsWindow::addDevicesContent(float scale) {
  if (m_selectedSection != "devices") {
    return;
  }
  settings::addSettingsDevices(*m_contentContainer, scale);
}
