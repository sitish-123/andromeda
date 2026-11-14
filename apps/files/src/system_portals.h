#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class SessionBus;

namespace sdbus {
  class IObject;
  class IProxy;
}

// The PT2 portal backends served by `andromeda-files --picker` next to the FileChooser (FILES-UI-SPEC §12):
// org.freedesktop.impl.portal.Account, .Wallpaper and .DynamicLauncher ask through a sheet. The shell serves
// .Notification.
class SystemPortals {
public:
  enum class Kind : std::uint8_t { Account, Wallpaper, Launcher };

  struct Request {
    Kind kind = Kind::Account;
    std::string appId;
    std::string parentWindow;
    std::string reason;     // Account
    std::string path;       // Wallpaper: local file; Launcher: icon file ("" = none)
    std::string setOn;      // Wallpaper: "background", "lockscreen" or "both"
    bool showPreview = true; // Wallpaper
    std::string name;       // Launcher: the proposed name
    bool editableName = true;
    bool webApp = false;    // Launcher type 2
    std::string target;     // Launcher: the web app URL
  };

  struct Handlers {
    std::function<void(std::uint64_t id, const Request& request)> ask;
    std::function<void(std::uint64_t id)> closed; // the caller gave up
  };

  SystemPortals(SessionBus& bus, Handlers handlers);
  ~SystemPortals();

  SystemPortals(const SystemPortals&) = delete;
  SystemPortals& operator=(const SystemPortals&) = delete;

  // response 0 granted, 1 cancelled, 2 other. `name` is the Launcher's final name.
  void answer(std::uint64_t id, std::uint32_t response, const std::string& name = {});
  [[nodiscard]] bool idle() const noexcept { return m_pending.empty(); }

  // The user's id, real name and picture file ("" = none), as Account returns them.
  struct User {
    std::string id;
    std::string name;
    std::string image;
  };
  [[nodiscard]] static User currentUser();

private:
  struct Pending;

  std::uint64_t track(const std::string& handle, std::unique_ptr<Pending> pending);

  SessionBus& m_bus;
  Handlers m_handlers;
  std::unique_ptr<sdbus::IObject> m_account;
  std::unique_ptr<sdbus::IObject> m_wallpaper;
  std::unique_ptr<sdbus::IObject> m_launcher;
  std::map<std::uint64_t, std::unique_ptr<Pending>> m_pending;
  std::uint64_t m_nextId = 1;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
