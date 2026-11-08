#pragma once

#include "system_portals.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class Node;

// The Account, Wallpaper and DynamicLauncher dialogs (FILES-UI-SPEC §12): a picture, a title, a body, an optional
// name field, Cancel / the action. `answer` gets 0 (granted) or 1 (cancelled) and the final name.
class PortalSheet {
public:
  PortalSheet(SystemPortals::Request request, std::function<void(std::uint32_t, const std::string&)> answer);

  [[nodiscard]] std::unique_ptr<Node> build(float scale);
  [[nodiscard]] static std::string titleFor(const SystemPortals::Request& request);
  void setName(std::string name) { m_name = std::move(name); } // test action portal.name
  void accept();
  void cancel();

private:
  SystemPortals::Request m_request;
  std::function<void(std::uint32_t, const std::string&)> m_answer;
  SystemPortals::User m_user; // Account
  std::string m_name;         // Launcher
  bool m_answered = false;
};

// A picture from a file path, an icon name, or "" (a grey placeholder), loaded at the first layout when a renderer is
// there; `radius` < 0 = a circle.
[[nodiscard]] std::unique_ptr<Node> pictureNode(std::string path, float width, float height, float radius);
