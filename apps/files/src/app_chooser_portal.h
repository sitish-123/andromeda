#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class SessionBus;

namespace sdbus {
  class IObject;
}

// org.freedesktop.impl.portal.AppChooser and org.freedesktop.impl.portal.Access, served next to the FileChooser by
// `andromeda-files --picker` (FILES-UI-SPEC §11). Each call stays pending until its sheet answers or the portal closes
// the Request.
class AppChooserPortal {
public:
  struct ChooseRequest {
    std::string appId;        // the asking app
    std::string parentWindow; // "wayland:<handle>" or ""
    std::vector<std::string> choices; // desktop ids without ".desktop", recommended first
    std::string lastChoice;
    std::string contentType;
    std::string filename;
  };

  struct AccessChoice {
    std::string id;
    std::string label;
    std::vector<std::pair<std::string, std::string>> options; // empty = a check box ("true" / "false")
    std::string value;
  };

  struct AccessRequest {
    std::string appId;
    std::string parentWindow;
    std::string title;
    std::string subtitle;
    std::string body;
    std::string denyLabel;
    std::string grantLabel;
    std::string icon;
    std::vector<AccessChoice> choices;
  };

  struct Handlers {
    std::function<void(std::uint64_t id, const ChooseRequest& request)> choose;
    std::function<void(std::uint64_t id, const std::vector<std::string>& choices)> updateChoices;
    std::function<void(std::uint64_t id, const AccessRequest& request)> access;
    std::function<void(std::uint64_t id)> closed; // the caller gave up
  };

  AppChooserPortal(SessionBus& bus, Handlers handlers);
  ~AppChooserPortal();

  AppChooserPortal(const AppChooserPortal&) = delete;
  AppChooserPortal& operator=(const AppChooserPortal&) = delete;

  // response 0 granted / chosen, 1 cancelled, 2 other. `choice` is the desktop id (AppChooser).
  void answerChoose(std::uint64_t id, std::uint32_t response, const std::string& choice);
  void answerAccess(std::uint64_t id, std::uint32_t response, const std::vector<AccessChoice>& choices);
  [[nodiscard]] bool idle() const noexcept { return m_pending.empty(); }

private:
  struct Pending;
  std::uint64_t track(const std::string& handle, std::unique_ptr<Pending> pending);

  SessionBus& m_bus;
  Handlers m_handlers;
  std::unique_ptr<sdbus::IObject> m_chooser;
  std::unique_ptr<sdbus::IObject> m_access;
  std::map<std::uint64_t, std::unique_ptr<Pending>> m_pending;
  std::map<std::string, std::uint64_t> m_byHandle; // UpdateChoices names the Request by its handle
  std::uint64_t m_nextId = 1;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
