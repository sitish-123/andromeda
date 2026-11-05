#include "dbus/locale/locale_service.h"

#include "core/log.h"
#include "dbus/system_bus.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <utility>

namespace {

  constexpr Logger kLog("locale1");
  const sdbus::ServiceName kBusName{"org.freedesktop.locale1"};
  const sdbus::ObjectPath kObjectPath{"/org/freedesktop/locale1"};
  constexpr auto kInterface = "org.freedesktop.locale1";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";

  template <typename T> T getPropertyOr(sdbus::IProxy& proxy, std::string_view name, T fallback) {
    try {
      return proxy.getProperty(name).onInterface(kInterface).get<T>();
    } catch (const sdbus::Error&) {
      return fallback;
    }
  }

} // namespace

LocaleService::LocaleService(SystemBus& bus, ChangeCallback onChange) : m_bus(bus), m_onChange(std::move(onChange)) {
  try {
    m_proxy = sdbus::createProxy(m_bus.connection(), kBusName, kObjectPath);
    m_proxy->uponSignal("PropertiesChanged")
        .onInterface(kPropertiesInterface)
        .call([this](
                  const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& /*changed*/,
                  const std::vector<std::string>& /*invalidated*/
              ) {
          if (interfaceName == kInterface) {
            refreshProperties();
            if (m_onChange) {
              m_onChange();
            }
          }
        });
    refreshProperties();
    m_ready = true;
  } catch (const sdbus::Error& e) {
    kLog.warn("locale1 unavailable: {}", e.what());
  }
}

LocaleService::~LocaleService() = default;

void LocaleService::refreshProperties() {
  if (m_proxy == nullptr) {
    return;
  }
  m_locale = getPropertyOr<std::vector<std::string>>(*m_proxy, "Locale", {});
  m_x11Layout = getPropertyOr<std::string>(*m_proxy, "X11Layout", {});
  m_x11Model = getPropertyOr<std::string>(*m_proxy, "X11Model", {});
  m_x11Variant = getPropertyOr<std::string>(*m_proxy, "X11Variant", {});
  m_x11Options = getPropertyOr<std::string>(*m_proxy, "X11Options", {});
}

void LocaleService::setLocale(const std::vector<std::string>& assignments) {
  if (m_proxy == nullptr) {
    return;
  }
  m_lastError.clear();
  m_proxy->callMethodAsync("SetLocale")
      .onInterface(kInterface)
      .withArguments(assignments, true)
      .uponReplyInvoke([this](std::optional<sdbus::Error> error) {
        if (error.has_value()) {
          m_lastError = std::string(error->what());
          kLog.warn("SetLocale failed: {}", error->what());
        } else {
          refreshProperties();
        }
        if (m_onChange) {
          m_onChange();
        }
      });
}

std::vector<std::string> LocaleService::supportedLocales() {
  std::vector<std::string> locales;
  std::ifstream in("/usr/share/i18n/SUPPORTED");
  std::string line;
  while (std::getline(in, line)) {
    const std::size_t space = line.find(' ');
    std::string name = space == std::string::npos ? line : line.substr(0, space);
    if (!name.empty() && name.front() != '#') {
      locales.push_back(std::move(name));
    }
  }
  std::ranges::sort(locales);
  locales.erase(std::unique(locales.begin(), locales.end()), locales.end());
  return locales;
}

void LocaleService::setX11Keyboard(
    const std::string& layout, const std::string& model, const std::string& variant, const std::string& options,
    bool convert
) {
  if (m_proxy == nullptr) {
    return;
  }
  m_lastError.clear();
  m_proxy->callMethodAsync("SetX11Keyboard")
      .onInterface(kInterface)
      .withArguments(layout, model, variant, options, convert, true)
      .uponReplyInvoke([this](std::optional<sdbus::Error> error) {
        if (error.has_value()) {
          m_lastError = std::string(error->what());
          kLog.warn("SetX11Keyboard failed: {}", error->what());
        } else {
          refreshProperties();
        }
        if (m_onChange) {
          m_onChange();
        }
      });
}
