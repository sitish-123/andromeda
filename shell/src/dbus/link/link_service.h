#pragma once

#include "core/timer_manager.h"
#include "notification/notification.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <sdbus-c++/Types.h>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class ClipboardService;
class IpcService;
class NotificationManager;
class SessionBus;
class SoundPlayer;

namespace sdbus {
  class IProxy;
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

// A LocalSend device on the LAN, which takes files like a Link device does.
struct LinkNearby {
  std::string id;
  std::string alias;
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

// Client of umbriel-linkd (org.umbriel.Link1): paired devices, the pairing window, shares, and phone notifications.
// A received share becomes a notification whose action copies the text or opens the link. A phone notification is
// shown with its actions and inline reply, which run on the phone; dismissing it here dismisses it there. A phone
// looking for this desktop rings it until stopped. An incoming call offers mute and decline. The daemon may start,
// stop, or restart at any time; available() follows its bus name.
class LinkService {
public:
  using ChangeCallback = std::function<void()>;

  LinkService(
      SessionBus& bus, NotificationManager& notifications, ClipboardService& clipboard,
      std::weak_ptr<SoundPlayer> sounds
  );
  ~LinkService();

  LinkService(const LinkService&) = delete;
  LinkService& operator=(const LinkService&) = delete;

  void setChangeCallback(ChangeCallback callback);
  void startPairing();
  void cancelPairing();
  // Whether a phone with a Link session reported this code for the Bluetooth pairing under way; `done` runs once.
  void confirmBluetoothPairing(std::uint32_t passkey, std::function<void(bool)> done);
  void unpair(const std::string& deviceId);
  // kind is "text" or "link"; the daemon checks the rest and failures are logged.
  void share(const std::string& deviceId, const std::string& kind, const std::string& text);
  // The clipboard's text, as a link when it is one.
  void shareClipboard(const std::string& deviceId);
  /// Opens the phone's folder under ~/Phone, which umbriel-link-mount serves while the phone allows browsing.
  void browse(const std::string& deviceId);
  /// Opens umbriel-link-mirror for the phone, which asks it to show its screen.
  void mirror(const std::string& deviceId);
  /// Opens umbriel-link-apps, which lists the phone's apps and opens each in its own window.
  void apps(const std::string& deviceId);
  // Opens the files here and passes the descriptors, since the sandboxed daemon cannot read the user's files. Returns
  // the paths that could not be opened.
  std::vector<std::string> sendFiles(const std::string& deviceId, const std::vector<std::string>& paths);
  void setAutoAccept(const std::string& deviceId, bool enabled);
  // feature is clipboard, files, or notifications.
  void setGrant(const std::string& deviceId, const std::string& feature, bool granted);
  [[nodiscard]] bool granted(const std::string& deviceId, std::string_view feature) const;
  // Starts or stops ringing a phone.
  void ring(const std::string& deviceId, bool on);
  void registerIpc(IpcService& ipc, std::function<void()> showPairing);

  [[nodiscard]] bool available() const noexcept { return m_available; }
  [[nodiscard]] const std::vector<LinkDevice>& devices() const noexcept { return m_devices; }
  void setLocalSendVisible(bool visible);
  [[nodiscard]] bool localSendVisible() const noexcept { return m_localSendVisible; }
  [[nodiscard]] const std::vector<LinkNearby>& nearby() const noexcept { return m_nearby; }
  // The device's status while it is connected and has reported one.
  [[nodiscard]] const LinkStatus* status(const std::string& deviceId) const;
  // Devices whose file offers are accepted without asking.
  [[nodiscard]] const std::vector<std::string>& autoAccept() const noexcept { return m_autoAccept; }
  // The window this shell opened, while the daemon keeps it open.
  [[nodiscard]] const std::optional<LinkPairing>& pairing() const noexcept { return m_pairing; }
  // How the last pairing attempt ended; cleared by the next startPairing().
  [[nodiscard]] const std::optional<LinkPairingOutcome>& outcome() const noexcept { return m_outcome; }
  [[nodiscard]] bool phoneRinging(const std::string& deviceId) const { return m_phonesRinging.contains(deviceId); }

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
  void onNotificationPosted(
      const std::string& deviceId, const std::string& id, const std::string& app, const std::string& title,
      const std::string& text, const std::vector<std::uint8_t>& icon,
      const std::vector<sdbus::Struct<std::string, std::string, bool>>& actions
  );
  void onNotificationRemoved(const std::string& deviceId, const std::string& id);
  void onNotificationClosed(std::uint32_t id, CloseReason reason);
  void onRingRequested(const std::string& deviceId, bool on);
  void onHotspot(const std::string& deviceId, const std::string& ssid);
  void
  onCall(const std::string& deviceId, const std::string& state, const std::string& number, const std::string& name);
  void stopRinging();
  [[nodiscard]] std::uint32_t mirroredId(const std::string& deviceId, const std::string& id) const;
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

  struct MirroredNotification {
    std::string deviceId;
    std::string id;
    // The phone action behind the inline reply field, if the notification takes a reply.
    std::string replyAction;
    // An action ran on the phone, so closing the notification here is not a dismissal.
    bool acted = false;
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
  std::vector<LinkNearby> m_nearby;
  bool m_localSendVisible = false;
  // Hash of the last clipboard offered, so a re-read of the same selection is not offered again.
  std::size_t m_lastClipboardHash = 0;
  // Phone notifications by the id of the desktop notification that shows them.
  std::unordered_map<std::uint32_t, MirroredNotification> m_mirrored;
  std::weak_ptr<SoundPlayer> m_sounds;
  std::set<std::string> m_phonesRinging;
  /// The notice shown while the laptop is on a phone's hotspot, replaced when it leaves.
  std::uint32_t m_hotspotNotice = 0;
  // The phone this desktop rings for, the notification that offers Stop, and the ring's timers.
  std::optional<std::string> m_ringingFor;
  std::uint32_t m_ringNotification = 0;
  Timer m_ringRepeat;
  Timer m_ringLimit;
  // Incoming-call notifications by phone, and their phone by notification id.
  std::unordered_map<std::string, std::uint32_t> m_callNotifications;
  std::unordered_map<std::uint32_t, std::string> m_callDevices;
  ChangeCallback m_changeCallback;
  std::vector<LinkDevice> m_devices;
  std::optional<LinkPairing> m_pairing;
  std::optional<LinkPairingOutcome> m_outcome;
  bool m_available = false;
};
