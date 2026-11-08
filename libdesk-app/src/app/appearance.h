#pragma once

#include <functional>
#include <memory>

class SessionBus;
namespace sdbus {
  class IProxy;
}

// Follows the desktop's light/dark choice and accent for an app process: org.freedesktop.appearance color-scheme and
// accent-color from the Settings portal (the public frontend, else the Andromeda shell backend directly). Applies them
// to the libdesk-ui palette; `changed` runs after each change so windows repaint.
class Appearance {
public:
  Appearance(SessionBus& bus, std::function<void()> changed);
  ~Appearance();
  Appearance(const Appearance&) = delete;
  Appearance& operator=(const Appearance&) = delete;

private:
  void read(bool backend);
  void apply();

  SessionBus& m_bus;
  std::function<void()> m_changed;
  std::unique_ptr<sdbus::IProxy> m_portal;
  std::unique_ptr<sdbus::IProxy> m_backend;
  bool m_light = false;
  bool m_hasAccent = false;
  double m_accent[3] = {0.0, 0.0, 0.0};
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
