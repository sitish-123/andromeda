#include "dbus/link/link_service.h"

#include "core/log.h"
#include "dbus/session_bus.h"
#include "i18n/i18n.h"
#include "ipc/ipc_service.h"
#include "net/url_open.h"
#include "notification/notification_manager.h"
#include "pipewire/sound_player.h"
#include "render/core/image_decoder.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <ranges>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <string_view>
#include <utility>

namespace {

  constexpr Logger kLog("link");

  const sdbus::ServiceName kLinkBusName{"org.umbriel.Link1"};
  const sdbus::ObjectPath kLinkPath{"/org/umbriel/Link1"};
  constexpr auto kLinkInterface = "org.umbriel.Link1";
  const sdbus::ServiceName kDaemonBusName{"org.freedesktop.DBus"};
  const sdbus::ObjectPath kDaemonPath{"/org/freedesktop/DBus"};
  constexpr auto kDaemonInterface = "org.freedesktop.DBus";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";
  // The daemon's window length; it reports the close itself through Pairing.
  constexpr auto kPairingWindow = std::chrono::seconds(120);

  // A mirrored notification's buttons are "phone:<the phone's action id>"; its reply field is the shell's inline reply.
  constexpr std::string_view kPhoneActionPrefix = "phone:";
  constexpr std::string_view kInlineReplyPrefix = "inline-reply::";
  constexpr auto kRingSound = "alarm-clock-elapsed";
  // The sound is shorter than this, so the gap between repeats stays short.
  constexpr auto kRingRepeat = std::chrono::milliseconds(2500);
  // A ring stops by itself after this, as link/ARCHITECTURE.md says.
  constexpr auto kRingLimit = std::chrono::minutes(2);
  // Phone icons are drawn at 64 px; the bound keeps a small hostile PNG from inflating into a huge bitmap.
  constexpr std::uint32_t kMaxIconSide = 256;
  constexpr std::array<std::uint8_t, 16> kPngHeader{
      0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R'
  };

  using VariantMap = std::map<std::string, sdbus::Variant>;

  void logFailure(std::string_view method, const std::optional<sdbus::Error>& error) {
    if (error.has_value()) {
      kLog.warn("{} failed: {}", method, error->what());
    }
  }

  std::uint32_t readBigEndian(const std::uint8_t* bytes) {
    return (std::uint32_t{bytes[0]} << 24) | (std::uint32_t{bytes[1]} << 16) | (std::uint32_t{bytes[2]} << 8)
        | std::uint32_t{bytes[3]};
  }

  // The PNG's size is read from its header before anything is decoded.
  std::optional<NotificationImageData> decodeIcon(const std::vector<std::uint8_t>& png) {
    if (png.size() < kPngHeader.size() + 8 || !std::ranges::equal(kPngHeader, png | std::views::take(kPngHeader.size()))) {
      return std::nullopt;
    }
    const std::uint32_t width = readBigEndian(&png[16]);
    const std::uint32_t height = readBigEndian(&png[20]);
    if (width == 0 || height == 0 || width > kMaxIconSide || height > kMaxIconSide) {
      kLog.info("phone notification icon of {}x{} ignored", width, height);
      return std::nullopt;
    }
    auto decoded = decodeRasterImage(png.data(), png.size());
    if (!decoded) {
      kLog.info("phone notification icon: {}", decoded.error());
      return std::nullopt;
    }
    return NotificationImageData{
        .width = decoded->width,
        .height = decoded->height,
        .rowStride = decoded->width * 4,
        .hasAlpha = true,
        .bitsPerSample = 8,
        .channels = 4,
        .data = std::move(decoded->pixels),
    };
  }

} // namespace

