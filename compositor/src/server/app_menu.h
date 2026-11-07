#pragma once

#include "wlr.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace andromeda {

  // Where a window's menus live on the session bus, for the macOS-layout top bar (board desktop-menus.html A5).
  struct AppMenuAddress {
    enum class Kind : uint8_t { DbusMenu, Gtk };
    Kind kind = Kind::DbusMenu;
    // DbusMenu: com.canonical.dbusmenu at service + path (org_kde_kwin_appmenu, Qt/KDE apps).
    // Gtk: org.gtk.Menus at service + path (the menubar), org.gtk.Actions at the app and window paths.
    std::string service;
    std::string path;
    std::string applicationPath;
    std::string windowPath;
    bool operator==(const AppMenuAddress&) const = default;
  };

  // Serves org_kde_kwin_appmenu_manager and gtk_shell1 (version 1: dbus properties only) and keeps each surface's
  // menu address. A changed address calls the change callback.
  class AppMenuRegistry {
  public:
    AppMenuRegistry(wl_display* display, std::function<void()> onChange);
    ~AppMenuRegistry();
    AppMenuRegistry(const AppMenuRegistry&) = delete;
    AppMenuRegistry& operator=(const AppMenuRegistry&) = delete;

    [[nodiscard]] std::optional<AppMenuAddress> addressFor(const wlr_surface* surface) const;

    struct Entry;

  private:
    friend struct AppMenuCallbacks;
    Entry& entryFor(wlr_surface* surface);
    void destroy(Entry* entry);
    void changed();

    wl_global* m_kdeGlobal = nullptr;
    wl_global* m_gtkGlobal = nullptr;
    std::function<void()> m_onChange;
    std::vector<std::unique_ptr<Entry>> m_entries;
  };

} // namespace andromeda
