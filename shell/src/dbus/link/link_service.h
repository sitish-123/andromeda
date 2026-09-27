#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

// What a connected phone reports about itself.
struct LinkStatus {
  std::uint32_t battery = 0;
  bool charging = false;
  // wifi, cellular, ethernet, none, or other.
  std::string network;
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
  // Opens the files here and passes the descriptors, since the sandboxed daemon cannot read the user's files. Returns
  // the paths that could not be opened.
  std::vector<std::string> sendFiles(const std::string& deviceId, const std::vector<std::string>& paths);
  void setAutoAccept(const std::string& deviceId, bool enabled);
  // feature is clipboard, files, or notifications.
  void setGrant(const std::string& deviceId, const std::string& feature, bool granted);
  [[nodiscard]] bool granted(const std::string& deviceId, std::string_view feature) const;
  void registerIpc(IpcService& ipc, std::function<void()> showPairing);

  [[nodiscard]] bool available() const noexcept { return m_available; }
  [[nodiscard]] const std::vector<LinkDevice>& devices() const noexcept { return m_devices; }
  // The device's status while it is connected and has reported one.
  [[nodiscard]] const LinkStatus* status(const std::string& deviceId) const;
  // Devices whose file offers are accepted without asking.
  [[nodiscard]] const std::vector<std::string>& autoAccept() const noexcept { return m_autoAccept; }
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
  void onOffered(
      const std::string& transferId, const std::string& deviceId,
      const std::vector<std::pair<std::string, std::uint64_t>>& files
  );
  void onProgress(const std::string& transferId, std::uint64_t bytes, std::uint64_t total);
  void onFinished(const std::string& transferId, const std::string& status, const std::vector<std::string>& paths);
  void callTransfer(const char* method, const std::string& transferId);
  void onLocalClipboard(
      const std::vector<std::string>& mimeTypes, const std::string& dataMimeType, const std::vector<std::uint8_t>& data
  );
  void onClipboardOffered(
      const std::string& deviceId, std::uint64_t id, const std::vector<std::string>& mimeTypes, std::uint64_t size
  );
  void closeTransferNotification(std::uint32_t& id);
  [[nodiscard]] std::string deviceName(const std::string& deviceId) const;

  struct ReceivedShare {
    bool link = false;
    std::string text;
  };

  struct Transfer {
    std::string deviceId;
    bool incoming = false;
    std::uint32_t offerNotification = 0;
    std::uint32_t progressNotification = 0;
    // The user closed the progress toast; it is not shown again.
    bool progressDismissed = false;
  };

  // What a transfer notification's actions act on: the transfer, or the files it delivered.
  struct TransferAction {
    std::string transferId;
    std::vector<std::string> paths;
  };

  std::unique_ptr<sdbus::IProxy> m_daemon;
  std::unique_ptr<sdbus::IProxy> m_link;
  NotificationManager& m_notifications;
  ClipboardService& m_clipboard;
  // Received shares by the id of the notification that offers them, until it closes.
  std::unordered_map<std::uint32_t, ReceivedShare> m_received;
  std::unordered_map<std::string, Transfer> m_transfers;
  std::unordered_map<std::uint32_t, TransferAction> m_transferActions;
  std::vector<std::string> m_autoAccept;
  std::map<std::string, std::vector<std::string>> m_grants;
  std::map<std::string, LinkStatus> m_status;
  // Hash of the last clipboard offered, so a re-read of the same selection is not offered again.
  std::size_t m_lastClipboardHash = 0;
  ChangeCallback m_changeCallback;
  std::vector<LinkDevice> m_devices;
  std::optional<LinkPairing> m_pairing;
  std::optional<LinkPairingOutcome> m_outcome;
  bool m_available = false;
};
