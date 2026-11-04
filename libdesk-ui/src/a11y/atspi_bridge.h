#pragma once

#include "render/scene/accessible.h"

#include <sdbus-c++/IConnection.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class Node;

namespace sdbus {
  class IObject;
  class IProxy;
} // namespace sdbus

// Publishes libdesk-ui scene trees to assistive technology over AT-SPI (docs/accessibility.md §1,
// .agent/a11y-atspi-protocol.md). Idle until org.a11y.Status reports a screen reader or AT; then it connects to the
// accessibility bus, embeds an application root and exports one object per accessible node. No thread: the host
// loop polls pollData() and calls dispatch().
class AtspiBridge {
public:
  static AtspiBridge& instance();

  AtspiBridge();
  ~AtspiBridge();
  AtspiBridge(const AtspiBridge&) = delete;
  AtspiBridge& operator=(const AtspiBridge&) = delete;

  // Watches org.a11y.Status on the host's session bus connection. `appName` is the AT-SPI application name.
  void start(sdbus::IConnection& sessionBus, std::string appName);
  void stop();

  // A surface's scene root; `title` names its window object when the root has no name. Roots must be removed
  // before they are destroyed.
  void addRoot(const Node* root, std::function<std::string()> title);
  void removeRoot(const Node* root);
  // Re-reads every root's tree and emits change events. Cheap no-op while no AT is active.
  void update();
  // A root repainted: the poll source runs update() at most every kUpdateIntervalMs.
  void markDirty() noexcept { m_dirty = m_connection != nullptr; }
  [[nodiscard]] bool dirty() const noexcept { return m_dirty; }
  static constexpr int kUpdateIntervalMs = 100;

  [[nodiscard]] bool active() const noexcept { return m_connection != nullptr; }
  // Poll data of the accessibility bus connection while active.
  [[nodiscard]] std::optional<sdbus::IConnection::PollData> pollData() const;
  void dispatch();

private:
  struct Item {
    std::uint32_t id = 0;
    std::uint64_t key = 0;
    std::uint32_t parent = 0;
    std::vector<std::uint32_t> children;
    const Node* node = nullptr;
    const Node* root = nullptr;
    AccessibleRole role = AccessibleRole::None;
    std::string name;
    std::string description;
    std::optional<AccessibleValue> value;
    std::string actionName;
    std::optional<std::string> text;
    std::int32_t caretOffset = 0;
    std::uint64_t states = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::unique_ptr<sdbus::IObject> object;
  };
  struct Root {
    const Node* node = nullptr;
    std::function<std::string()> title;
  };

  void setEnabled(bool enabled);
  void failed(const char* what, const std::exception& error);
  void connect(const std::string& address);
  void disconnect();
  void exportApplication();
  void exportCache();
  void updateTree();
  void exportItem(Item& item);
  [[nodiscard]] std::uint32_t idFor(std::uint64_t key);
  [[nodiscard]] std::int32_t indexInParent(std::uint32_t id) const;
  [[nodiscard]] std::vector<std::uint32_t> windowIds() const;
  [[nodiscard]] std::string pathFor(std::uint32_t id) const;
  [[nodiscard]] sdbus::Struct<std::string, sdbus::ObjectPath> refFor(std::uint32_t id) const;
  [[nodiscard]] const Item* item(std::uint32_t id) const;
  // The item's node when it is still in its root's tree, else nullptr.
  [[nodiscard]] Node* liveNode(std::uint32_t id) const;
  void emitObjectEvent(
      std::uint32_t id, const std::string& member, const std::string& detail, std::int32_t detail1,
      std::int32_t detail2, const sdbus::Variant& value, const char* interfaceName = nullptr
  );

  std::string m_appName;
  std::unique_ptr<sdbus::IProxy> m_busProxy;
  std::unique_ptr<sdbus::IConnection> m_connection;
  std::unique_ptr<sdbus::IObject> m_application;
  std::unique_ptr<sdbus::IObject> m_cache;
  std::unique_ptr<sdbus::IProxy> m_registry;
  std::vector<Root> m_roots;
  std::unordered_map<std::uint64_t, std::uint32_t> m_ids;
  std::unordered_map<std::uint32_t, Item> m_items;
  std::uint32_t m_nextId = 1;
  std::int32_t m_applicationId = 0;
  bool m_dirty = false;
  bool m_isEnabled = false;
  bool m_screenReaderEnabled = false;
  bool m_wanted = false;
  bool m_connecting = false;
  std::uint32_t m_activeWindow = 0;
  std::string m_desktopBus;
  std::string m_desktopPath = "/org/a11y/atspi/null";
  bool m_strict = false;
  // Actions and focus requests run after dispatch(): they can destroy the object whose handler queued them.
  std::vector<std::function<void()>> m_deferred;
  std::unordered_map<std::uint32_t, bool> m_reportedUnnamed;
  std::shared_ptr<int> m_alive = std::make_shared<int>(0);
};
