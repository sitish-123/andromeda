#include "dbus/link/quickshare_service.h"

#include "cli/schema_msg.h"
#include "core/log.h"
#include "dbus/session_bus.h"
#include "i18n/i18n.h"
#include "ipc/ipc_service.h"
#include "net/url_open.h"
#include "notification/notification_manager.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"

#include <array>
#include <filesystem>
#include <format>
#include <glib.h>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <utility>

namespace {

  constexpr Logger kLog("quick-share");

  const sdbus::ServiceName kLinkBusName{"org.umbriel.Link1"};
  const sdbus::ObjectPath kLinkPath{"/org/umbriel/Link1"};
  constexpr auto kInterface = "org.umbriel.Link1.QuickShare";
  const sdbus::ServiceName kBusName{"org.freedesktop.DBus"};
  const sdbus::ObjectPath kBusPath{"/org/freedesktop/DBus"};
  constexpr auto kBusInterface = "org.freedesktop.DBus";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";
  const sdbus::ServiceName kFileManagerName{"org.freedesktop.FileManager1"};
  const sdbus::ObjectPath kFileManagerPath{"/org/freedesktop/FileManager1"};
  // The daemon declines an unanswered offer after 60 s; the toast goes with it.
  constexpr std::int32_t kOfferTimeoutMs = 60000;

  using VariantMap = std::map<std::string, sdbus::Variant>;

  std::string humanSize(std::int64_t bytes) {
    constexpr std::array kUnits{"B", "KB", "MB", "GB", "TB"};
    auto value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1000.0 && unit + 1 < kUnits.size()) {
      value /= 1000.0;
      ++unit;
    }
    return unit == 0 ? std::format("{} B", bytes) : std::format("{:.1f} {}", value, kUnits[unit]);
  }

  std::string fileUri(const std::string& path) {
    gchar* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
    if (uri == nullptr) {
      return {};
    }
    std::string out = uri;
    g_free(uri);
    return out;
  }

} // namespace

QuickShareService::QuickShareService(SessionBus& bus, NotificationManager& notifications, ClipboardService& clipboard)
    : m_notifications(notifications), m_clipboard(clipboard) {
  m_bus = sdbus::createProxy(bus.connection(), kBusName, kBusPath);
  m_bus->uponSignal("NameOwnerChanged")
      .onInterface(kBusInterface)
      .call([this](const std::string& name, const std::string& /*oldOwner*/, const std::string& newOwner) {
        if (name != kLinkBusName) {
          return;
        }
        m_available = false;
        m_visible = false;
        notify();
        if (!newOwner.empty()) {
          refresh();
        }
      });

  m_proxy = sdbus::createProxy(bus.connection(), kLinkBusName, kLinkPath);
  m_proxy->uponSignal("PropertiesChanged")
      .onInterface(kPropertiesInterface)
      .call([this](const std::string& interfaceName, const VariantMap& changed, const std::vector<std::string>&) {
        if (interfaceName == kInterface) {
          apply(changed);
        }
      });
  m_proxy->uponSignal("Offer")
      .onInterface(kInterface)
      .call([this](
                std::uint64_t id, const std::string& sender, const std::string& pin,
                const std::vector<sdbus::Struct<std::string, std::int64_t>>& files, const std::vector<std::string>& texts
            ) {
        std::vector<std::pair<std::string, std::int64_t>> list;
        list.reserve(files.size());
        for (const auto& file : files) {
          list.emplace_back(file.get<0>(), file.get<1>());
        }
        onOffer(id, sender, pin, list, texts);
      });
  m_proxy->uponSignal("Finished")
      .onInterface(kInterface)
      .call([this](
                std::uint64_t id, const std::string& status, const std::vector<std::string>& files,
                const std::vector<sdbus::Struct<std::string, std::string>>& texts, const std::string& error
            ) {
        std::vector<std::pair<std::string, std::string>> list;
        list.reserve(texts.size());
        for (const auto& text : texts) {
          list.emplace_back(text.get<0>(), text.get<1>());
        }
        onFinished(id, status, files, list, error);
      });
  m_fileManager = sdbus::createProxy(bus.connection(), kFileManagerName, kFileManagerPath);
  m_notifications.addInternalActionCallback(
      [this](std::uint32_t id, const std::string& action, const std::string& activationToken) {
        onAction(id, action, activationToken);
      }
  );

  m_bus->callMethodAsync("NameHasOwner")
      .onInterface(kBusInterface)
      .withArguments(std::string{kLinkBusName})
      .uponReplyInvoke([this](std::optional<sdbus::Error> error, bool owned) {
        if (!error.has_value() && owned) {
          refresh();
        }
      });
}

