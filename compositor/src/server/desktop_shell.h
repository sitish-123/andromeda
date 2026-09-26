#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

struct dsk_shell_v1_interface;
struct wl_client;
struct wl_global;
struct wl_resource;

namespace umbriel {

  class Server;

  // Serves dsk_shell_v1: keybind actions and lock-key state for the desktop shell.
  class DesktopShell {
  public:
    explicit DesktopShell(Server& server);
    ~DesktopShell();

    DesktopShell(const DesktopShell&) = delete;
    DesktopShell& operator=(const DesktopShell&) = delete;

    // False when no shell is bound.
    bool sendAction(std::string_view command);
    void setLockKeys(bool capsLock, bool numLock, bool scrollLock);

  private:
    // The scanner also declares a wl_interface variable of this name, so the struct needs its elaborated form.
    static const struct dsk_shell_v1_interface kImplementation;

    static void bind(wl_client* client, void* data, uint32_t version, uint32_t id);
    static void handleDestroy(wl_client* client, wl_resource* resource);
    static void handleResourceDestroyed(wl_resource* resource);

    void sendLockKeys(wl_resource* resource) const;

    Server& m_server;
    wl_global* m_global = nullptr;
    std::vector<wl_resource*> m_resources;
    bool m_capsLock = false;
    bool m_numLock = false;
    bool m_scrollLock = false;
  };

} // namespace umbriel
