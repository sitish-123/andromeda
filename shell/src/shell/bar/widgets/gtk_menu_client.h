#pragma once

#include "shell/bar/widgets/dbus_menu_client.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace sdbus {
  class IConnection;
  class IProxy;
} // namespace sdbus

// A GTK app's menu bar (org.gtk.Menus at the menubar path, actions on org.gtk.Actions at the app and window paths;
// board desktop-menus.html A5). Builds the same tree as DbusMenuClient: sections become separators, an item's ids are
// local to this client, and accelerators come from the item's "accel".
class GtkMenuClient {
public:
  GtkMenuClient(sdbus::IConnection& bus, std::string service, std::string menuPath, std::string appPath,
                std::string windowPath, std::function<void()> onChanged);
  ~GtkMenuClient();
  GtkMenuClient(const GtkMenuClient&) = delete;
  GtkMenuClient& operator=(const GtkMenuClient&) = delete;

  [[nodiscard]] const DbusMenuNode& root() const noexcept { return m_root; }
  [[nodiscard]] bool loaded() const noexcept { return m_loaded; }
  void activate(std::int32_t id);

  struct Item {
    std::map<std::string, std::string> strings; // label, action, target (string targets only), accel
    std::vector<std::pair<std::string, std::pair<std::uint32_t, std::uint32_t>>> links; // ":section"/":submenu"
  };

private:
  void load();
  void loadActions();
  void build();
  void appendMenu(DbusMenuNode& parent, std::uint32_t group, std::uint32_t menu, int depth);

  std::string m_service;
  std::string m_menuPath;
  std::string m_appPath;
  std::string m_windowPath;
  std::function<void()> m_onChanged;
  std::unique_ptr<sdbus::IProxy> m_menus;
  std::unique_ptr<sdbus::IProxy> m_appActions;
  std::unique_ptr<sdbus::IProxy> m_windowActions;
  // (group, menu) -> items, from org.gtk.Menus.Start.
  std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<Item>> m_menusById;
  std::map<std::string, bool> m_enabled; // "app.quit" -> enabled
  // Ids stay stable across reloads, so an open menu activates the row it shows.
  std::vector<std::string> m_actionById; // node id - 1 -> "app.quit" (+ "\n" target)
  std::map<std::string, std::int32_t> m_idByAction;
  std::set<std::pair<std::uint32_t, std::uint32_t>> m_visiting; // (group, menu) on the current path
  std::vector<std::uint32_t> m_subscribed;
  std::uint64_t m_loadGeneration = 0; // replies from an older load() are dropped
  int m_nodeBudget = 0;               // rows build() may still add; caps menus that link one section many times
  DbusMenuNode m_root;
  bool m_loaded = false;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