QuickShareService::~QuickShareService() = default;

void QuickShareService::refresh() {
  m_proxy->callMethodAsync("GetAll")
      .onInterface(kPropertiesInterface)
      .withArguments(std::string{kInterface})
      .uponReplyInvoke([this](std::optional<sdbus::Error> error, VariantMap properties) {
        if (error.has_value()) {
          kLog.debug("quick share unavailable: {}", error->what());
          return;
        }
        m_available = true;
        apply(properties);
      });
}

void QuickShareService::apply(const std::map<std::string, sdbus::Variant>& properties) {
  try {
    if (const auto it = properties.find("Visible"); it != properties.end()) {
      m_visible = it->second.get<bool>();
    }
    if (const auto it = properties.find("Name"); it != properties.end()) {
      m_name = it->second.get<std::string>();
    }
  } catch (const sdbus::Error& e) {
    kLog.warn("malformed quick share property: {}", e.what());
  }
  notify();
}

void QuickShareService::notify() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void QuickShareService::setVisible(bool visible) {
  m_proxy->callMethodAsync("Set")
      .onInterface(kPropertiesInterface)
      .withArguments(std::string{kInterface}, std::string{"Visible"}, sdbus::Variant{visible})
      .uponReplyInvoke([this](std::optional<sdbus::Error> error) {
        if (error.has_value()) {
          kLog.warn("setting quick share visibility failed: {}", error->what());
          m_notifications.addInternal(i18n::tr("quick-share.app"), i18n::tr("quick-share.visible-failed"), error->getMessage());
        }
      });
}

void QuickShareService::onOffer(
    std::uint64_t id, const std::string& sender, const std::string& pin,
    const std::vector<std::pair<std::string, std::int64_t>>& files, const std::vector<std::string>& texts
) {
  std::string body;
  for (const auto& [name, size] : files) {
    body += std::format("{} ({})\n", name, humanSize(size));
  }
  for (const auto& kind : texts) {
    body += i18n::tr(kind == "url" ? "quick-share.a-link" : "quick-share.some-text") + "\n";
  }
  body += i18n::tr("quick-share.pin", "pin", pin);
  NotificationRequest request;
  request.appName = i18n::tr("quick-share.app");
  request.summary = i18n::tr("quick-share.offer", "device", sender);
  request.body = std::move(body);
  request.origin = NotificationOrigin::Internal;
  request.icon = std::string("noctalia-glyph:share");
  request.timeout = kOfferTimeoutMs;
  request.actions = {"accept", i18n::tr("quick-share.accept"), "decline", i18n::tr("quick-share.decline")};
  if (const std::uint32_t notification = m_notifications.addOrReplace(std::move(request)); notification != 0) {
    m_offers[id] = Pending{.sender = sender};
    m_offerNotifications[notification] = id;
  }
}

