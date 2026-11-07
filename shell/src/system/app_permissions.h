#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

class SessionBus;

namespace sdbus {
  class IProxy;
}

// What apps may use through xdg-desktop-portal, from its permission store (org.freedesktop.impl.portal.PermissionStore).
// Host apps (not Flatpak) share the empty app id. Async; replies run on the main loop.
class AppPermissions {
public:
  struct Kind {
    // Store table; `id` empty = every entry of the table is one saved grant (screencast restore tokens).
    std::string table;
    std::string id;
  };
  struct Grant {
    std::string app;
    bool allowed = false;
    // Store entries that hold this app's grant, and the stored values of each (same order).
    std::vector<std::string> ids;
    std::vector<std::vector<std::string>> values;
  };
  using Grants = std::vector<Grant>;

  explicit AppPermissions(SessionBus& bus);
  ~AppPermissions();

  // Camera, Location, Screenshots, Screen Sharing, Remote Control.
  [[nodiscard]] static const std::vector<Kind>& kinds();

  void load(const Kind& kind, std::function<void(Grants)> done);
  // A single-entry kind stores the answer for the app ("yes"/"no"; location "EXACT"/"NONE"); a token kind can only
  // be revoked, which deletes the app's saved tokens.
  void setAllowed(const Kind& kind, const Grant& grant, bool allowed, std::function<void(bool ok)> done);

private:
  std::unique_ptr<sdbus::IProxy> m_store;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
