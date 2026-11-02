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
  // Cellular bars, 0 to 4; -1 without service.
  std::int32_t signal = -1;
};

// A paired device's last session and how it is reached.
struct LinkDeviceInfo {
  // Unix seconds of its last session; 0 when never seen.
  std::int64_t lastSeen = 0;
  // wifi or bluetooth while connected, else empty.
  std::string transport;
};

// A phone's own settings, from phones with system control: -1 or empty where the phone does not say.
struct LinkControls {
  std::int32_t mediaVolume = -1;
  // normal, vibrate, silent, or empty.
  std::string ringer;
  std::int32_t dnd = -1;
};

// A text message on a phone; `date` in Unix milliseconds, `sent` for one the phone sent.
struct LinkText {
  std::uint64_t id = 0;
  std::uint64_t thread = 0;
  std::string address;
  std::string body;
  std::uint64_t date = 0;
  bool sent = false;
};

// A recent photo on a phone; `path` feeds photoThumbnail and savePhoto.
struct LinkPhoto {
  std::string path;
  std::string name;
  std::uint64_t added = 0;
};

// What the last recent-photos request for a phone found.
enum class LinkPhotosState : std::uint8_t {
  Unknown, // not asked yet
  Loading,
  Ready,
  Denied, // Android withholds photo access on the phone
  Unavailable, // browse switch off, an old phone, or the phone is away
};

enum class LinkSaveResult : std::uint8_t { Saved, Denied, Failed };

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

// A command line phones may run on this desktop under `sh -c`.
struct LinkCommand {
  std::string name;
  std::string command;
};

struct LinkPairingOutcome {
  bool paired = false;
  // The new device's name when paired, else the daemon's reason.
  std::string detail;
};

// Client of andromeda-linkd (org.andromeda.Link1): paired devices, the pairing window, shares, and phone notifications.
// A received share becomes a notification whose action copies the text or opens the link. A phone notification is
// shown with its actions and inline reply, which run on the phone; dismissing it here dismisses it there. A phone
// looking for this desktop rings it until stopped. An incoming call offers mute and decline. Phones that share their
// texts list them here, and texts go out through them. The daemon may start, stop, or restart at any time; available()
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

  struct TransferStatus {
    std::string transferId;
    std::string title;
    std::string subtitle;
    float progress = 0.0F;
  };
  // The transfer that reported progress last; empty when none runs.
  [[nodiscard]] std::optional<TransferStatus> activeTransfer() const;
  void cancelTransfer(const std::string& transferId);

  struct CallStatus {
    std::string deviceId;
    std::string caller;
    std::string device;
    bool ringing = false;
  };
  // The phone call that is ringing or active; empty when none.
  [[nodiscard]] std::optional<CallStatus> activeCall() const;
  void declineCall(const std::string& deviceId);
  void muteCall(const std::string& deviceId);

  // Runs on each transfer progress or finish and on each call state change. While it is set, transfer progress
  // shows in the live activity island instead of a toast.
  void setActivityCallback(ChangeCallback callback);
  void startPairing();
  void cancelPairing();
  // Whether a phone with a Link session reported this code for the Bluetooth pairing under way; `done` runs once.
  void confirmBluetoothPairing(std::uint32_t passkey, std::function<void(bool)> done);
  void unpair(const std::string& deviceId);
  // kind is "text" or "link"; the daemon checks the rest and failures are logged.
  void share(const std::string& deviceId, const std::string& kind, const std::string& text);
  // The clipboard's text, as a link when it is one.
  void shareClipboard(const std::string& deviceId);
  /// Opens the phone's folder under ~/Phone, which andromeda-link-mount serves while the phone allows browsing.
  void browse(const std::string& deviceId);
  /// Opens andromeda-link-mirror for the phone, which asks it to show its screen.
  void mirror(const std::string& deviceId);
  /// Opens andromeda-link-apps, which lists the phone's apps and opens each in its own window.
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
  // Sends a text through the phone; `done` gets the daemon's error, or nothing once the phone reports it sent.
  void sendText(
      const std::string& deviceId, const std::string& to, const std::string& body,
      std::function<void(std::optional<std::string>)> done
  );
  void ping(const std::string& deviceId);
  // command is set-media-volume, set-ringer (0 silent, 1 vibrate, 2 normal), or set-dnd (1 or 0).
  void sendSystemCommand(const std::string& deviceId, const std::string& command, std::uint32_t value);
  /// Opens the Link app, andromeda-link.
  static void openApp();
  // A phone with the input grant moves this pointer and types through it.
  [[nodiscard]] const std::vector<LinkCommand>& commands() const noexcept { return m_commands; }
  void setCommands(const std::vector<LinkCommand>& commands);
  void registerIpc(IpcService& ipc);

  [[nodiscard]] bool available() const noexcept { return m_available; }
  [[nodiscard]] const std::vector<LinkDevice>& devices() const noexcept { return m_devices; }
  void setLocalSendVisible(bool visible);
  [[nodiscard]] bool localSendVisible() const noexcept { return m_localSendVisible; }
  [[nodiscard]] const std::vector<LinkNearby>& nearby() const noexcept { return m_nearby; }
  // The device's status while it is connected and has reported one.
  [[nodiscard]] const LinkStatus* status(const std::string& deviceId) const;
  [[nodiscard]] const LinkDeviceInfo* info(const std::string& deviceId) const;
  [[nodiscard]] const LinkControls* controls(const std::string& deviceId) const;
  // The phone notifications on show, those held while locked included.
  [[nodiscard]] std::uint32_t notificationCount(const std::string& deviceId) const;
  // Devices whose file offers are accepted without asking.
  [[nodiscard]] const std::vector<std::string>& autoAccept() const noexcept { return m_autoAccept; }
  // The window this shell opened, while the daemon keeps it open.
  [[nodiscard]] const std::optional<LinkPairing>& pairing() const noexcept { return m_pairing; }
  // How the last pairing attempt ended; cleared by the next startPairing().
  [[nodiscard]] const std::optional<LinkPairingOutcome>& outcome() const noexcept { return m_outcome; }
  // The user paused the phone here: it stays paired but is refused until connect().
  [[nodiscard]] bool paused(const std::string& deviceId) const { return m_paused.contains(deviceId); }
  // connect() ran and the phone has not dialed back yet; the phone retries within 30 s.
  [[nodiscard]] bool connecting(const std::string& deviceId) const;
  // `done` gets whether the daemon accepted the call.
  void connect(const std::string& deviceId, std::function<void(bool)> done = {});
  void disconnect(const std::string& deviceId, std::function<void(bool)> done = {});
  // Asks the phone for its newest photos; the answer arrives through the change callback.
  void requestPhotos(const std::string& deviceId, std::uint16_t count);
  [[nodiscard]] LinkPhotosState photosState(const std::string& deviceId) const;
  [[nodiscard]] const std::vector<LinkPhoto>* photos(const std::string& deviceId) const;
  // The decoded thumbnail of a listed photo, or null while it loads (asked on the first call).
  [[nodiscard]] std::shared_ptr<const NotificationImageData>
  photoThumbnail(const std::string& deviceId, const std::string& path, std::uint16_t size);
  // Copies a listed photo into the downloads folder; `done` runs once.
  void savePhoto(const std::string& deviceId, const std::string& path, std::function<void(LinkSaveResult)> done);
  [[nodiscard]] bool phoneRinging(const std::string& deviceId) const { return m_phonesRinging.contains(deviceId); }
  // The phone's newest texts, newest first, while its messages switch is on.
  [[nodiscard]] const std::vector<LinkText>* texts(const std::string& deviceId) const;
  // The phone's contact name for a number, else the number.
  [[nodiscard]] std::string contactName(const std::string& deviceId, const std::string& address) const;

