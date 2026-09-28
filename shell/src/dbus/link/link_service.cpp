#include "dbus/link/link_service.h"

#include "core/log.h"
#include "core/process/process.h"
#include "dbus/session_bus.h"
#include "i18n/i18n.h"
#include "ipc/ipc_arg_parse.h"
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
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <glib.h>
#include <map>
#include <ranges>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>
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
  constexpr std::array<std::uint8_t, 16> kPngHeader{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n',
                                                    0,    0,   0,   13,  'I',  'H',  'D',  'R'};

  using VariantMap = std::map<std::string, sdbus::Variant>;

  void logFailure(std::string_view method, const std::optional<sdbus::Error>& error) {
    if (error.has_value()) {
      kLog.warn("{} failed: {}", method, error->what());
    }
  }

  std::string formatBytes(std::uint64_t bytes) {
    static constexpr std::array kUnits{"B", "KB", "MB", "GB", "TB"};
    auto size = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (size >= 1024.0 && unit + 1 < kUnits.size()) {
      size /= 1024.0;
      ++unit;
    }
    return unit == 0 ? std::format("{} {}", bytes, kUnits[0]) : std::format("{:.1f} {}", size, kUnits[unit]);
  }

  // The protocol's limits on a clipboard offer.
  constexpr std::size_t kMaxClipboardBytes = 64U << 20U;
  constexpr std::size_t kMaxClipboardTypes = 16;

  std::string fileUri(const std::filesystem::path& path) {
    gchar* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
    if (uri == nullptr) {
      return {};
    }
    std::string out(uri);
    g_free(uri);
    return out;
  }

  std::uint32_t readBigEndian(const std::uint8_t* bytes) {
    return (std::uint32_t{bytes[0]} << 24)
        | (std::uint32_t{bytes[1]} << 16)
        | (std::uint32_t{bytes[2]} << 8)
        | std::uint32_t{bytes[3]};
  }

  // The PNG's size is read from its header before anything is decoded.
  std::optional<NotificationImageData> decodeIcon(const std::vector<std::uint8_t>& png) {
    if (png.size() < kPngHeader.size() + 8
        || !std::ranges::equal(kPngHeader, png | std::views::take(kPngHeader.size()))) {
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
  m_link->uponSignal("TransferOffered")
      .onInterface(kLinkInterface)
      .call([this](
                const std::string& transferId, const std::string& deviceId,
                const std::vector<sdbus::Struct<std::string, std::uint64_t>>& files
            ) {
        std::vector<std::pair<std::string, std::uint64_t>> offered;
        offered.reserve(files.size());
        for (const auto& file : files) {
          offered.emplace_back(file.get<0>(), file.get<1>());
        }
        onOffered(transferId, deviceId, offered);
      });
  m_link->uponSignal("TransferProgress")
      .onInterface(kLinkInterface)
      .call([this](const std::string& transferId, std::uint64_t bytes, std::uint64_t total) {
        onProgress(transferId, bytes, total);
      });
  m_link->uponSignal("TransferFinished")
      .onInterface(kLinkInterface)
      .call([this](const std::string& transferId, const std::string& status, const std::vector<std::string>& paths) {
        onFinished(transferId, status, paths);
      });
  m_link->uponSignal("ClipboardOffered")
      .onInterface(kLinkInterface)
      .call([this](
                const std::string& deviceId, std::uint64_t id, const std::vector<std::string>& mimeTypes,
                std::uint64_t size
            ) { onClipboardOffered(deviceId, id, mimeTypes, size); });
  m_clipboard.setSelectionListener([this](
                                       const std::vector<std::string>& mimeTypes, const std::string& dataMimeType,
                                       const std::vector<std::uint8_t>& data
                                   ) { onLocalClipboard(mimeTypes, dataMimeType, data); });
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
  m_link->uponSignal("Call")
      .onInterface(kLinkInterface)
      .call([this](
                const std::string& deviceId, const std::string& state, const std::string& number,
                const std::string& name
            ) { onCall(deviceId, state, number, name); });
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
  m_link->uponSignal("Hotspot")
      .onInterface(kLinkInterface)
      .call([this](const std::string& deviceId, const std::string& ssid) { onHotspot(deviceId, ssid); });
  m_link->uponSignal("NotificationRemoved")
      .onInterface(kLinkInterface)
      .call([this](const std::string& deviceId, const std::string& id) { onNotificationRemoved(deviceId, id); });
  m_notifications.addEventCallback([this](const Notification& notification, NotificationEvent event) {
    if (event != NotificationEvent::Closed) {
      return;
    }
    m_received.erase(notification.id);
    m_transferActions.erase(notification.id);
    for (auto& [id, transfer] : m_transfers) {
      if (transfer.progressNotification == notification.id) {
        transfer.progressNotification = 0;
        transfer.progressDismissed = true;
      }
      if (transfer.offerNotification == notification.id) {
        transfer.offerNotification = 0;
      }
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
  if (const auto it = properties.find("LocalSendVisible"); it != properties.end()) {
    try {
      m_localSendVisible = it->second.get<bool>();
    } catch (const sdbus::Error& e) {
      kLog.warn("malformed LocalSendVisible: {}", e.what());
    }
  }
  if (const auto it = properties.find("Nearby"); it != properties.end()) {
    try {
      m_nearby.clear();
      for (const auto& peer : it->second.get<std::vector<sdbus::Struct<std::string, std::string>>>()) {
        m_nearby.push_back({.id = peer.get<0>(), .alias = peer.get<1>()});
      }
    } catch (const sdbus::Error& e) {
      kLog.warn("malformed Nearby: {}", e.what());
    }
  }
  if (const auto it = properties.find("DeviceStatus"); it != properties.end()) {
    try {
      m_status.clear();
      for (const auto& [id, status] :
           it->second.get<std::map<std::string, sdbus::Struct<std::uint32_t, bool, std::string>>>()) {
        m_status[id] = LinkStatus{.battery = status.get<0>(), .charging = status.get<1>(), .network = status.get<2>()};
      }
    } catch (const sdbus::Error& e) {
      kLog.warn("malformed DeviceStatus: {}", e.what());
    }
  }
  if (const auto it = properties.find("Grants"); it != properties.end()) {
    try {
      m_grants = it->second.get<std::map<std::string, std::vector<std::string>>>();
    } catch (const sdbus::Error& e) {
      kLog.warn("malformed Grants: {}", e.what());
    }
  }
  if (const auto it = properties.find("AutoAccept"); it != properties.end()) {
    try {
      m_autoAccept = it->second.get<std::vector<std::string>>();
    } catch (const sdbus::Error& e) {
      kLog.warn("malformed AutoAccept: {}", e.what());
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
  m_status.clear();
  m_nearby.clear();
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

// Joining a phone's hotspot takes the laptop off its Wi-Fi, so it is said while it lasts, and when it ends.
void LinkService::onHotspot(const std::string& deviceId, const std::string& ssid) {
  const bool joined = !ssid.empty();
  NotificationRequest request;
  request.appName = i18n::tr("notifications.internal.link");
  request.summary = i18n::tr(
      joined ? "notifications.internal.link-hotspot-joined" : "notifications.internal.link-hotspot-left", "device",
      deviceName(deviceId)
  );
  request.body = joined ? i18n::tr("notifications.internal.link-hotspot-joined-body") : std::string();
  request.origin = NotificationOrigin::Internal;
  request.icon = std::string("noctalia-glyph:device-mobile");
  request.replacesId = m_hotspotNotice;
  request.timeout = joined ? 0 : 5000;
  const std::uint32_t shown = m_notifications.addOrReplace(std::move(request));
  m_hotspotNotice = joined ? shown : 0;
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

std::vector<std::string> LinkService::sendFiles(const std::string& deviceId, const std::vector<std::string>& paths) {
  std::vector<sdbus::Struct<sdbus::UnixFd, std::string>> files;
  std::vector<std::string> failed;
  for (const auto& path : paths) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) {
      kLog.warn("opening {} to send: {}", path, std::strerror(errno));
      failed.push_back(path);
      continue;
    }
    files.emplace_back(sdbus::UnixFd{fd, sdbus::adopt_fd}, std::filesystem::path(path).filename().string());
  }
  if (files.empty()) {
    return failed;
  }
  m_link->callMethodAsync("SendFiles")
      .onInterface(kLinkInterface)
      .withArguments(deviceId, files)
      .uponReplyInvoke([this, deviceId](std::optional<sdbus::Error> error, std::string transferId) {
        if (error.has_value()) {
          logFailure("SendFiles", error);
          return;
        }
        m_transfers[transferId] = Transfer{.deviceId = deviceId, .incoming = false};
      });
  return failed;
}

void LinkService::setAutoAccept(const std::string& deviceId, bool enabled) {
  m_link->callMethodAsync("SetAutoAccept")
      .onInterface(kLinkInterface)
      .withArguments(deviceId, enabled)
      .uponReplyInvoke([](std::optional<sdbus::Error> error) { logFailure("SetAutoAccept", error); });
}

void LinkService::setGrant(const std::string& deviceId, const std::string& feature, bool granted) {
  m_link->callMethodAsync("SetGrant")
      .onInterface(kLinkInterface)
      .withArguments(deviceId, feature, granted)
      .uponReplyInvoke([](std::optional<sdbus::Error> error) { logFailure("SetGrant", error); });
}

const LinkStatus* LinkService::status(const std::string& deviceId) const {
  const auto it = m_status.find(deviceId);
  return it != m_status.end() ? &it->second : nullptr;
}

bool LinkService::granted(const std::string& deviceId, std::string_view feature) const {
  const auto it = m_grants.find(deviceId);
  return it != m_grants.end() && std::ranges::contains(it->second, feature);
}

void LinkService::onLocalClipboard(
    const std::vector<std::string>& mimeTypes, const std::string& dataMimeType, const std::vector<std::uint8_t>& data
) {
  const bool anyone = std::ranges::any_of(m_devices, [this](const LinkDevice& device) {
    return device.connected && granted(device.id, "clipboard");
  });
  if (!m_available || !anyone || data.size() > kMaxClipboardBytes) {
    return;
  }
  const std::size_t hash =
      std::hash<std::string_view>{}(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
  if (hash == m_lastClipboardHash) {
    return;
  }
  m_lastClipboardHash = hash;
  // The first type is the one the bytes are; the daemon offers the rest as its alternatives.
  std::vector<std::string> offered{dataMimeType};
  for (const auto& mimeType : mimeTypes) {
    if (mimeType != dataMimeType && mimeType.contains('/')) {
      offered.push_back(mimeType);
    }
  }
  offered.resize(std::min<std::size_t>(offered.size(), kMaxClipboardTypes));
  const int fd = memfd_create("link-clipboard", MFD_CLOEXEC);
  if (fd < 0) {
    kLog.warn("memfd for the clipboard: {}", std::strerror(errno));
    return;
  }
  sdbus::UnixFd memfd{fd, sdbus::adopt_fd};
  if (::write(fd, data.data(), data.size()) != static_cast<ssize_t>(data.size()) || ::lseek(fd, 0, SEEK_SET) != 0) {
    kLog.warn("writing the clipboard memfd: {}", std::strerror(errno));
    return;
  }
  m_link->callMethodAsync("OfferClipboard")
      .onInterface(kLinkInterface)
      .withArguments(offered, memfd)
      .uponReplyInvoke([](std::optional<sdbus::Error> error) { logFailure("OfferClipboard", error); });
}

void LinkService::onClipboardOffered(
    const std::string& deviceId, std::uint64_t id, const std::vector<std::string>& mimeTypes, std::uint64_t /*size*/
) {
  // The daemon forwards only offers from devices holding the clipboard grant.
  const bool offered = m_clipboard.offerRemote(mimeTypes, [this, deviceId, id](const std::string& mimeType, int fd) {
    m_link->callMethodAsync("PullClipboard")
        .onInterface(kLinkInterface)
        .withArguments(deviceId, id, mimeType, sdbus::UnixFd{fd, sdbus::adopt_fd})
        .uponReplyInvoke([](std::optional<sdbus::Error> error, std::uint64_t /*bytes*/) {
          logFailure("PullClipboard", error);
        });
  });
  if (!offered) {
    kLog.warn("could not take the selection for {}'s clipboard", deviceId);
  }
}

void LinkService::callTransfer(const char* method, const std::string& transferId) {
  m_link->callMethodAsync(method)
      .onInterface(kLinkInterface)
      .withArguments(transferId)
      .uponReplyInvoke([method](std::optional<sdbus::Error> error) { logFailure(method, error); });
}

std::string LinkService::deviceName(const std::string& deviceId) const {
  if (const auto device = std::ranges::find(m_devices, deviceId, &LinkDevice::id); device != m_devices.end()) {
    return device->name;
  }
  const auto peer = std::ranges::find(m_nearby, deviceId, &LinkNearby::id);
  return peer != m_nearby.end() ? peer->alias : deviceId;
}

void LinkService::browse(const std::string& deviceId) {
  const char* home = std::getenv("HOME");
  if (home == nullptr) {
    return;
  }
  // umbriel-link-mount names a phone's folder after it, with any slash swapped for U+2215 (∕).
  std::string name = deviceName(deviceId);
  for (std::size_t at = 0; (at = name.find('/', at)) != std::string::npos;) {
    name.replace(at, 1, "\u2215");
  }
  (void)net::openInBrowser(fileUri(std::filesystem::path(home) / "Phone" / name));
}

void LinkService::mirror(const std::string& deviceId) {
  if (!process::runAsync(std::vector<std::string>{"umbriel-link-mirror", deviceId, "--name", deviceName(deviceId)})) {
    kLog.warn("could not start umbriel-link-mirror");
  }
}

void LinkService::apps(const std::string& deviceId) {
  if (!process::runAsync(std::vector<std::string>{"umbriel-link-apps", deviceName(deviceId)})) {
    kLog.warn("could not start umbriel-link-apps");
  }
}

void LinkService::setLocalSendVisible(bool visible) {
  m_link->callMethodAsync("SetLocalSendVisible")
      .onInterface(kLinkInterface)
      .withArguments(visible)
      .uponReplyInvoke([](std::optional<sdbus::Error> error) { logFailure("SetLocalSendVisible", error); });
}

void LinkService::onOffered(
    const std::string& transferId, const std::string& deviceId,
    const std::vector<std::pair<std::string, std::uint64_t>>& files
) {
  if (files.empty()) {
    return;
  }
  std::uint64_t total = 0;
  std::string body;
  for (const auto& [name, size] : files) {
    total += size;
    body += (body.empty() ? "" : "\n") + name;
  }
  const std::string name = deviceName(deviceId);
  NotificationRequest request;
  request.appName = i18n::tr("notifications.internal.link");
  request.summary = files.size() == 1 ? i18n::tr(
                                            "notifications.internal.link-offer-one", "device", name, "name",
                                            files.front().first, "size", formatBytes(total)
                                        )
                                      : i18n::tr(
                                            "notifications.internal.link-offer", "device", name, "count",
                                            std::to_string(files.size()), "size", formatBytes(total)
                                        );
  request.body = body;
  request.origin = NotificationOrigin::Internal;
  request.icon = std::string("noctalia-glyph:device-mobile");
  // The daemon declines after 120 s without an answer.
  request.timeout = 120000;
  request.actions = {
      "accept", i18n::tr("notifications.internal.link-accept"), "decline",
      i18n::tr("notifications.internal.link-decline")
  };
  const std::uint32_t id = m_notifications.addOrReplace(std::move(request));
  m_transfers[transferId] = Transfer{.deviceId = deviceId, .incoming = true, .offerNotification = id};
  if (id != 0) {
    m_transferActions[id] = TransferAction{.transferId = transferId, .paths = {}};
  }
}

void LinkService::onProgress(const std::string& transferId, std::uint64_t bytes, std::uint64_t total) {
  auto it = m_transfers.find(transferId);
  if (it == m_transfers.end()) {
    // An auto-accepted offer announces itself with its first progress.
    it = m_transfers.emplace(transferId, Transfer{.deviceId = {}, .incoming = true}).first;
  }
  Transfer& transfer = it->second;
  closeTransferNotification(transfer.offerNotification);
  const std::string body =
      i18n::tr("notifications.internal.link-progress", "done", formatBytes(bytes), "total", formatBytes(total));
  if (transfer.progressDismissed) {
    return;
  }
  if (transfer.progressNotification != 0 && m_notifications.updateBody(transfer.progressNotification, body)) {
    return;
  }
  const std::string device =
      transfer.deviceId.empty() ? i18n::tr("notifications.internal.link") : deviceName(transfer.deviceId);
  NotificationRequest request;
  request.appName = i18n::tr("notifications.internal.link");
  request.summary = i18n::tr(
      transfer.incoming ? "notifications.internal.link-receiving" : "notifications.internal.link-sending", "device",
      device
  );
  request.body = body;
  request.origin = NotificationOrigin::Internal;
  request.icon = std::string("noctalia-glyph:device-mobile");
  request.timeout = 0;
  request.transient = true;
  request.actions = {"cancel", i18n::tr("notifications.internal.link-cancel")};
  transfer.progressNotification = m_notifications.addOrReplace(std::move(request));
  if (transfer.progressNotification != 0) {
    m_transferActions[transfer.progressNotification] = TransferAction{.transferId = transferId, .paths = {}};
  }
}

void LinkService::closeTransferNotification(std::uint32_t& id) {
  if (id == 0) {
    return;
  }
  const std::uint32_t closing = id;
  id = 0;
  m_transferActions.erase(closing);
  (void)m_notifications.close(closing);
}

void LinkService::onFinished(
    const std::string& transferId, const std::string& status, const std::vector<std::string>& paths
) {
  const auto it = m_transfers.find(transferId);
  if (it == m_transfers.end()) {
    return;
  }
  Transfer transfer = std::move(it->second);
  m_transfers.erase(it);
  closeTransferNotification(transfer.offerNotification);
  closeTransferNotification(transfer.progressNotification);
  const std::string device =
      transfer.deviceId.empty() ? i18n::tr("notifications.internal.link") : deviceName(transfer.deviceId);
  NotificationRequest request;
  request.appName = i18n::tr("notifications.internal.link");
  request.origin = NotificationOrigin::Internal;
  request.icon = std::string("noctalia-glyph:device-mobile");
  if (status == "done") {
    request.summary = i18n::tr(
        transfer.incoming ? "notifications.internal.link-received-files" : "notifications.internal.link-sent-files",
        "device", device
    );
    for (const auto& path : paths) {
      request.body += (request.body.empty() ? "" : "\n") + std::filesystem::path(path).filename().string();
    }
    if (!paths.empty()) {
      const std::string open = i18n::tr("notifications.internal.link-open");
      request.actions = {"default", open, "open", open, "folder", i18n::tr("notifications.internal.link-show-folder")};
    }
  } else {
    const bool known = status == "declined"
        || status == "no-space"
        || status == "too-large"
        || status == "busy"
        || status == "cancelled";
    request.summary = i18n::tr(
        known ? "notifications.internal.link-transfer-" + status
              : std::string("notifications.internal.link-transfer-failed"),
        "device", device
    );
  }
  if (const std::uint32_t id = m_notifications.addOrReplace(std::move(request)); id != 0 && !paths.empty()) {
    m_transferActions[id] = TransferAction{.transferId = transferId, .paths = paths};
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

void LinkService::onCall(
    const std::string& deviceId, const std::string& state, const std::string& number, const std::string& name
) {
  const auto shown = m_callNotifications.find(deviceId);
  if (state != "ringing") {
    if (shown != m_callNotifications.end()) {
      const std::uint32_t id = shown->second;
      m_callNotifications.erase(shown);
      m_callDevices.erase(id);
      (void)m_notifications.close(id, CloseReason::ClosedByCall);
    }
    return;
  }
  const std::string caller =
      !name.empty() ? name : (!number.empty() ? number : i18n::tr("notifications.internal.link-call-unknown"));
  NotificationRequest request;
  request.replacesId = shown != m_callNotifications.end() ? shown->second : 0;
  request.appName = i18n::tr("notifications.internal.link");
  request.summary = i18n::tr("notifications.internal.link-call-title", "caller", caller);
  request.body = name.empty() || number.empty()
      ? i18n::tr("notifications.internal.link-call-body", "device", deviceName(deviceId))
      : number + " · " + i18n::tr("notifications.internal.link-call-body", "device", deviceName(deviceId));
  request.origin = NotificationOrigin::Internal;
  request.urgency = Urgency::Critical;
  request.timeout = 0;
  request.icon = std::string("noctalia-glyph:phone-call");
  request.actions = {
      "call:mute", i18n::tr("notifications.internal.link-call-mute"), "call:decline",
      i18n::tr("notifications.internal.link-call-decline")
  };
  if (const std::uint32_t id = m_notifications.addOrReplace(std::move(request)); id != 0) {
    m_callNotifications[deviceId] = id;
    m_callDevices[id] = deviceId;
  }
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
  if (const auto call = m_callDevices.find(id); call != m_callDevices.end()) {
    m_callNotifications.erase(call->second);
    m_callDevices.erase(call);
    return;
  }
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

void LinkService::onAction(std::uint32_t id, const std::string& action, const std::string& activationToken) {
  if (const auto call = m_callDevices.find(id); call != m_callDevices.end() && action.starts_with("call:")) {
    m_link->callMethodAsync("CallAction")
        .onInterface(kLinkInterface)
        .withArguments(call->second, action.substr(5))
        .uponReplyInvoke([](std::optional<sdbus::Error> error) { logFailure("CallAction", error); });
    return;
  }
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
  if (const auto transfer = m_transferActions.find(id); transfer != m_transferActions.end()) {
    const TransferAction target = transfer->second;
    m_transferActions.erase(transfer);
    if (action == "accept") {
      callTransfer("AcceptTransfer", target.transferId);
    } else if (action == "decline") {
      callTransfer("DeclineTransfer", target.transferId);
    } else if (action == "cancel") {
      callTransfer("CancelTransfer", target.transferId);
    } else if ((action == "default" || action == "open") && !target.paths.empty()) {
      (void)net::openInBrowser(fileUri(target.paths.front()), activationToken);
    } else if (action == "folder" && !target.paths.empty()) {
      (void)net::openInBrowser(fileUri(std::filesystem::path(target.paths.front()).parent_path()), activationToken);
    }
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
  ipc.bind(noctalia::cli::msg::linkSendFile, [this](const std::string& args) -> std::string {
    std::vector<std::string> words = noctalia::ipc::splitWords(args);
    if (words.size() < 2) {
      return "error: link-send-file <device-id> <path>...\n";
    }
    const std::string id = words.front();
    const auto device = std::ranges::find(m_devices, id, &LinkDevice::id);
    if (device == m_devices.end()) {
      return "error: no paired device " + id + "\n";
    }
    if (!device->connected) {
      return "error: " + device->name + " is not connected\n";
    }
    words.erase(words.begin());
    const std::vector<std::string> failed = sendFiles(id, words);
    if (!failed.empty()) {
      return "error: cannot open " + failed.front() + "\n";
    }
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