LinkService::LinkService(
    SessionBus& bus, NotificationManager& notifications, ClipboardService& clipboard, std::weak_ptr<SoundPlayer> sounds
)
    : m_notifications(notifications), m_clipboard(clipboard), m_sounds(std::move(sounds)) {
  m_daemon = sdbus::createProxy(bus.connection(), kDaemonBusName, kDaemonPath);
  m_daemon->uponSignal("NameOwnerChanged")
      .onInterface(kDaemonInterface)
      .call([this](const std::string& name, const std::string& /*oldOwner*/, const std::string& newOwner) {
        if (name != kLinkBusName) {
          return;
        }
        // A restart hands the name straight to the new owner, so any owner means reload.
        detach();
        if (!newOwner.empty()) {
          refresh();
        }
      });

  m_link = sdbus::createProxy(bus.connection(), kLinkBusName, kLinkPath);
  m_link->uponSignal("PropertiesChanged")
      .onInterface(kPropertiesInterface)
      .call([this](
                const std::string& interfaceName, const VariantMap& changed, const std::vector<std::string>& invalidated
            ) {
        if (interfaceName != kLinkInterface) {
          return;
        }
        if (!invalidated.empty()) {
          refresh();
          return;
        }
        apply(changed);
      });
  m_link->uponSignal("PairingFinished")
      .onInterface(kLinkInterface)
      .call([this](const std::string& /*deviceId*/, const std::string& name) {
        m_pairing.reset();
        m_outcome = LinkPairingOutcome{.paired = true, .detail = name};
        notify();
      });
  m_link->uponSignal("PairingFailed").onInterface(kLinkInterface).call([this](const std::string& reason) {
    m_pairing.reset();
    m_outcome = LinkPairingOutcome{.paired = false, .detail = reason};
    notify();
  });
  m_link->uponSignal("Received")
      .onInterface(kLinkInterface)
      .call([this](const std::string& deviceId, const std::string& kind, const std::string& text) {
        onReceived(deviceId, kind, text);
      });
  m_notifications.addInternalActionCallback(
      [this](std::uint32_t id, const std::string& action, const std::string& activationToken) {
        onAction(id, action, activationToken);
      }
  );
  m_link->uponSignal("NotificationPosted")
      .onInterface(kLinkInterface)
      .call([this](
                const std::string& deviceId, const std::string& id, const std::string& app, const std::string& title,
                const std::string& text, const std::vector<std::uint8_t>& icon,
                const std::vector<sdbus::Struct<std::string, std::string, bool>>& actions
            ) { onNotificationPosted(deviceId, id, app, title, text, icon, actions); });
  m_link->uponSignal("RingRequested").onInterface(kLinkInterface).call([this](const std::string& deviceId, bool on) {
    onRingRequested(deviceId, on);
  });
  m_link->uponSignal("PhoneRinging").onInterface(kLinkInterface).call([this](const std::string& deviceId, bool on) {
    if (on) {
      m_phonesRinging.insert(deviceId);
    } else {
      m_phonesRinging.erase(deviceId);
    }
    notify();
  });
  m_link->uponSignal("NotificationRemoved")
      .onInterface(kLinkInterface)
      .call([this](const std::string& deviceId, const std::string& id) { onNotificationRemoved(deviceId, id); });
  m_notifications.addEventCallback([this](const Notification& notification, NotificationEvent event) {
    if (event == NotificationEvent::Closed) {
      m_received.erase(notification.id);
    }
  });
  m_notifications.addCloseObserver([this](std::uint32_t id, CloseReason reason) { onNotificationClosed(id, reason); });

  m_daemon->callMethodAsync("NameHasOwner")
      .onInterface(kDaemonInterface)
      .withArguments(std::string{kLinkBusName})
      .uponReplyInvoke([this](std::optional<sdbus::Error> error, bool owned) {
        if (!error.has_value() && owned) {
          refresh();
        }
      });
}

LinkService::~LinkService() = default;

void LinkService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void LinkService::refresh() {
  m_link->callMethodAsync("GetAll")
      .onInterface(kPropertiesInterface)
      .withArguments(std::string{kLinkInterface})
      .uponReplyInvoke([this](std::optional<sdbus::Error> error, VariantMap properties) {
        if (error.has_value()) {
          kLog.debug("link properties unavailable: {}", error->what());
          return;
        }
        m_available = true;
        apply(properties);
      });
}

