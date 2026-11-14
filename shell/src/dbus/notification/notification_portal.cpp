#include "dbus/notification/notification_portal.h"

#include "core/log.h"
#include "dbus/session_bus.h"
#include "notification/notification_manager.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

  constexpr Logger kLog("notification-portal");

  const sdbus::ObjectPath kObjectPath{"/org/freedesktop/portal/desktop"};
  constexpr auto kInterface = "org.freedesktop.impl.portal.Notification";
  constexpr std::uint32_t kVersion = 2;
  constexpr std::size_t kMaxIconBytes = 1 << 20;
  constexpr std::string_view kReplyPurpose = "im.reply-with-text";
  constexpr std::string_view kInlineReplyKey = "inline-reply";

  using Options = NotificationPortal::Options;
  using IconVariant = sdbus::Struct<std::string, sdbus::Variant>; // a serialized GIcon: (kind, data)

  template <typename T> std::optional<T> option(const Options& options, const std::string& key) {
    const auto it = options.find(key);
    if (it == options.end()) {
      return std::nullopt;
    }
    try {
      return it->second.get<T>();
    } catch (const sdbus::Error&) {
      return std::nullopt;
    }
  }

  std::optional<sdbus::Variant> variantOption(const Options& options, const std::string& key) {
    const auto it = options.find(key);
    return it != options.end() ? std::optional(it->second) : std::nullopt;
  }

  // The markup subset (b, i, a) dropped: the island and history render plain text.
  std::string plainFromMarkup(std::string_view markup) {
    std::string out;
    bool inTag = false;
    for (const char c : markup) {
      if (c == '<') {
        inTag = true;
      } else if (c == '>') {
        inTag = false;
      } else if (!inTag) {
        out.push_back(c);
      }
    }
    for (const auto& [entity, text] : {std::pair{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"},
                                       {"&amp;", "&"}}) {
      for (std::size_t at = out.find(entity); at != std::string::npos; at = out.find(entity, at + 1)) {
        out.replace(at, std::string_view(entity).size(), text);
      }
    }
    return out;
  }

  // A sealed memfd from the portal frontend: a regular file, so reading it does not block.
  std::optional<std::vector<char>> readIconFd(int fd) {
    struct stat st{};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0
        || static_cast<std::size_t>(st.st_size) > kMaxIconBytes) {
      return std::nullopt;
    }
    std::vector<char> bytes(static_cast<std::size_t>(st.st_size));
    if (pread(fd, bytes.data(), bytes.size(), 0) != static_cast<ssize_t>(bytes.size())) {
      return std::nullopt;
    }
    return bytes;
  }

  fs::path iconDir() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    return fs::path(runtime != nullptr ? runtime : "/tmp") / "andromeda-shell" / "portal-icons";
  }

  // Icon name or file path for NotificationManager; `file` gets the written file, if any. A bad icon drops only
  // the icon.
  std::string iconFor(const sdbus::Variant& value, std::uint64_t serial, std::string& file) {
    try {
      const auto icon = value.get<IconVariant>();
      const std::string& kind = icon.get<0>();
      if (kind == "themed") {
        const auto names = icon.get<1>().get<std::vector<std::string>>();
        return names.empty() ? std::string() : names.front();
      }
      std::optional<std::vector<char>> bytes;
      if (kind == "bytes") {
        const auto raw = icon.get<1>().get<std::vector<std::uint8_t>>();
        if (raw.size() <= kMaxIconBytes) {
          bytes.emplace(raw.begin(), raw.end());
        }
      } else if (kind == "file-descriptor") {
        bytes = readIconFd(icon.get<1>().get<sdbus::UnixFd>().get());
      }
      if (!bytes.has_value()) {
        kLog.warn("icon kind \"{}\" dropped", kind);
        return {};
      }
      std::error_code ec;
      fs::create_directories(iconDir(), ec);
      const fs::path path = iconDir() / ("icon-" + std::to_string(serial));
      std::ofstream(path, std::ios::binary).write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
      file = path.string();
      return file;
    } catch (const sdbus::Error& e) {
      kLog.warn("icon dropped: {}", e.what());
      return {};
    }
  }

} // namespace

