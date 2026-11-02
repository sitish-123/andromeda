#pragma once

#include "config/managed_settings.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct dsk_settings_manager_v1_interface;
struct wl_client;
struct wl_event_source;
struct wl_global;
struct wl_resource;

namespace andromeda {

  class Server;

  struct InputDeviceInfo {
    std::string name;
    uint32_t kind = 0; // dsk_settings_manager_v1 kind
    bool operator==(const InputDeviceInfo&) const = default;
  };

  // Serves dsk_settings_manager_v1: the settings a desktop shell may change, stored in the generated settings.toml,
  // and the connected input devices.
  class DesktopSettingsManager {
  public:
    explicit DesktopSettingsManager(Server& server);
    ~DesktopSettingsManager();

    DesktopSettingsManager(const DesktopSettingsManager&) = delete;
    DesktopSettingsManager& operator=(const DesktopSettingsManager&) = delete;

    void devicesChanged();
    void settingsChanged();

    // True while a client waits for capture_chord; the keyboard hands the next non-modifier press to captureChord
    // instead of keybinds and clients.
    [[nodiscard]] bool capturingChord() const noexcept { return m_captureResource != nullptr; }
    void captureChord(uint32_t keysym, uint32_t modifiers);

  private:
    // Values a client holds above the user's settings; dropped with the client. Kept in order of first use.
    struct Layer {
      wl_resource* owner = nullptr;
      std::string name;
      std::vector<std::pair<std::string, ManagedSettingValue>> values;
    };

    // The scanner also declares a wl_interface variable of this name, so the struct needs its elaborated form.
    static const struct dsk_settings_manager_v1_interface kImplementation;

    static void bind(wl_client* client, void* data, uint32_t version, uint32_t id);
    static void handleSet(wl_client* client, wl_resource* resource, const char* key, const char* value);
    static void handleBind(wl_client* client, wl_resource* resource, const char* chord, const char* action);
    static void handleCaptureChord(wl_client* client, wl_resource* resource);
    static void handleCancelCapture(wl_client* client, wl_resource* resource);
    static void handleSetInLayer(
        wl_client* client, wl_resource* resource, const char* layer, const char* key, const char* value
    );
    static void handleClearLayer(wl_client* client, wl_resource* resource, const char* layer);
    static void handleDestroy(wl_client* client, wl_resource* resource);
    static void handleResourceDestroyed(wl_resource* resource);

    void set(wl_resource* resource, std::string_view key, std::string_view value);
    void setInLayer(wl_resource* resource, std::string_view layer, std::string_view key, std::string_view value);
    void applyLayers();
    void dropLayers(wl_resource* owner, const std::string* name);
    void bindChord(wl_resource* resource, std::string_view chord, std::string_view action);
    void sendSettings(wl_resource* resource) const;
    void sendKeybinds(wl_resource* resource) const;
    void finishCapture(std::string_view chord);

    Server& m_server;
    wl_global* m_global = nullptr;
    std::vector<wl_resource*> m_resources;
    std::vector<InputDeviceInfo> m_devices;
    wl_resource* m_captureResource = nullptr;
    std::vector<Layer> m_layers;
    std::vector<std::pair<std::string, ManagedSettingValue>> m_appliedOverlay;
    wl_event_source* m_applyIdle = nullptr;
  };

} // namespace andromeda