void LinkService::apply(const std::map<std::string, sdbus::Variant>& properties) {
  if (const auto it = properties.find("Devices"); it != properties.end()) {
    try {
      m_devices.clear();
      for (const auto& device : it->second.get<std::vector<sdbus::Struct<std::string, std::string, bool>>>()) {
        m_devices.push_back({.id = device.get<0>(), .name = device.get<1>(), .connected = device.get<2>()});
      }
    } catch (const sdbus::Error& e) {
      kLog.warn("malformed Devices: {}", e.what());
    }
  }
  if (const auto it = properties.find("Pairing"); it != properties.end()) {
    try {
      if (!it->second.get<bool>()) {
        m_pairing.reset();
      }
    } catch (const sdbus::Error& e) {
      kLog.warn("malformed Pairing: {}", e.what());
    }
  }
  notify();
}

void LinkService::detach() {
  if (!m_available) {
    return;
  }
  m_available = false;
  m_devices.clear();
  m_pairing.reset();
  notify();
}

void LinkService::notify() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void LinkService::startPairing() {
  m_link->callMethodAsync("StartPairing")
      .onInterface(kLinkInterface)
      .uponReplyInvoke([this](std::optional<sdbus::Error> error, std::string code, std::string uri) {
        if (error.has_value()) {
          logFailure("StartPairing", error);
          m_outcome = LinkPairingOutcome{.paired = false, .detail = error->getMessage()};
        } else if (code.size() != 6 || !std::ranges::all_of(code, [](char c) { return c >= '0' && c <= '9'; })) {
          kLog.warn("StartPairing returned a malformed code");
        } else {
          m_pairing = LinkPairing{
              .code = std::move(code),
              .uri = std::move(uri),
              .deadline = std::chrono::steady_clock::now() + kPairingWindow,
          };
          m_outcome.reset();
        }
        notify();
      });
}

void LinkService::cancelPairing() {
  m_link->callMethodAsync("CancelPairing")
      .onInterface(kLinkInterface)
      .uponReplyInvoke([](std::optional<sdbus::Error> error) { logFailure("CancelPairing", error); });
}

void LinkService::unpair(const std::string& deviceId) {
  m_link->callMethodAsync("Unpair")
      .onInterface(kLinkInterface)
      .withArguments(deviceId)
      .uponReplyInvoke([](std::optional<sdbus::Error> error) { logFailure("Unpair", error); });
}

void LinkService::share(const std::string& deviceId, const std::string& kind, const std::string& text) {
  m_link->callMethodAsync("Share")
      .onInterface(kLinkInterface)
      .withArguments(deviceId, kind, text)
      .uponReplyInvoke([](std::optional<sdbus::Error> error) { logFailure("Share", error); });
}

void LinkService::shareClipboard(const std::string& deviceId) {
  const std::optional<std::string> text = m_clipboard.clipboardText();
  if (!text.has_value() || text->empty()) {
    kLog.info("no clipboard text to send");
    return;
  }
  const std::string lower = StringUtils::toLower(text->substr(0, 8));
  const bool link = (lower.starts_with("http://") || lower.starts_with("https://"))
      && std::ranges::none_of(*text, [](unsigned char c) { return std::isspace(c) != 0; });
  share(deviceId, link ? "link" : "text", *text);
}

