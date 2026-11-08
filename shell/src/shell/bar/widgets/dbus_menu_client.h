#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sdbus {
  class IConnection;
  class IProxy;
} // namespace sdbus

struct DbusMenuNode {
  std::int32_t id = 0;
  std::string label;
  // Shortcut text as the board shows it ("Ctrl+Shift+Z").
  std::string shortcut;
  bool enabled = true;
  bool visible = true;
  bool separator = false;
  bool checkmark = false;
  bool radio = false;
  std::int32_t toggleState = -1;
  std::vector<DbusMenuNode> children;
};

// One app's com.canonical.dbusmenu tree (the macOS-layout top bar, board desktop-menus.html A5). Loads the whole tree
// async and again on LayoutUpdated or ItemsPropertiesUpdated; calls onChanged after each load.
class DbusMenuClient {
public:
  DbusMenuClient(sdbus::IConnection& bus, std::string service, std::string path, std::function<void()> onChanged);
  ~DbusMenuClient();
  DbusMenuClient(const DbusMenuClient&) = delete;
  DbusMenuClient& operator=(const DbusMenuClient&) = delete;

  [[nodiscard]] const DbusMenuNode& root() const noexcept { return m_root; }
  [[nodiscard]] bool loaded() const noexcept { return m_loaded; }
  [[nodiscard]] const std::string& service() const noexcept { return m_service; }
  [[nodiscard]] const std::string& path() const noexcept { return m_path; }
  // A menu opens: the app may fill it in first (AboutToShow), then the tree reloads.
  void opened(std::int32_t id);
  void closed(std::int32_t id);
  void activate(std::int32_t id);

private:
  void load();
  void event(std::int32_t id, const char* name);

  std::string m_service;
  std::string m_path;
  std::function<void()> m_onChanged;
  std::unique_ptr<sdbus::IProxy> m_proxy;
  DbusMenuNode m_root;
  bool m_loaded = false;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
