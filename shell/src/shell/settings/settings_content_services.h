#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

class Flex;
class Node;

namespace settings {

  struct SettingsServicesContext {
    float scale = 1.0F;
    struct Player {
      std::string label;
      // Shown in the island and the control center (not on shell.mpris.blacklist).
      bool shown = true;
      std::function<void(bool shown)> setShown;
    };
    // Running players first, then hidden players that are not running.
    std::vector<Player> players;
    bool playersLoaded = false;

    struct BackgroundApp {
      std::string id;
      std::string label;
      std::string icon;
      bool allowed = false;
      bool startsAtLogin = false;
    };
    // Grants of the Background portal (permission store table "background"); empty until loaded.
    std::vector<BackgroundApp> background;
    bool backgroundLoaded = false;
    std::function<void(std::string app, bool allowed)> setBackground;
    std::function<std::unique_ptr<Node>(const std::string& icon, float size)> appIcon;

    struct DefaultApp {
      std::string label;
      std::string app;
    };
    std::vector<DefaultApp> defaults;
    std::function<void()> openDefaultApps;
  };

  // Apps and Services page (.agent/mockups/settings-d6.html §3): Media Players, Running in the Background,
  // Default Apps.
  void addSettingsServices(Flex& content, const SettingsServicesContext& ctx);

} // namespace settings
