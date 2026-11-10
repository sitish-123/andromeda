#include "app/appearance.h"

#include "core/log.h"
#include "dbus/session_bus.h"
#include "ui/palette.h"

#include <sdbus-c++/sdbus-c++.h>

namespace {
  constexpr Logger kLog("appearance");
  const sdbus::ServiceName kPortal{"org.freedesktop.portal.Desktop"};
  const sdbus::ServiceName kBackend{"org.freedesktop.impl.portal.desktop.andromeda.shell"};
  const sdbus::ObjectPath kPath{"/org/freedesktop/portal/desktop"};
  constexpr auto kPortalSettings = "org.freedesktop.portal.Settings";
  constexpr auto kBackendSettings = "org.freedesktop.impl.portal.Settings";
  constexpr auto kNamespace = "org.freedesktop.appearance";
  constexpr std::uint32_t kPreferLight = 2; // color-scheme: 0 no preference, 1 dark, 2 light

  // The frontend's Read wraps the value in one more variant; ReadOne and the backend do not.
  sdbus::Variant unwrap(sdbus::Variant v) {
    while (v.containsValueOfType<sdbus::Variant>()) {
      v = v.get<sdbus::Variant>();
    }
    return v;
  }
} // namespace

Appearance::Appearance(SessionBus& bus, std::function<void()> changed) : m_bus(bus), m_changed(std::move(changed)) {
  const auto onChanged = [this](const std::string& ns, const std::string& key, const sdbus::Variant& raw) {
    if (ns != kNamespace) {
      return;
    }
    const sdbus::Variant value = unwrap(raw);
    if (key == "color-scheme" && value.containsValueOfType<std::uint32_t>()) {
      m_light = value.get<std::uint32_t>() == kPreferLight;
    } else if (key == "accent-color" && value.containsValueOfType<sdbus::Struct<double, double, double>>()) {
      const auto [r, g, b] = value.get<sdbus::Struct<double, double, double>>();
      m_hasAccent = r >= 0.0 && r <= 1.0 && g >= 0.0 && g <= 1.0 && b >= 0.0 && b <= 1.0;
      m_accent[0] = r;
      m_accent[1] = g;
      m_accent[2] = b;
    } else {
      return;
    }
    apply();
  };
  try {
    m_portal = sdbus::createProxy(m_bus.connection(), kPortal, kPath);
    m_portal->uponSignal("SettingChanged").onInterface(kPortalSettings).call(onChanged);
    m_backend = sdbus::createProxy(m_bus.connection(), kBackend, kPath);
    m_backend->uponSignal("SettingChanged").onInterface(kBackendSettings).call(onChanged);
  } catch (const sdbus::Error& e) {
    kLog.warn("settings portal: {}", e.getMessage());
    return;
  }
  read(false);
}

Appearance::~Appearance() = default;

void Appearance::read(bool backend) {
  sdbus::IProxy* proxy = backend ? m_backend.get() : m_portal.get();
  for (const char* key : {"color-scheme", "accent-color"}) {
    proxy->callMethodAsync("Read")
        .onInterface(backend ? kBackendSettings : kPortalSettings)
        .withArguments(std::string(kNamespace), std::string(key))
        .uponReplyInvoke([this, alive = std::weak_ptr<bool>(m_alive), backend, key = std::string(key)](
                             std::optional<sdbus::Error> error, sdbus::Variant raw) {
          if (alive.expired()) {
            return;
          }
          if (error.has_value()) {
            // No portal frontend (a bare session, the E2E bus): ask the shell's backend once.
            if (!backend && key == "color-scheme" && error->getName() != "org.freedesktop.portal.Error.NotFound") {
              read(true);
            }
            return;
          }
          const sdbus::Variant value = unwrap(std::move(raw));
          if (key == "color-scheme" && value.containsValueOfType<std::uint32_t>()) {
            m_light = value.get<std::uint32_t>() == kPreferLight;
          } else if (key == "accent-color" && value.containsValueOfType<sdbus::Struct<double, double, double>>()) {
            const auto [r, g, b] = value.get<sdbus::Struct<double, double, double>>();
            m_hasAccent = r >= 0.0 && r <= 1.0 && g >= 0.0 && g <= 1.0 && b >= 0.0 && b <= 1.0;
            m_accent[0] = r;
            m_accent[1] = g;
            m_accent[2] = b;
          }
          apply();
        });
  }
}

void Appearance::apply() {
  Palette palette = tokenPalette(m_light);
  if (m_hasAccent) {
    palette.accent = rgba(static_cast<float>(m_accent[0]), static_cast<float>(m_accent[1]),
                          static_cast<float>(m_accent[2]), 1.0F);
  }
  setResolvedThemeLight(m_light);
  setPalette(palette);
  kLog.debug("scheme {}{}", m_light ? "light" : "dark", m_hasAccent ? " accent" : "");
  if (m_changed) {
    m_changed();
  }
}
