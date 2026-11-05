#pragma once

#include <array>
#include <cstdint>
#include <memory>

class SessionBus;

namespace sdbus {
  class IObject;
  class Variant;
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
  // org.freedesktop.appearance accent-color (sRGB 0..1), contrast (high = 1), reduced-motion (reduce = 1).
  void setAccent(double r, double g, double b);
  void setHighContrast(bool high);
  void setReducedMotion(bool reduce);

private:
  SessionBus& m_bus;
  std::unique_ptr<sdbus::IObject> m_object;
  // org.freedesktop.appearance color-scheme: 0 no preference, 1 prefer dark, 2 prefer light.
  std::uint32_t m_colorScheme = 0;
  // Out of range (-1) until the shell sets an accent: the spec's "no preference".
  std::array<double, 3> m_accent{-1.0, -1.0, -1.0};
  std::uint32_t m_contrast = 0;
  std::uint32_t m_reducedMotion = 0;

  void emitChanged(const char* key, const sdbus::Variant& value);
  [[nodiscard]] sdbus::Variant accentVariant() const;
};
