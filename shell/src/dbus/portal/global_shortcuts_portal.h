#pragma once

#include <map>
#include <memory>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <vector>

class SessionBus;

// Serves org.freedesktop.impl.portal.GlobalShortcuts under org.freedesktop.impl.portal.desktop.noctalia. Apps
// register shortcut ids; nothing grabs a key on an app's behalf. A key reaches an app only through a compositor
// keybind the user writes, `shell:global-shortcut <app-id> <shortcut-id>`, which calls activate().
class GlobalShortcutsPortal {
public:
  explicit GlobalShortcutsPortal(SessionBus& bus);
  ~GlobalShortcutsPortal();

  GlobalShortcutsPortal(const GlobalShortcutsPortal&) = delete;
  GlobalShortcutsPortal& operator=(const GlobalShortcutsPortal&) = delete;

  // Emits Activated then Deactivated to every session of appId that bound shortcutId. False when none did.
  bool activate(const std::string& appId, const std::string& shortcutId);
  // One "app-id shortcut-id description" line per bound shortcut.
  [[nodiscard]] std::string list() const;

private:
  using Shortcuts = std::vector<sdbus::Struct<std::string, std::map<std::string, sdbus::Variant>>>;

  struct Session {
    std::string appId;
    std::unique_ptr<sdbus::IObject> object;
    std::map<std::string, std::string> descriptions;
  };

  [[nodiscard]] Shortcuts describe(const Session& session) const;
  void close(const std::string& sessionHandle);

  SessionBus& m_bus;
  std::unique_ptr<sdbus::IObject> m_object;
  std::map<std::string, Session> m_sessions;
};
