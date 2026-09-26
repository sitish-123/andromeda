#include "server/desktop_shell.h"

#include "desktop-unstable-v1-protocol.h"
#include "server/server.h"

#include <algorithm>
#include <string>

namespace umbriel {

  namespace {

    constexpr uint32_t kVersion = 1;

    DesktopShell* shellFrom(wl_resource* resource) {
      return static_cast<DesktopShell*>(wl_resource_get_user_data(resource));
    }

  } // namespace

  DesktopShell::DesktopShell(Server& server) : m_server(server) {
    m_global = wl_global_create(server.display(), &dsk_shell_v1_interface, kVersion, this, bind);
  }

  DesktopShell::~DesktopShell() {
    for (wl_resource* resource : m_resources) {
      wl_resource_set_user_data(resource, nullptr);
    }
    if (m_global != nullptr) {
      wl_global_destroy(m_global);
    }
  }

  void DesktopShell::bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* self = static_cast<DesktopShell*>(data);
    wl_resource* resource = wl_resource_create(client, &dsk_shell_v1_interface, static_cast<int>(version), id);
    if (resource == nullptr) {
      wl_client_post_no_memory(client);
      return;
    }
    wl_resource_set_implementation(resource, &kImplementation, self, handleResourceDestroyed);
    self->m_resources.push_back(resource);
    self->sendLockKeys(resource);
  }

  void DesktopShell::handleDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

  void DesktopShell::handleResourceDestroyed(wl_resource* resource) {
    if (DesktopShell* self = shellFrom(resource)) {
      std::erase(self->m_resources, resource);
    }
  }

  bool DesktopShell::sendAction(std::string_view command) {
    const std::string line(command);
    for (wl_resource* resource : m_resources) {
      dsk_shell_v1_send_action(resource, line.c_str());
    }
    return !m_resources.empty();
  }

  void DesktopShell::setLockKeys(bool capsLock, bool numLock, bool scrollLock) {
    if (capsLock == m_capsLock && numLock == m_numLock && scrollLock == m_scrollLock) {
      return;
    }
    m_capsLock = capsLock;
    m_numLock = numLock;
    m_scrollLock = scrollLock;
    for (wl_resource* resource : m_resources) {
      sendLockKeys(resource);
    }
  }

  void DesktopShell::sendLockKeys(wl_resource* resource) const {
    dsk_shell_v1_send_lock_keys(resource, m_capsLock ? 1 : 0, m_numLock ? 1 : 0, m_scrollLock ? 1 : 0);
  }

  const struct dsk_shell_v1_interface DesktopShell::kImplementation = {
      .destroy = DesktopShell::handleDestroy,
  };

} // namespace umbriel
