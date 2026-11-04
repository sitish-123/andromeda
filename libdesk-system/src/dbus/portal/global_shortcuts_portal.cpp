#include "dbus/portal/global_shortcuts_portal.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "dbus/session_bus.h"

#include <chrono>
#include <tuple>
#include <utility>

namespace {

  constexpr Logger kLog("global-shortcuts-portal");

  const sdbus::ObjectPath kObjectPath{"/org/freedesktop/portal/desktop"};
  constexpr auto kInterface = "org.freedesktop.impl.portal.GlobalShortcuts";
  constexpr auto kSessionInterface = "org.freedesktop.impl.portal.Session";
  constexpr std::uint32_t kVersion = 1;

  using Results = std::map<std::string, sdbus::Variant>;

  std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count()
    );
  }

} // namespace

GlobalShortcutsPortal::GlobalShortcutsPortal(SessionBus& bus) : m_bus(bus) {
  m_object = sdbus::createObject(m_bus.connection(), kObjectPath);
  m_object
      ->addVTable(
          sdbus::registerMethod("CreateSession")
              .withInputParamNames("handle", "session_handle", "app_id", "options")
              .withOutputParamNames("response", "results")
              .implementedAs([this](
                                 const sdbus::ObjectPath& /*handle*/, const sdbus::ObjectPath& sessionHandle,
                                 const std::string& appId, const Results& /*options*/
                             ) {
                Session session;
                session.appId = appId;
                session.object = sdbus::createObject(m_bus.connection(), sessionHandle);
                const std::string& path = sessionHandle;
                session.object
                    ->addVTable(
                        sdbus::registerMethod("Close").implementedAs([this, path]() { close(path); }),
                        sdbus::registerSignal("Closed"),
                        sdbus::registerProperty("version").withGetter([]() { return 1U; })
                    )
                    .forInterface(kSessionInterface);
                m_sessions.insert_or_assign(path, std::move(session));
                return std::make_tuple(std::uint32_t{0}, Results{});
              }),
          sdbus::registerMethod("BindShortcuts")
              .withInputParamNames("handle", "session_handle", "shortcuts", "parent_window", "options")
              .withOutputParamNames("response", "results")
              .implementedAs([this](
                                 const sdbus::ObjectPath& /*handle*/, const sdbus::ObjectPath& sessionHandle,
                                 const Shortcuts& shortcuts, const std::string& /*parentWindow*/,
                                 const Results& /*options*/
                             ) {
                const auto it = m_sessions.find(sessionHandle);
                if (it == m_sessions.end()) {
                  return std::make_tuple(std::uint32_t{2}, Results{});
                }
                for (const auto& shortcut : shortcuts) {
                  const auto& [id, properties] = shortcut;
                  const auto description = properties.find("description");
                  it->second.descriptions[id] =
                      description != properties.end() ? description->second.get<std::string>() : std::string{};
                }
                kLog.info("{} bound {} shortcuts", it->second.appId, shortcuts.size());
                return std::make_tuple(std::uint32_t{0}, Results{{"shortcuts", sdbus::Variant{describe(it->second)}}});
              }),
          sdbus::registerMethod("ListShortcuts")
              .withInputParamNames("handle", "session_handle")
              .withOutputParamNames("response", "results")
              .implementedAs([this](const sdbus::ObjectPath& /*handle*/, const sdbus::ObjectPath& sessionHandle) {
                const auto it = m_sessions.find(sessionHandle);
                if (it == m_sessions.end()) {
                  return std::make_tuple(std::uint32_t{2}, Results{});
                }
                return std::make_tuple(std::uint32_t{0}, Results{{"shortcuts", sdbus::Variant{describe(it->second)}}});
              }),
          sdbus::registerSignal("Activated")
              .withParameters<sdbus::ObjectPath, std::string, std::uint64_t, Results>(
                  "session_handle", "shortcut_id", "timestamp", "options"
              ),
          sdbus::registerSignal("Deactivated")
              .withParameters<sdbus::ObjectPath, std::string, std::uint64_t, Results>(
                  "session_handle", "shortcut_id", "timestamp", "options"
              ),
          sdbus::registerSignal("ShortcutsChanged")
              .withParameters<sdbus::ObjectPath, Shortcuts>("session_handle", "shortcuts"),
          sdbus::registerProperty("version").withGetter([]() { return kVersion; })
      )
      .forInterface(kInterface);
  kLog.info("serving {}", kInterface);
}

GlobalShortcutsPortal::~GlobalShortcutsPortal() = default;

GlobalShortcutsPortal::Shortcuts GlobalShortcutsPortal::describe(const Session& session) const {
  Shortcuts shortcuts;
  for (const auto& [id, description] : session.descriptions) {
    shortcuts.emplace_back(
        id,
        Results{
            {"description", sdbus::Variant{description}},
            {"trigger_description", sdbus::Variant{"shell:global-shortcut " + session.appId + " " + id}},
        }
    );
  }
  return shortcuts;
}

bool GlobalShortcutsPortal::activate(const std::string& appId, const std::string& shortcutId) {
  bool delivered = false;
  for (const auto& [path, session] : m_sessions) {
    if (session.appId != appId || !session.descriptions.contains(shortcutId)) {
      continue;
    }
    const sdbus::ObjectPath handle{path};
    const std::uint64_t timestamp = nowMs();
    m_object->emitSignal("Activated").onInterface(kInterface).withArguments(handle, shortcutId, timestamp, Results{});
    m_object->emitSignal("Deactivated").onInterface(kInterface).withArguments(handle, shortcutId, timestamp, Results{});
    delivered = true;
  }
  return delivered;
}

std::string GlobalShortcutsPortal::list() const {
  std::string out;
  for (const auto& [path, session] : m_sessions) {
    for (const auto& [id, description] : session.descriptions) {
      out += session.appId + " " + id + " " + description + "\n";
    }
  }
  return out;
}

void GlobalShortcutsPortal::close(const std::string& sessionHandle) {
  // The session object is running this Close call; destroy it once the call has returned.
  DeferredCall::callLater([this, sessionHandle]() { m_sessions.erase(sessionHandle); });
}
