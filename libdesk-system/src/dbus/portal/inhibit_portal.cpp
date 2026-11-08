#include "dbus/portal/inhibit_portal.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "dbus/session_bus.h"
#include "dbus/system_bus.h"

#include <utility>

namespace {

  constexpr Logger kLog("inhibit-portal");

  const sdbus::ObjectPath kObjectPath{"/org/freedesktop/portal/desktop"};
  constexpr auto kInterface = "org.freedesktop.impl.portal.Inhibit";
  constexpr auto kRequestInterface = "org.freedesktop.impl.portal.Request";
  constexpr auto kSessionInterface = "org.freedesktop.impl.portal.Session";
  constexpr std::uint32_t kVersion = 3;
  constexpr std::uint32_t kFlagLogout = 1;
  constexpr std::uint32_t kFlagSuspend = 4;
  constexpr std::uint32_t kFlagIdle = 8;

} // namespace

InhibitPortal::InhibitPortal(SessionBus& bus, SystemBus* systemBus) : m_bus(bus) {
  if (systemBus != nullptr) {
    m_logind = sdbus::createProxy(
        systemBus->connection(), sdbus::ServiceName{"org.freedesktop.login1"},
        sdbus::ObjectPath{"/org/freedesktop/login1"}
    );
  }
  m_object = sdbus::createObject(m_bus.connection(), kObjectPath);
  m_object
      ->addVTable(
          sdbus::registerMethod("Inhibit")
              .withInputParamNames("handle", "app_id", "window", "flags", "options")
              .implementedAs([this](
                                 const sdbus::ObjectPath& handle, const std::string& appId,
                                 const std::string& /*window*/, std::uint32_t flags,
                                 const std::map<std::string, sdbus::Variant>& options
                             ) {
                std::string reason;
                if (const auto it = options.find("reason"); it != options.end()) {
                  try {
                    reason = it->second.get<std::string>();
                  } catch (const sdbus::Error&) {
                  }
                }
                inhibit(handle, appId, flags, reason);
              }),
          sdbus::registerMethod("CreateMonitor")
              .withInputParamNames("handle", "session_handle", "app_id", "window")
              .withOutputParamNames("response")
              .implementedAs([this](
                                 const sdbus::ObjectPath& /*handle*/, const sdbus::ObjectPath& sessionHandle,
                                 const std::string& /*appId*/, const std::string& /*window*/
                             ) {
                createMonitor(sessionHandle);
                return std::uint32_t{0};
              }),
          sdbus::registerMethod("QueryEndSessionResponse")
              .withInputParamNames("session_handle")
              .implementedAs([](const sdbus::ObjectPath& /*sessionHandle*/) {}),
          sdbus::registerSignal("StateChanged")
              .withParameters<sdbus::ObjectPath, std::map<std::string, sdbus::Variant>>("session_handle", "state"),
          sdbus::registerProperty("version").withGetter([]() { return kVersion; })
      )
      .forInterface(kInterface);
  kLog.info("serving {}", kInterface);
}

InhibitPortal::~InhibitPortal() = default;

void InhibitPortal::inhibit(
    const std::string& handle, const std::string& appId, std::uint32_t flags, const std::string& reason
) {
  Handle entry;
  entry.appId = appId;
  entry.reason = reason;
  entry.flags = flags;
  entry.object = sdbus::createObject(m_bus.connection(), sdbus::ObjectPath{handle});
  entry.object->addVTable(
                  sdbus::registerMethod("Close").implementedAs([this, handle]() { close(handle); })
  ).forInterface(kRequestInterface);

  const std::string who = appId.empty() ? std::string("an app") : appId;
  for (const auto& [flag, what] : {std::pair{kFlagIdle, "idle"}, std::pair{kFlagSuspend, "sleep"}}) {
    if ((flags & flag) == 0 || m_logind == nullptr) {
      continue;
    }
    try {
      sdbus::UnixFd fd;
      m_logind->callMethod("Inhibit")
          .onInterface("org.freedesktop.login1.Manager")
          .withArguments(std::string(what), who, std::string("Inhibited through the portal"), std::string("block"))
          .storeResultsTo(fd);
      entry.inhibitors.push_back(std::move(fd));
    } catch (const sdbus::Error& e) {
      kLog.warn("logind {} inhibit for {} failed: {}", what, who, e.what());
    }
  }
  kLog.info("{} inhibits flags {} ({} logind locks)", who, flags, entry.inhibitors.size());
  m_handles.insert_or_assign(handle, std::move(entry));
  if ((flags & kFlagLogout) != 0 && m_changeCallback) {
    m_changeCallback();
  }
}

std::vector<InhibitPortal::LogoutInhibitor> InhibitPortal::logoutInhibitors() const {
  std::vector<LogoutInhibitor> out;
  for (const auto& [handle, entry] : m_handles) {
    if (!entry.monitor && (entry.flags & kFlagLogout) != 0) {
      out.push_back({.appId = entry.appId, .reason = entry.reason});
    }
  }
  return out;
}

void InhibitPortal::setSessionState(SessionState state) {
  if (state == m_state) {
    return;
  }
  m_state = state;
  for (const auto& [handle, entry] : m_handles) {
    if (entry.monitor) {
      emitState(handle);
    }
  }
}

void InhibitPortal::emitState(const std::string& sessionHandle) {
  const std::map<std::string, sdbus::Variant> state{
      {"screensaver-active", sdbus::Variant{false}},
      {"session-state", sdbus::Variant{static_cast<std::uint32_t>(m_state)}}
  };
  m_object->emitSignal("StateChanged").onInterface(kInterface).withArguments(sdbus::ObjectPath{sessionHandle}, state);
}

void InhibitPortal::createMonitor(const std::string& sessionHandle) {
  Handle entry;
  entry.object = sdbus::createObject(m_bus.connection(), sdbus::ObjectPath{sessionHandle});
  entry.object
      ->addVTable(
          sdbus::registerMethod("Close").implementedAs([this, sessionHandle]() { close(sessionHandle); }),
          sdbus::registerSignal("Closed"), sdbus::registerProperty("version").withGetter([]() { return 1U; })
      )
      .forInterface(kSessionInterface);
  entry.monitor = true;
  m_handles.insert_or_assign(sessionHandle, std::move(entry));

  // xdg-desktop-portal registers the session only once CreateMonitor has replied.
  DeferredCall::callLater([this, sessionHandle]() {
    if (m_handles.contains(sessionHandle)) {
      emitState(sessionHandle);
    }
  });
}

void InhibitPortal::close(const std::string& handle) {
  const auto it = m_handles.find(handle);
  if (it == m_handles.end()) {
    return;
  }
  it->second.inhibitors.clear();
  const bool logout = !it->second.monitor && (it->second.flags & kFlagLogout) != 0;
  // The object is running this Close call; destroy it once the call has returned.
  DeferredCall::callLater([this, handle, logout]() {
    m_handles.erase(handle);
    if (logout && m_changeCallback) {
      m_changeCallback();
    }
  });
}