NotificationPortal::NotificationPortal(SessionBus& bus, NotificationManager& manager) : m_bus(bus), m_manager(manager) {
  m_object = sdbus::createObject(m_bus.connection(), kObjectPath);
  m_object
      ->addVTable(
          sdbus::registerMethod("AddNotification")
              .withInputParamNames("app_id", "id", "notification")
              .implementedAs([this](const std::string& appId, const std::string& id, const Options& n) {
                add(appId, id, n, m_object->getCurrentlyProcessedMessage().getSender());
              }),
          sdbus::registerMethod("RemoveNotification")
              .withInputParamNames("app_id", "id")
              .implementedAs([this](const std::string& appId, const std::string& id) { remove(appId, id); }),
          sdbus::registerSignal("ActionInvoked")
              .withParameters<std::string, std::string, std::string, std::vector<sdbus::Variant>>(
                  "app_id", "id", "action", "parameter"
              ),
          sdbus::registerProperty("SupportedOptions").withGetter([] {
            return Options{
                {"category", sdbus::Variant(std::vector<std::string>{
                                 "im.received", "alarm.ringing", "call.incoming", "call.ongoing", "call.unanswered",
                                 "weather.warning.extreme", "cellbroadcast.danger.extreme",
                                 "cellbroadcast.danger.severe", "cellbroadcast.amber-alert", "cellbroadcast.test",
                                 "os.battery.low", "browser.web-notification"
                             })},
                {"button-purpose", sdbus::Variant(std::vector<std::string>{std::string(kReplyPurpose)})},
            };
          }),
          sdbus::registerProperty("version").withGetter([] { return kVersion; })
      )
      .forInterface(kInterface);

  m_manager.setPortalCallbacks(
      [this](std::uint32_t id, const std::string& key, const std::string& token, const std::string& /*sender*/) {
        invoke(id, key, token, std::nullopt);
      },
      [this](std::uint32_t id, const std::string& text, const std::string& token, const std::string& /*sender*/) {
        invoke(id, std::string(kInlineReplyKey), token, text);
      }
  );
  m_manager.addCloseObserver([this, alive = std::weak_ptr<bool>(m_alive)](
                                 std::uint32_t id, CloseReason /*reason*/, const std::string& /*sender*/
                             ) {
    if (alive.expired()) {
      return;
    }
    const auto it = m_entries.find(id);
    if (it == m_entries.end()) {
      return;
    }
    m_byKey.erase({it->second.appId, it->second.id});
    if (!it->second.iconFile.empty()) {
      std::error_code ec;
      fs::remove(it->second.iconFile, ec);
    }
    m_entries.erase(it);
  });
  kLog.info("serving {}", kInterface);
}

NotificationPortal::~NotificationPortal() { m_manager.setPortalCallbacks(nullptr, nullptr); }