void QuickShareService::onFinished(
    std::uint64_t id, const std::string& status, const std::vector<std::string>& files,
    const std::vector<std::pair<std::string, std::string>>& texts, const std::string& error
) {
  const auto offer = m_offers.find(id);
  const std::string sender = offer != m_offers.end() ? offer->second.sender : std::string{};
  if (offer != m_offers.end()) {
    m_offers.erase(offer);
  }
  std::erase_if(m_offerNotifications, [id](const auto& entry) { return entry.second == id; });
  if (status == "failed") {
    m_notifications.addInternal(i18n::tr("quick-share.app"), i18n::tr("quick-share.failed"), error);
    return;
  }
  if (status != "received") {
    return;
  }
  for (const auto& [kind, text] : texts) {
    const bool link = kind == "url";
    const std::string action = i18n::tr(link ? "quick-share.open" : "quick-share.copy");
    NotificationRequest request;
    request.appName = i18n::tr("quick-share.app");
    request.summary = i18n::tr(link ? "quick-share.received-link" : "quick-share.received-text", "device", sender);
    request.body = text;
    request.origin = NotificationOrigin::Internal;
    request.icon = std::string("noctalia-glyph:share");
    request.actions = {"default", action, link ? "open" : "copy", action};
    if (const std::uint32_t notification = m_notifications.addOrReplace(std::move(request)); notification != 0) {
      m_results[notification] = Result{.text = text, .link = link};
    }
  }
  if (files.empty()) {
    return;
  }
  std::string body;
  for (const auto& path : files) {
    body += std::filesystem::path(path).filename().string() + "\n";
  }
  NotificationRequest request;
  request.appName = i18n::tr("quick-share.app");
  request.summary = i18n::tr("quick-share.received-files", "device", sender);
  request.body = StringUtils::trim(body);
  request.origin = NotificationOrigin::Internal;
  request.icon = std::string("noctalia-glyph:share");
  request.persistInHistory = true;
  request.actions = {
      "default", i18n::tr("quick-share.open"), "open", i18n::tr("quick-share.open"), "folder",
      i18n::tr("quick-share.show-in-folder"),
  };
  if (const std::uint32_t notification = m_notifications.addOrReplace(std::move(request)); notification != 0) {
    m_results[notification] = Result{.files = files};
  }
}

void QuickShareService::onAction(std::uint32_t notification, const std::string& action, const std::string& activationToken) {
  if (const auto offer = m_offerNotifications.find(notification); offer != m_offerNotifications.end()) {
    const std::uint64_t id = offer->second;
    m_offerNotifications.erase(offer);
    if (action == "accept" || action == "decline") {
      call(action == "accept" ? "Accept" : "Decline", id);
    }
    return;
  }
  const auto it = m_results.find(notification);
  if (it == m_results.end()) {
    return;
  }
  const Result result = std::move(it->second);
  m_results.erase(it);
  if (!result.files.empty()) {
    // A single file opens itself; several open their folder, where they sit together.
    const bool folder = action == "folder" || result.files.size() > 1;
    if (folder) {
      std::vector<std::string> uris;
      uris.reserve(result.files.size());
      for (const auto& path : result.files) {
        uris.push_back(fileUri(path));
      }
      m_fileManager->callMethodAsync("ShowItems")
          .onInterface("org.freedesktop.FileManager1")
          .withArguments(uris, activationToken)
          .uponReplyInvoke([dir = std::filesystem::path(result.files.front()).parent_path().string(),
                            activationToken](std::optional<sdbus::Error> error) {
            if (error.has_value() && !net::openInBrowser(fileUri(dir), activationToken)) {
              kLog.warn("showing received files failed: {}", error->what());
            }
          });
    } else if (!net::openInBrowser(fileUri(result.files.front()), activationToken)) {
      kLog.warn("opening a received file failed");
    }
    return;
  }
  if (result.link && (action == "default" || action == "open")) {
    if (!net::openInBrowser(result.text, activationToken)) {
      kLog.warn("opening a received link failed");
    }
  } else if (!result.link && (action == "default" || action == "copy")) {
    (void)m_clipboard.copyText(result.text);
  }
}

void QuickShareService::call(const std::string& method, std::uint64_t id) {
  m_proxy->callMethodAsync(method).onInterface(kInterface).withArguments(id).uponReplyInvoke(
      [method](std::optional<sdbus::Error> error) {
        if (error.has_value()) {
          kLog.warn("{} failed: {}", method, error->what());
        }
      }
  );
}

void QuickShareService::registerIpc(IpcService& ipc) {
  ipc.bind(noctalia::cli::msg::quickShareVisible, [this](const std::string& args) -> std::string {
    if (!m_available) {
      return "error: umbriel-linkd is not running\n";
    }
    const std::string arg = StringUtils::trim(args);
    if (arg == "on" || arg == "off" || arg == "toggle") {
      const bool next = arg == "toggle" ? !m_visible : arg == "on";
      setVisible(next);
      return next ? "on\n" : "off\n";
    }
    if (!arg.empty()) {
      return "error: on, off, or toggle\n";
    }
    return m_visible ? "on\n" : "off\n";
  });
}
