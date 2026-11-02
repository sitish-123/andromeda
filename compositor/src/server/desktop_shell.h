#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

struct dsk_shell_v1_interface;
struct wl_client;
struct wl_global;
struct wl_resource;

namespace andromeda {

  class Server;

  // Serves dsk_shell_v1: keybind actions, lock-key state, and system bells for the desktop shell.
  class DesktopShell {
  public:
    explicit DesktopShell(Server& server);
    ~DesktopShell();

    DesktopShell(const DesktopShell&) = delete;
    DesktopShell& operator=(const DesktopShell&) = delete;

    // False when no shell is bound.
    bool sendAction(std::string_view command);
    // False when no bound shell speaks version 2.
    bool sendBell(std::string_view appId);
    void setLockKeys(bool capsLock, bool numLock, bool scrollLock);
    // Rect is in output coordinates. Shells that bound version 3 get the state on bind and on every change.
    void setTextInputState(bool enabled, uint32_t purpose, uint32_t hint, int x, int y, int width, int height);
    // Both reach only shells that bound version 3. sendRecovered does nothing unless sendRelock came before it.
    void sendRelock();
    void sendRecovered();
    // Releases the keys the shell pressed with dsk_shell_v1 key and has not released.
    void releaseKeys();
    // The flags the shell last reported for the app process, from dsk_shell_v1 app_activity.
    [[nodiscard]] std::optional<uint32_t> appActivity(uint32_t pid) const;

  private:
    // The scanner also declares a wl_interface variable of this name, so the struct needs its elaborated form.
    static const struct dsk_shell_v1_interface kImplementation;

    static void bind(wl_client* client, void* data, uint32_t version, uint32_t id);
    static void handleDestroy(wl_client* client, wl_resource* resource);
    static void handleAppActivity(wl_client* client, wl_resource* resource, uint32_t pid, uint32_t flags);
    static void handleInsertText(wl_client* client, wl_resource* resource, const char* text);
    static void handleKey(wl_client* client, wl_resource* resource, uint32_t keysym, uint32_t state);
    static void handleResourceDestroyed(wl_resource* resource);

    void sendLockKeys(wl_resource* resource) const;
    void sendTextInputState(wl_resource* resource) const;

    Server& m_server;
    wl_global* m_global = nullptr;
    std::vector<wl_resource*> m_resources;
    bool m_capsLock = false;
    bool m_numLock = false;
    bool m_scrollLock = false;
    bool m_relocked = false;
    struct TextInputState {
      bool enabled = false;
      uint32_t purpose = 0;
      uint32_t hint = 0;
      int x = 0;
      int y = 0;
      int width = 0;
      int height = 0;
      bool operator==(const TextInputState&) const = default;
    } m_textInput;
    struct AppActivity {
      uint32_t flags;
      wl_resource* owner;
    };
    std::unordered_map<uint32_t, AppActivity> m_appActivity;
    // keysym -> evdev keycode of each key pressed and not yet released.
    std::unordered_map<uint32_t, uint32_t> m_pressedKeys;
  };

} // namespace andromeda