void NotificationPortal::add(const std::string& appId, const std::string& id, const Options& n, const std::string& sender) {
  Entry entry{.appId = appId, .id = id, .defaultAction = {}, .buttons = {}, .iconFile = {}};
  entry.defaultAction = {.name = option<std::string>(n, "default-action").value_or(""),
                         .target = variantOption(n, "default-action-target")};
  std::vector<std::string> actions{"default", ""};
  for (const Options& button : option<std::vector<Options>>(n, "buttons").value_or({})) {
    const std::string purpose = option<std::string>(button, "purpose").value_or("");
    const std::string key = purpose == kReplyPurpose ? std::string(kInlineReplyKey)
                                                     : "b" + std::to_string(entry.buttons.size());
    actions.push_back(key);
    actions.push_back(option<std::string>(button, "label").value_or(""));
    entry.buttons[key] = {.name = option<std::string>(button, "action").value_or(""),
                          .target = variantOption(button, "target")};
  }

  const std::string priority = option<std::string>(n, "priority").value_or("normal");
  const auto hints = option<std::vector<std::string>>(n, "display-hint").value_or({});
  const auto hinted = [&hints](std::string_view h) { return std::ranges::contains(hints, h); };
  std::string body = option<std::string>(n, "body").value_or("");
  if (const auto markup = option<std::string>(n, "markup-body"); markup.has_value()) {
    body = plainFromMarkup(*markup);
  }
  std::string icon = appId;
  if (const auto value = variantOption(n, "icon"); value.has_value()) {
    if (std::string named = iconFor(*value, ++m_iconSerial, entry.iconFile); !named.empty()) {
      icon = std::move(named);
    }
  }

  const auto old = m_byKey.find({appId, id});
  NotificationRequest request{
      .replacesId = old != m_byKey.end() ? old->second : 0,
      .appName = appId,
      .summary = option<std::string>(n, "title").value_or(""),
      .body = std::move(body),
      .sender = sender,
      .urgency = priority == "low" ? Urgency::Low : (priority == "urgent" ? Urgency::Critical : Urgency::Normal),
      .timeout = hinted("persistent") ? 0 : kDefaultNotificationTimeout,
      .origin = NotificationOrigin::Portal,
      .transient = hinted("transient"),
      .actions = std::move(actions),
      .icon = icon.empty() ? std::nullopt : std::optional(icon),
      .category = option<std::string>(n, "category"),
      .desktopEntry = appId.empty() ? std::nullopt : std::optional(appId),
      .portalPriority = priority,
  };
  const std::uint32_t managerId = m_manager.addOrReplace(std::move(request));
  if (old != m_byKey.end() && old->second != managerId) {
    m_entries.erase(old->second);
  }
  if (const auto replaced = m_entries.find(managerId); replaced != m_entries.end() && !replaced->second.iconFile.empty()
      && replaced->second.iconFile != entry.iconFile) {
    std::error_code ec;
    fs::remove(replaced->second.iconFile, ec);
  }
  m_byKey[{appId, id}] = managerId;
  m_entries[managerId] = std::move(entry);
  kLog.debug("{} {} shown as #{}", appId, id, managerId);
}

void NotificationPortal::remove(const std::string& appId, const std::string& id) {
  if (const auto it = m_byKey.find({appId, id}); it != m_byKey.end()) {
    (void)m_manager.close(it->second, CloseReason::ClosedByCall);
  }
}

void NotificationPortal::invoke(
    std::uint32_t managerId, const std::string& key, const std::string& token, const std::optional<std::string>& response
) {
  const auto it = m_entries.find(managerId);
  if (it == m_entries.end()) {
    return;
  }
  const Entry& entry = it->second;
  const Action* action = &entry.defaultAction;
  if (key != "default") {
    const auto button = entry.buttons.find(key);
    if (button == entry.buttons.end()) {
      return;
    }
    action = &button->second;
  }
  // parameter: [target?, platform-data, response?] (portal spec v2).
  std::vector<sdbus::Variant> parameter;
  if (action->target.has_value()) {
    parameter.push_back(*action->target);
  }
  Options platform;
  if (!token.empty()) {
    platform.emplace("activation-token", sdbus::Variant(token));
    platform.emplace("desktop-startup-id", sdbus::Variant(token));
  }
  parameter.emplace_back(platform);
  if (response.has_value()) {
    parameter.emplace_back(*response);
  }
  const std::string name = action->name.empty() ? "default" : action->name;
  try {
    m_object->emitSignal("ActionInvoked").onInterface(kInterface).withArguments(entry.appId, entry.id, name, parameter);
  } catch (const sdbus::Error& e) {
    kLog.warn("ActionInvoked {} {}: {}", entry.appId, entry.id, e.what());
  }
}
