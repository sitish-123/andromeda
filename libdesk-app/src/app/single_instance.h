#pragma once

#include <functional>
#include <map>
#include <memory>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <string_view>
#include <vector>

class SessionBus;

// One process per app: the first launch owns the app id on the session bus and serves org.freedesktop.Application
// (what DBusActivatable=true and `gapplication launch` call). A later launch forwards its files or an Activate to
// the owner and exits.
class SingleInstance {
public:
  enum class Role : std::uint8_t {
    Primary,   // this process owns the name and serves the interface
    Forwarded, // another process owns it and took the request; exit now
    Alone,     // no session bus: run without single-instance
  };

  struct Handlers {
    // activationToken is the xdg-activation token of the launch, or empty.
    std::function<void(const std::string& activationToken)> activate;
    std::function<void(const std::vector<std::string>& uris, const std::string& activationToken)> open;
    // parameter: the string values of the action's parameter (s or as), else empty.
    std::function<void(const std::string& action, const std::vector<std::string>& parameter, const std::string& activationToken)>
        action;
  };

  SingleInstance(SessionBus* bus, std::string appId);
  ~SingleInstance();

  SingleInstance(const SingleInstance&) = delete;
  SingleInstance& operator=(const SingleInstance&) = delete;

  // uris empty = Activate. The launch's XDG_ACTIVATION_TOKEN goes along as platform data.
  Role claim(const std::vector<std::string>& uris, Handlers handlers);

  // "/dev/andromeda/Files" for "dev.andromeda.Files", as the freedesktop spec maps an app id to a path.
  [[nodiscard]] static std::string objectPathFor(std::string_view appId);

private:
  bool forward(const std::vector<std::string>& uris);
  void serve();

  SessionBus* m_bus = nullptr;
  std::string m_appId;
  Handlers m_handlers;
  std::unique_ptr<sdbus::IObject> m_object;
};
