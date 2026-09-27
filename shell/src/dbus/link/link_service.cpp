#include "dbus/link/link_service.h"

#include "core/log.h"
#include "dbus/session_bus.h"
#include "i18n/i18n.h"
#include "ipc/ipc_arg_parse.h"
#include "ipc/ipc_service.h"
#include "net/url_open.h"
#include "notification/notification_manager.h"
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
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
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

  std::string fileUri(const std::filesystem::path& path) {
    gchar* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
    if (uri == nullptr) {
      return {};
    }
    std::string out(uri);
    g_free(uri);
    return out;
  }

} // namespace

LinkService::LinkService(SessionBus& bus, NotificationManager& notifications, ClipboardService& clipboard)
    : m_notifications(notifications), m_clipboard(clipboard) {
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
  m_notifications.addInternalActionCallback(
      [this](std::uint32_t id, const std::string& action, const std::string& activationToken) {
        onAction(id, action, activationToken);
      }
  );
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

void LinkService::callTransfer(const char* method, const std::string& transferId) {
  m_link->callMethodAsync(method)
      .onInterface(kLinkInterface)
      .withArguments(transferId)
      .uponReplyInvoke([method](std::optional<sdbus::Error> error) { logFailure(method, error); });
}

std::string LinkService::deviceName(const std::string& deviceId) const {
  const auto device = std::ranges::find(m_devices, deviceId, &LinkDevice::id);
  return device != m_devices.end() ? device->name : deviceId;
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

void LinkService::onAction(std::uint32_t id, const std::string& action, const std::string& activationToken) {
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
