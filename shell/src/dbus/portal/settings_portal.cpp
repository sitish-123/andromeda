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
  constexpr auto kAccentColor = "accent-color";
  constexpr auto kContrast = "contrast";
  constexpr auto kReducedMotion = "reduced-motion";
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
                  all[kAppearance][kAccentColor] = accentVariant();
                  all[kAppearance][kContrast] = sdbus::Variant{m_contrast};
                  all[kAppearance][kReducedMotion] = sdbus::Variant{m_reducedMotion};
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
                if (ns == kAppearance && key == kAccentColor) {
                  return accentVariant();
                }
                if (ns == kAppearance && key == kContrast) {
                  return sdbus::Variant{m_contrast};
                }
                if (ns == kAppearance && key == kReducedMotion) {
                  return sdbus::Variant{m_reducedMotion};
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
  emitChanged(kColorScheme, sdbus::Variant{scheme});
}

void SettingsPortal::setAccent(double r, double g, double b) {
  const std::array<double, 3> accent{r, g, b};
  if (accent == m_accent) {
    return;
  }
  m_accent = accent;
  emitChanged(kAccentColor, accentVariant());
}

void SettingsPortal::setHighContrast(bool high) {
  const std::uint32_t contrast = high ? 1 : 0;
  if (contrast == m_contrast) {
    return;
  }
  m_contrast = contrast;
  emitChanged(kContrast, sdbus::Variant{contrast});
}

void SettingsPortal::setReducedMotion(bool reduce) {
  const std::uint32_t reduced = reduce ? 1 : 0;
  if (reduced == m_reducedMotion) {
    return;
  }
  m_reducedMotion = reduced;
  emitChanged(kReducedMotion, sdbus::Variant{reduced});
}

sdbus::Variant SettingsPortal::accentVariant() const {
  return sdbus::Variant{sdbus::Struct<double, double, double>{m_accent[0], m_accent[1], m_accent[2]}};
}

void SettingsPortal::emitChanged(const char* key, const sdbus::Variant& value) {
  m_object->emitSignal("SettingChanged")
      .onInterface(kInterface)
      .withArguments(std::string(kAppearance), std::string(key), value);
}
