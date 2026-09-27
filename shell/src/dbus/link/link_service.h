#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class ClipboardService;
class IpcService;
class NotificationManager;
class SessionBus;

namespace sdbus {
  class IProxy;
  class Variant;
} // namespace sdbus

struct LinkDevice {
  std::string id;
  std::string name;
  bool connected = false;
};

struct LinkPairing {
  std::string code;
  std::string uri;
  std::chrono::steady_clock::time_point deadline;
};

struct LinkPairingOutcome {
  bool paired = false;
  // The new device's name when paired, else the daemon's reason.
  std::string detail;
};

// Client of umbriel-linkd (org.umbriel.Link1): paired devices, the pairing window, and shares. A received share
// becomes a notification whose action copies the text or opens the link. The daemon may start, stop, or restart at
// any time; available() follows its bus name.
class LinkService {
public:
  using ChangeCallback = std::function<void()>;

  LinkService(SessionBus& bus, NotificationManager& notifications, ClipboardService& clipboard);
  ~LinkService();

  LinkService(const LinkService&) = delete;
  LinkService& operator=(const LinkService&) = delete;

  void setChangeCallback(ChangeCallback callback);
  void startPairing();
  void cancelPairing();
  void unpair(const std::string& deviceId);
  // kind is "text" or "link"; the daemon checks the rest and failures are logged.
  void share(const std::string& deviceId, const std::string& kind, const std::string& text);
  // The clipboard's text, as a link when it is one.
  void shareClipboard(const std::string& deviceId);
  void registerIpc(IpcService& ipc, std::function<void()> showPairing);

  [[nodiscard]] bool available() const noexcept { return m_available; }
  [[nodiscard]] const std::vector<LinkDevice>& devices() const noexcept { return m_devices; }
  // The window this shell opened, while the daemon keeps it open.
  [[nodiscard]] const std::optional<LinkPairing>& pairing() const noexcept { return m_pairing; }
  // How the last pairing attempt ended; cleared by the next startPairing().
  [[nodiscard]] const std::optional<LinkPairingOutcome>& outcome() const noexcept { return m_outcome; }

private:
  void refresh();
  void apply(const std::map<std::string, sdbus::Variant>& properties);
  void detach();
  void notify();
  void onReceived(const std::string& deviceId, const std::string& kind, const std::string& text);
  void onAction(std::uint32_t id, const std::string& action, const std::string& activationToken);

  struct ReceivedShare {
    bool link = false;
    std::string text;
  };

  std::unique_ptr<sdbus::IProxy> m_daemon;
  std::unique_ptr<sdbus::IProxy> m_link;
  NotificationManager& m_notifications;
  ClipboardService& m_clipboard;
  // Received shares by the id of the notification that offers them, until it closes.
  std::unordered_map<std::uint32_t, ReceivedShare> m_received;
  ChangeCallback m_changeCallback;
  std::vector<LinkDevice> m_devices;
  std::optional<LinkPairing> m_pairing;
  std::optional<LinkPairingOutcome> m_outcome;
  bool m_available = false;
};
