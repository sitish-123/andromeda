#pragma once

#include <sdbus-c++/sdbus-c++.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class NotificationManager;
class SessionBus;

// org.freedesktop.impl.portal.Notification v2 (docs/focus.md §5): portal notifications from sandboxed and GTK 4 apps
// go into the one NotificationManager store, keyed by (app_id, id).
class NotificationPortal {
public:
  NotificationPortal(SessionBus& bus, NotificationManager& manager);
  ~NotificationPortal();

  NotificationPortal(const NotificationPortal&) = delete;
  NotificationPortal& operator=(const NotificationPortal&) = delete;

  using Options = std::map<std::string, sdbus::Variant>;

private:
  struct Action {
    std::string name;                     // the app's action; empty: activate the app
    std::optional<sdbus::Variant> target; // the app's action target
  };
  struct Entry {
    std::string appId;
    std::string id;
    Action defaultAction;
    std::map<std::string, Action> buttons; // by manager action key
    std::string iconFile;                  // written from bytes or a file descriptor; removed with the entry
  };

  void add(const std::string& appId, const std::string& id, const Options& n, const std::string& sender);
  void remove(const std::string& appId, const std::string& id);
  void invoke(std::uint32_t managerId, const std::string& key, const std::string& token,
              const std::optional<std::string>& response);

  SessionBus& m_bus;
  NotificationManager& m_manager;
  std::unique_ptr<sdbus::IObject> m_object;
  std::map<std::uint32_t, Entry> m_entries;               // by manager id
  std::map<std::pair<std::string, std::string>, std::uint32_t> m_byKey; // (app_id, id) -> manager id
  std::uint64_t m_iconSerial = 0;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