void LinkService::onReceived(const std::string& deviceId, const std::string& kind, const std::string& text) {
  const bool link = kind == "link";
  if (!link && kind != "text") {
    kLog.warn("Received with unknown kind {}", kind);
    return;
  }
  const auto device = std::ranges::find(m_devices, deviceId, &LinkDevice::id);
  const std::string name = device != m_devices.end() ? device->name : deviceId;
  const std::string action = i18n::tr(link ? "notifications.internal.link-open" : "notifications.internal.link-copy");
  NotificationRequest request;
  request.appName = i18n::tr("notifications.internal.link");
  request.summary = i18n::tr(
      link ? "notifications.internal.link-received-link" : "notifications.internal.link-received-text", "device", name
  );
  request.body = text;
  request.origin = NotificationOrigin::Internal;
  request.icon = std::string("noctalia-glyph:device-mobile");
  // "default" makes a click on the toast do the same as the button.
  request.actions = {"default", action, link ? "open" : "copy", action};
  if (const std::uint32_t id = m_notifications.addOrReplace(std::move(request)); id != 0) {
    m_received[id] = ReceivedShare{.link = link, .text = text};
  }
}

void LinkService::onNotificationPosted(
    const std::string& deviceId, const std::string& id, const std::string& app, const std::string& title,
    const std::string& text, const std::vector<std::uint8_t>& icon,
    const std::vector<sdbus::Struct<std::string, std::string, bool>>& actions
) {
  NotificationRequest request;
  request.replacesId = mirroredId(deviceId, id);
  request.appName = app;
  request.summary = title.empty() ? app : title;
  request.body = text;
  request.origin = NotificationOrigin::Internal;
  request.persistInHistory = true;
  // A glyph icon takes precedence over image data, so it is only the fallback.
  request.imageData = decodeIcon(icon);
  if (!request.imageData.has_value()) {
    request.icon = std::string("noctalia-glyph:device-mobile");
  }
  MirroredNotification mirrored{.deviceId = deviceId, .id = id};
  for (const auto& action : actions) {
    if (!action.get<2>()) {
      request.actions.push_back(std::string(kPhoneActionPrefix) + action.get<0>());
      request.actions.push_back(action.get<1>());
    } else if (mirrored.replyAction.empty()) {
      // The shell offers one reply field per notification, so a second reply action is left out.
      mirrored.replyAction = action.get<0>();
      request.actions.emplace_back("inline-reply");
      request.actions.push_back(action.get<1>());
    }
  }
  if (const std::uint32_t shown = m_notifications.addOrReplace(std::move(request)); shown != 0) {
    m_mirrored[shown] = std::move(mirrored);
  }
  kLog.debug("{}: phone notification from {} shown", deviceName(deviceId), app);
}

void LinkService::onNotificationRemoved(const std::string& deviceId, const std::string& id) {
  const std::uint32_t shown = mirroredId(deviceId, id);
  if (shown == 0) {
    return;
  }
  m_mirrored.erase(shown);
  (void)m_notifications.close(shown, CloseReason::ClosedByCall);
}

void LinkService::ring(const std::string& deviceId, bool on) {
  m_link->callMethodAsync("Ring")
      .onInterface(kLinkInterface)
      .withArguments(deviceId, on)
      .uponReplyInvoke([](std::optional<sdbus::Error> error) { logFailure("Ring", error); });
}

void LinkService::onRingRequested(const std::string& deviceId, bool on) {
  if (!on) {
    if (m_ringingFor == deviceId) {
      stopRinging();
    }
    return;
  }
  if (m_ringingFor.has_value()) {
    return;
  }
  m_ringingFor = deviceId;
  const auto playOnce = [this]() {
    if (const auto sounds = m_sounds.lock()) {
      sounds->playAlert(kRingSound);
    }
  };
  playOnce();
  // The sound player has no loop, so the ring is the sound played again until it stops.
  m_ringRepeat.startRepeating(kRingRepeat, playOnce);
  m_ringLimit.start(kRingLimit, [this]() { stopRinging(); });
  const std::string stop = i18n::tr("notifications.internal.link-ring-stop");
  NotificationRequest request;
  request.appName = i18n::tr("notifications.internal.link");
  request.summary = i18n::tr("notifications.internal.link-ring-title", "device", deviceName(deviceId));
  request.body = i18n::tr("notifications.internal.link-ring-body");
  request.origin = NotificationOrigin::Internal;
  request.urgency = Urgency::Critical;
  request.dndPolicy = NotificationDndPolicy::Bypass;
  request.timeout = 0;
  request.icon = std::string("noctalia-glyph:device-mobile");
  request.actions = {"default", stop, "stop", stop};
  m_ringNotification = m_notifications.addOrReplace(std::move(request));
  m_link->callMethodAsync("DesktopRinging")
      .onInterface(kLinkInterface)
      .withArguments(deviceId, true)
      .uponReplyInvoke([](std::optional<sdbus::Error> error) { logFailure("DesktopRinging", error); });
  notify();
}

