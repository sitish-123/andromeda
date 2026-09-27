#include "dbus/link/link_service.h"
#include "dbus/link/quickshare_service.h"
#include "shell/settings/settings_content_devices.h"
#include "shell/settings/settings_window.h"

#include <glib.h>

void SettingsWindow::addDevicesContent(float scale) {
  if (m_selectedSection != "devices") {
    return;
  }
  settings::SettingsDevicesContext ctx{.scale = scale};
  if (m_linkService != nullptr) {
    ctx.linkAvailable = m_linkService->available();
    for (const auto& device : m_linkService->devices()) {
      ctx.phones.push_back({.id = device.id, .name = device.name, .connected = device.connected});
    }
    ctx.unpair = [link = m_linkService](std::string id) { link->unpair(id); };
  }
  if (m_quickShareService != nullptr) {
    ctx.quickShareAvailable = m_quickShareService->available();
    ctx.quickShareVisible = m_quickShareService->visible();
    ctx.quickShareName = m_quickShareService->name();
    ctx.setQuickShareVisible = [qs = m_quickShareService](bool visible) { qs->setVisible(visible); };
  }
  const char* downloads = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
  ctx.downloads = downloads != nullptr ? downloads : "~/Downloads";
  ctx.pair = m_openPairing;
  settings::addSettingsDevices(*m_contentContainer, ctx);
}

void SettingsWindow::onDevicesChanged() {
  if (isOpen() && m_selectedSection == "devices") {
    requestContentRebuild();
  }
}
