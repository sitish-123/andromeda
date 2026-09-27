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
// looking for this desktop rings it until stopped. The daemon may start, stop, or restart at any time; available()
// follows its bus name.
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
  void unpair(const std::string& deviceId);
  // kind is "text" or "link"; the daemon checks the rest and failures are logged.
  void share(const std::string& deviceId, const std::string& kind, const std::string& text);
  // The clipboard's text, as a link when it is one.
  void shareClipboard(const std::string& deviceId);
  // Starts or stops ringing a phone.
  void ring(const std::string& deviceId, bool on);
  void registerIpc(IpcService& ipc, std::function<void()> showPairing);

  [[nodiscard]] bool available() const noexcept { return m_available; }
  [[nodiscard]] const std::vector<LinkDevice>& devices() const noexcept { return m_devices; }
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
  void onNotificationPosted(
      const std::string& deviceId, const std::string& id, const std::string& app, const std::string& title,
      const std::string& text, const std::vector<std::uint8_t>& icon,
      const std::vector<sdbus::Struct<std::string, std::string, bool>>& actions
  );
  void onNotificationRemoved(const std::string& deviceId, const std::string& id);
  void onNotificationClosed(std::uint32_t id, CloseReason reason);
  void onRingRequested(const std::string& deviceId, bool on);
  void stopRinging();
  [[nodiscard]] std::uint32_t mirroredId(const std::string& deviceId, const std::string& id) const;
  [[nodiscard]] std::string deviceName(const std::string& deviceId) const;

  struct ReceivedShare {
    bool link = false;
    std::string text;
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
  // Phone notifications by the id of the desktop notification that shows them.
  std::unordered_map<std::uint32_t, MirroredNotification> m_mirrored;
  std::weak_ptr<SoundPlayer> m_sounds;
  std::set<std::string> m_phonesRinging;
  // The phone this desktop rings for, the notification that offers Stop, and the ring's timers.
  std::optional<std::string> m_ringingFor;
  std::uint32_t m_ringNotification = 0;
  Timer m_ringRepeat;
  Timer m_ringLimit;
  ChangeCallback m_changeCallback;
  std::vector<LinkDevice> m_devices;
  std::optional<LinkPairing> m_pairing;
  std::optional<LinkPairingOutcome> m_outcome;
  bool m_available = false;
};
