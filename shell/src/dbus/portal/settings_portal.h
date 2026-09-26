#pragma once

#include <cstdint>
#include <memory>

class SessionBus;

namespace sdbus {
  class IObject;
}

// Serves org.freedesktop.impl.portal.Settings under org.freedesktop.impl.portal.desktop.noctalia, so apps follow
// the shell's dark mode through xdg-desktop-portal (org.freedesktop.appearance color-scheme).
class SettingsPortal {
public:
  explicit SettingsPortal(SessionBus& bus);
  ~SettingsPortal();

  SettingsPortal(const SettingsPortal&) = delete;
  SettingsPortal& operator=(const SettingsPortal&) = delete;

  // Emits SettingChanged only when the value changes.
  void setDark(bool dark);

private:
  SessionBus& m_bus;
  std::unique_ptr<sdbus::IObject> m_object;
  // org.freedesktop.appearance color-scheme: 0 no preference, 1 prefer dark, 2 prefer light.
  std::uint32_t m_colorScheme = 0;
};
