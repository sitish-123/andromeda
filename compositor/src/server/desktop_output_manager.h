#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

struct dsk_output_manager_v1_interface;
struct wl_client;
struct wl_global;
struct wl_resource;

namespace umbriel {

  class Output;
  class Server;

  // Serves dsk_output_manager_v1: mirror control for the desktop shell.
  class DesktopOutputManager {
  public:
    explicit DesktopOutputManager(Server& server);
    ~DesktopOutputManager();

    DesktopOutputManager(const DesktopOutputManager&) = delete;
    DesktopOutputManager& operator=(const DesktopOutputManager&) = delete;

    // Tell every bound client this output's current mirror state.
    void broadcast(const Output& output);
    // Resends every output's properties after a configuration change.
    void propertiesChanged();

  private:
    // The scanner also declares a wl_interface variable of this name, so the struct needs its elaborated form.
    static const struct dsk_output_manager_v1_interface kImplementation;

    static void bind(wl_client* client, void* data, uint32_t version, uint32_t id);
    static void handleSetMirror(wl_client* client, wl_resource* resource, const char* target, const char* source);
    static void handleClearMirror(wl_client* client, wl_resource* resource, const char* target);
    static void
    handleSetProperty(wl_client* client, wl_resource* resource, const char* target, const char* key, const char* value);
    static void handleDestroy(wl_client* client, wl_resource* resource);
    static void handleResourceDestroyed(wl_resource* resource);

    void apply(wl_resource* resource, std::string_view target, std::string_view source);
    static void sendState(wl_resource* resource, const Output& output);
    static void sendProperties(wl_resource* resource, const Output& output);
    void setProperty(wl_resource* resource, std::string_view target, std::string_view key, std::string_view value);

    Server& m_server;
    wl_global* m_global = nullptr;
    std::vector<wl_resource*> m_resources;
  };

} // namespace umbriel