void LinkService::stopRinging() {
  if (!m_ringingFor.has_value()) {
    return;
  }
  const std::string deviceId = *std::exchange(m_ringingFor, std::nullopt);
  m_ringRepeat.stop();
  m_ringLimit.stop();
  if (const std::uint32_t shown = std::exchange(m_ringNotification, 0); shown != 0) {
    (void)m_notifications.close(shown, CloseReason::ClosedByCall);
  }
  m_link->callMethodAsync("DesktopRinging")
      .onInterface(kLinkInterface)
      .withArguments(deviceId, false)
      .uponReplyInvoke([](std::optional<sdbus::Error> error) { logFailure("DesktopRinging", error); });
  notify();
}

void LinkService::onNotificationClosed(std::uint32_t id, CloseReason reason) {
  if (id != 0 && id == m_ringNotification) {
    m_ringNotification = 0;
    stopRinging();
    return;
  }
  const auto it = m_mirrored.find(id);
  if (it == m_mirrored.end()) {
    return;
  }
  const MirroredNotification mirrored = std::move(it->second);
  m_mirrored.erase(it);
  if (reason != CloseReason::Dismissed || mirrored.acted) {
    return;
  }
  m_link->callMethodAsync("NotificationDismiss")
      .onInterface(kLinkInterface)
      .withArguments(mirrored.deviceId, mirrored.id)
      .uponReplyInvoke([](std::optional<sdbus::Error> error) { logFailure("NotificationDismiss", error); });
}

std::uint32_t LinkService::mirroredId(const std::string& deviceId, const std::string& id) const {
  const auto it = std::ranges::find_if(m_mirrored, [&](const auto& entry) {
    return entry.second.deviceId == deviceId && entry.second.id == id;
  });
  return it != m_mirrored.end() ? it->first : 0;
}

std::string LinkService::deviceName(const std::string& deviceId) const {
  const auto device = std::ranges::find(m_devices, deviceId, &LinkDevice::id);
  return device != m_devices.end() ? device->name : deviceId;
}

void LinkService::onAction(std::uint32_t id, const std::string& action, const std::string& activationToken) {
  if (id != 0 && id == m_ringNotification) {
    m_ringNotification = 0;
    stopRinging();
    return;
  }
  if (const auto mirrored = m_mirrored.find(id); mirrored != m_mirrored.end()) {
    std::string phoneAction;
    std::string replyText;
    if (action.starts_with(kInlineReplyPrefix) && !mirrored->second.replyAction.empty()) {
      phoneAction = mirrored->second.replyAction;
      replyText = action.substr(kInlineReplyPrefix.size());
    } else if (action.starts_with(kPhoneActionPrefix)) {
      phoneAction = action.substr(kPhoneActionPrefix.size());
    } else {
      return;
    }
    mirrored->second.acted = true;
    m_link->callMethodAsync("NotificationAction")
        .onInterface(kLinkInterface)
        .withArguments(mirrored->second.deviceId, mirrored->second.id, phoneAction, replyText)
        .uponReplyInvoke([](std::optional<sdbus::Error> error) { logFailure("NotificationAction", error); });
    return;
  }
  const auto it = m_received.find(id);
  if (it == m_received.end()) {
    return;
  }
  const ReceivedShare received = std::move(it->second);
  m_received.erase(it);
  if (received.link && (action == "default" || action == "open")) {
    if (!net::openInBrowser(received.text, activationToken)) {
      kLog.warn("opening a received link failed");
    }
  } else if (!received.link && (action == "default" || action == "copy")) {
    (void)m_clipboard.copyText(received.text);
  }
}

