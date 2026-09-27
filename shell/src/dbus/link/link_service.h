#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class IpcService;
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

// Client of umbriel-linkd (org.umbriel.Link1): paired devices and the pairing window. The daemon may start, stop, or
// restart at any time; available() follows its bus name.
class LinkService {
public:
  using ChangeCallback = std::function<void()>;

  explicit LinkService(SessionBus& bus);
  ~LinkService();

  LinkService(const LinkService&) = delete;
  LinkService& operator=(const LinkService&) = delete;

  void setChangeCallback(ChangeCallback callback);
  void startPairing();
  void cancelPairing();
  void unpair(const std::string& deviceId);
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

  std::unique_ptr<sdbus::IProxy> m_daemon;
  std::unique_ptr<sdbus::IProxy> m_link;
  ChangeCallback m_changeCallback;
  std::vector<LinkDevice> m_devices;
  std::optional<LinkPairing> m_pairing;
  std::optional<LinkPairingOutcome> m_outcome;
  bool m_available = false;
};
