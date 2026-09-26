#include "dbus/portal/settings_portal.h"

#include "core/log.h"
#include "dbus/session_bus.h"

#include <map>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <vector>

namespace {

  constexpr Logger kLog("settings-portal");

  const sdbus::ServiceName kBusName{"org.freedesktop.impl.portal.desktop.noctalia"};
  const sdbus::ObjectPath kObjectPath{"/org/freedesktop/portal/desktop"};
  constexpr auto kInterface = "org.freedesktop.impl.portal.Settings";
  constexpr auto kAppearance = "org.freedesktop.appearance";
  constexpr auto kColorScheme = "color-scheme";
  constexpr std::uint32_t kVersion = 2;

  // A namespace pattern matches exactly, or as a prefix when it ends in '*'; no patterns means every namespace.
  bool wanted(const std::vector<std::string>& patterns, const std::string& ns) {
    if (patterns.empty()) {
      return true;
    }
    for (const std::string& pattern : patterns) {
      if (pattern == ns || (pattern.ends_with('*') && ns.starts_with(pattern.substr(0, pattern.size() - 1)))) {
        return true;
      }
    }
    return false;
  }

} // namespace

SettingsPortal::SettingsPortal(SessionBus& bus) : m_bus(bus) {
  m_object = sdbus::createObject(m_bus.connection(), kObjectPath);
  m_object
      ->addVTable(
          sdbus::registerMethod("ReadAll")
              .withInputParamNames("namespaces")
              .withOutputParamNames("value")
              .implementedAs([this](const std::vector<std::string>& namespaces) {
                std::map<std::string, std::map<std::string, sdbus::Variant>> all;
                if (wanted(namespaces, kAppearance)) {
                  all[kAppearance][kColorScheme] = sdbus::Variant{m_colorScheme};
                }
                return all;
              }),
          sdbus::registerMethod("Read")
              .withInputParamNames("namespace", "key")
              .withOutputParamNames("value")
              .implementedAs([this](const std::string& ns, const std::string& key) {
                if (ns == kAppearance && key == kColorScheme) {
                  return sdbus::Variant{m_colorScheme};
                }
                throw sdbus::Error(
                    sdbus::Error::Name{"org.freedesktop.portal.Error.NotFound"}, "Requested setting not found"
                );
              }),
          sdbus::registerSignal("SettingChanged")
              .withParameters<std::string, std::string, sdbus::Variant>("namespace", "key", "value"),
          sdbus::registerProperty("version").withGetter([]() { return kVersion; })
      )
      .forInterface(kInterface);
  m_bus.connection().requestName(kBusName);
  kLog.info("serving {} as {}", kInterface, static_cast<const std::string&>(kBusName));
}

SettingsPortal::~SettingsPortal() {
  try {
    m_bus.connection().releaseName(kBusName);
  } catch (const sdbus::Error& e) {
    kLog.debug("release {} failed: {}", static_cast<const std::string&>(kBusName), e.what());
  }
}

void SettingsPortal::setDark(bool dark) {
  const std::uint32_t scheme = dark ? 1 : 2;
  if (scheme == m_colorScheme) {
    return;
  }
  m_colorScheme = scheme;
  m_object->emitSignal("SettingChanged")
      .onInterface(kInterface)
      .withArguments(std::string(kAppearance), std::string(kColorScheme), sdbus::Variant{scheme});
}