void LinkService::registerIpc(IpcService& ipc, std::function<void()> showPairing) {
  ipc.bind(noctalia::cli::msg::linkDevices, [this](const std::string&) -> std::string {
    if (!m_available) {
      return "error: umbriel-linkd is not running\n";
    }
    std::string out;
    for (const auto& device : m_devices) {
      out += device.id + (device.connected ? " connected " : " disconnected ") + device.name + "\n";
    }
    return out;
  });
  ipc.bind(
      noctalia::cli::msg::linkPair, [this, showPairing = std::move(showPairing)](const std::string&) -> std::string {
        if (!m_available) {
          return "error: umbriel-linkd is not running\n";
        }
        startPairing();
        showPairing();
        return "ok\n";
      }
  );
  ipc.bind(noctalia::cli::msg::linkPairing, [this](const std::string&) -> std::string {
    if (m_pairing.has_value()) {
      return "open " + m_pairing->code + " " + m_pairing->uri + "\n";
    }
    if (m_outcome.has_value()) {
      return (m_outcome->paired ? "paired " : "failed ") + m_outcome->detail + "\n";
    }
    return "none\n";
  });
  ipc.bind(noctalia::cli::msg::linkUnpair, [this](const std::string& args) -> std::string {
    const std::string id = StringUtils::trim(args);
    if (!std::ranges::contains(m_devices, id, &LinkDevice::id)) {
      return "error: no paired device " + id + "\n";
    }
    unpair(id);
    return "ok\n";
  });
  ipc.bind(noctalia::cli::msg::linkRing, [this](const std::string& args) -> std::string {
    const std::string trimmed = StringUtils::trim(args);
    const auto space = trimmed.find(' ');
    const std::string id = trimmed.substr(0, space);
    const std::string mode = space == std::string::npos ? std::string() : StringUtils::trim(trimmed.substr(space + 1));
    const auto device = std::ranges::find(m_devices, id, &LinkDevice::id);
    if (device == m_devices.end()) {
      return "error: no paired device " + id + "\n";
    }
    if (!device->connected) {
      return "error: " + device->name + " is not connected\n";
    }
    if (!mode.empty() && mode != "stop") {
      return "error: link-ring <device-id> [stop]\n";
    }
    ring(id, mode.empty());
    return "ok\n";
  });
  ipc.bind(noctalia::cli::msg::linkRinging, [this](const std::string&) -> std::string {
    std::string out;
    if (m_ringingFor.has_value()) {
      out += "desktop " + *m_ringingFor + "\n";
    }
    for (const auto& id : m_phonesRinging) {
      out += "phone " + id + "\n";
    }
    return out.empty() ? "none\n" : out;
  });
  ipc.bind(noctalia::cli::msg::linkShare, [this](const std::string& args) -> std::string {
    const std::string trimmed = StringUtils::trim(args);
    const auto firstSpace = trimmed.find(' ');
    const auto secondSpace = firstSpace == std::string::npos ? firstSpace : trimmed.find(' ', firstSpace + 1);
    if (secondSpace == std::string::npos) {
      return "error: link-share <device-id> <text|link> <text>\n";
    }
    const std::string id = trimmed.substr(0, firstSpace);
    const std::string kind = trimmed.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    const auto device = std::ranges::find(m_devices, id, &LinkDevice::id);
    if (device == m_devices.end()) {
      return "error: no paired device " + id + "\n";
    }
    if (!device->connected) {
      return "error: " + device->name + " is not connected\n";
    }
    if (kind != "text" && kind != "link") {
      return "error: kind is text or link\n";
    }
    share(id, kind, trimmed.substr(secondSpace + 1));
    return "ok\n";
  });
}
