#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct dsk_input_manager_v1_interface;
struct wl_client;
struct wl_global;
struct wl_resource;

namespace umbriel {

  class Server;

  struct InputDeviceInfo {
    std::string name;
    uint32_t kind = 0; // dsk_input_manager_v1 kind
    bool operator==(const InputDeviceInfo&) const = default;
  };

  // Serves dsk_input_manager_v1: input settings for the desktop shell, stored in the generated input.toml.
  class DesktopInputManager {
  public:
    explicit DesktopInputManager(Server& server);
    ~DesktopInputManager();

    DesktopInputManager(const DesktopInputManager&) = delete;
    DesktopInputManager& operator=(const DesktopInputManager&) = delete;

    void devicesChanged();
    void settingsChanged();

  private:
    // The scanner also declares a wl_interface variable of this name, so the struct needs its elaborated form.
    static const struct dsk_input_manager_v1_interface kImplementation;

    static void bind(wl_client* client, void* data, uint32_t version, uint32_t id);
    static void handleSet(wl_client* client, wl_resource* resource, const char* key, const char* value);
    static void handleDestroy(wl_client* client, wl_resource* resource);
    static void handleResourceDestroyed(wl_resource* resource);

    void set(wl_resource* resource, std::string_view key, std::string_view value);
    void sendSettings(wl_resource* resource) const;

    Server& m_server;
    wl_global* m_global = nullptr;
    std::vector<wl_resource*> m_resources;
    std::vector<InputDeviceInfo> m_devices;
  };

} // namespace umbriel
