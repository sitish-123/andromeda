#include "app/single_instance.h"

#include "core/log.h"
#include "dbus/session_bus.h"

#include <cstdlib>

namespace {
  constexpr Logger kLog("app-instance");
  constexpr auto kInterface = "org.freedesktop.Application";
  constexpr std::uint64_t kForwardTimeoutUs = 5'000'000;

  using PlatformData = std::map<std::string, sdbus::Variant>;

  std::string tokenFrom(const PlatformData& data) {
    for (const char* key : {"activation-token", "desktop-startup-id"}) {
      if (const auto it = data.find(key); it != data.end()) {
        try {
          return it->second.get<std::string>();
        } catch (const sdbus::Error&) {
        }
      }
    }
    return {};
  }

  PlatformData launchPlatformData() {
    PlatformData data;
    if (const char* token = std::getenv("XDG_ACTIVATION_TOKEN"); token != nullptr && token[0] != '\0') {
      data["activation-token"] = sdbus::Variant{std::string(token)};
    }
    return data;
  }
} // namespace

SingleInstance::SingleInstance(SessionBus* bus, std::string appId) : m_bus(bus), m_appId(std::move(appId)) {}

SingleInstance::~SingleInstance() {
  m_object.reset();
  if (m_bus != nullptr) {
    try {
      m_bus->connection().releaseName(sdbus::ServiceName{m_appId});
    } catch (const sdbus::Error&) {
    }
  }
}

std::string SingleInstance::objectPathFor(std::string_view appId) {
  std::string path = "/";
  for (const char c : appId) {
    path += c == '.' ? '/' : c == '-' ? '_' : c;
  }
  return path;
}

SingleInstance::Role SingleInstance::claim(const std::vector<std::string>& uris, Handlers handlers) {
  if (m_bus == nullptr) {
    return Role::Alone;
  }
  m_handlers = std::move(handlers);
  try {
    m_bus->connection().requestName(sdbus::ServiceName{m_appId});
  } catch (const sdbus::Error& e) {
    if (forward(uris)) {
      return Role::Forwarded;
    }
    // The owner holds the name but did not answer: it hangs or quits. Run alone rather than not at all.
    kLog.warn("{} is owned but did not answer ({}); running a second instance", m_appId, e.getMessage());
    return Role::Alone;
  }
  serve();
  return Role::Primary;
}

bool SingleInstance::forward(const std::vector<std::string>& uris) {
  try {
    auto proxy = sdbus::createProxy(
        m_bus->connection(), sdbus::ServiceName{m_appId}, sdbus::ObjectPath{objectPathFor(m_appId)}
    );
    const PlatformData data = launchPlatformData();
    if (uris.empty()) {
      proxy->callMethod("Activate").onInterface(kInterface).withTimeout(kForwardTimeoutUs).withArguments(data);
    } else {
      proxy->callMethod("Open").onInterface(kInterface).withTimeout(kForwardTimeoutUs).withArguments(uris, data);
    }
    kLog.info("forwarded to the running {}", m_appId);
    return true;
  } catch (const sdbus::Error& e) {
    kLog.warn("forward to {} failed: {}", m_appId, e.getMessage());
    return false;
  }
}

void SingleInstance::serve() {
  m_object = sdbus::createObject(m_bus->connection(), sdbus::ObjectPath{objectPathFor(m_appId)});
  m_object
      ->addVTable(
          sdbus::registerMethod("Activate")
              .withInputParamNames("platform_data")
              .implementedAs([this](const PlatformData& data) {
                if (m_handlers.activate) {
                  m_handlers.activate(tokenFrom(data));
                }
              }),
          sdbus::registerMethod("Open")
              .withInputParamNames("uris", "platform_data")
              .implementedAs([this](const std::vector<std::string>& uris, const PlatformData& data) {
                if (m_handlers.open) {
                  m_handlers.open(uris, tokenFrom(data));
                } else if (m_handlers.activate) {
                  m_handlers.activate(tokenFrom(data));
                }
              }),
          sdbus::registerMethod("ActivateAction")
              .withInputParamNames("action_name", "parameter", "platform_data")
              .implementedAs([this](
                                 const std::string& action, const std::vector<sdbus::Variant>& parameter,
                                 const PlatformData& data
                             ) {
                std::vector<std::string> values;
                for (const sdbus::Variant& v : parameter) {
                  if (v.containsValueOfType<std::string>()) {
                    values.push_back(v.get<std::string>());
                  } else if (v.containsValueOfType<std::vector<std::string>>()) {
                    const auto list = v.get<std::vector<std::string>>();
                    values.insert(values.end(), list.begin(), list.end());
                  }
                }
                if (m_handlers.action) {
                  m_handlers.action(action, values, tokenFrom(data));
                }
              })
      )
      .forInterface(kInterface);
}