private:
  void refresh();
  void apply(const std::map<std::string, sdbus::Variant>& properties);
  // DeviceInfo, DeviceControls and NotificationCounts.
  void applyDeviceState(const std::map<std::string, sdbus::Variant>& properties);
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
  void closeTransferNotification(std::uint32_t& id);
  void onNotificationClosed(std::uint32_t id, CloseReason reason);
  void onRingRequested(const std::string& deviceId, bool on);
  void onHotspot(const std::string& deviceId, const std::string& ssid);
  void onHotspotRequested(const std::string& deviceId, const std::string& ssid);
  void onPinged(const std::string& deviceId, const std::string& message);
  void
  onCall(const std::string& deviceId, const std::string& state, const std::string& number, const std::string& name);
  void stopRinging();
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
    std::uint64_t bytes = 0;
    std::uint64_t total = 0;
    // Order of the last progress report; 0 before the first.
    std::uint64_t progressSerial = 0;
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
  std::uint64_t m_progressSerial = 0;
  ChangeCallback m_activityCallback;
  std::optional<CallStatus> m_call;
  std::unordered_map<std::uint32_t, TransferAction> m_transferActions;
  std::vector<std::string> m_autoAccept;
  std::map<std::string, std::vector<std::string>> m_grants;
  std::map<std::string, LinkStatus> m_status;
  std::set<std::string> m_paused;
  std::map<std::string, std::chrono::steady_clock::time_point> m_connectDeadline;
  struct PhotoList {
    LinkPhotosState state = LinkPhotosState::Unknown;
    std::vector<LinkPhoto> photos;
  };
  std::map<std::string, PhotoList> m_photoLists;
  // Decoded thumbnails by device and path; null while the request runs.
  std::map<std::pair<std::string, std::string>, std::shared_ptr<const NotificationImageData>> m_thumbnails;
  // Counts requests per phone so that a reply of an older request is dropped.
  std::map<std::string, std::uint64_t> m_photoGeneration;
  Timer m_connectTimer;
  void forgetPhotos(const std::string& deviceId);
  std::map<std::string, LinkDeviceInfo> m_info;
  std::map<std::string, LinkControls> m_controls;
  std::map<std::string, std::uint32_t> m_notificationCounts;
  std::map<std::string, std::vector<LinkText>> m_texts;
  // Per phone, a number's last 9 digits to the contact's name.
  std::map<std::string, std::unordered_map<std::string, std::string>> m_contactNames;
  std::vector<LinkNearby> m_nearby;
  std::vector<LinkCommand> m_commands;
  bool m_localSendVisible = false;
  std::weak_ptr<SoundPlayer> m_sounds;
  std::set<std::string> m_phonesRinging;
  /// The notice shown while the laptop is on a phone's hotspot, replaced when it leaves.
  std::uint32_t m_hotspotNotice = 0;
  /// The question whether to join a phone's hotspot, and the phone it is for.
  std::uint32_t m_hotspotAsk = 0;
  std::string m_hotspotAskDevice;
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
