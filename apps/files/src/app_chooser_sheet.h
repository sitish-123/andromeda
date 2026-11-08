#pragma once

#include "app_chooser_portal.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class Node;

// Open With (FILES-UI-SPEC §11, mockup §4): recommended apps, all apps with search, "Always use" toggle. Used by the
// AppChooser portal and by Files' own Open With. `answer` gets 0 + the desktop id (no ".desktop"), or 1 (cancelled).
class AppChooserSheet {
public:
  struct App {
    std::string id; // desktop id without ".desktop"
    std::string name;
    std::string icon; // icon file path, "" = none found
  };

  AppChooserSheet(AppChooserPortal::ChooseRequest request, std::function<void(std::uint32_t, const std::string&)> answer,
                  std::function<void()> rebuild);

  void updateChoices(const std::vector<std::string>& choices);
  // Keyboard and test actions (chooser.select / .accept / .cancel).
  void select(const std::string& id);
  void accept() { open(); }
  void cancel();
  [[nodiscard]] std::unique_ptr<Node> build(float scale);

  // The installed apps that show in menus, sorted by name.
  [[nodiscard]] static std::vector<App> allApps();

private:
  void open();
  void reloadRecommended();

  AppChooserPortal::ChooseRequest m_request;
  std::function<void(std::uint32_t, const std::string&)> m_answer;
  std::function<void()> m_rebuild;
  std::vector<App> m_recommended;
  std::vector<App> m_all;
  std::string m_default; // the type's current default app id
  std::string m_selected;
  std::string m_search;
  bool m_alwaysUse = false;
  bool m_answered = false;
  std::uint32_t m_lastClickTime = 0; // wl_pointer time of the last row click, ms
  static constexpr std::uint32_t kDoubleClickMs = 400;
  static constexpr std::size_t kAllAppsShown = 200; // rows built at most; search narrows the rest
};

// The Access portal dialog (FILES-UI-SPEC §11): icon, title, subtitle, body, choices, Deny / Grant.
class AccessSheet {
public:
  AccessSheet(AppChooserPortal::AccessRequest request,
              std::function<void(std::uint32_t, const std::vector<AppChooserPortal::AccessChoice>&)> answer);
  [[nodiscard]] std::unique_ptr<Node> build(float scale);
  void answer(bool grant); // access.grant / access.deny

private:
  AppChooserPortal::AccessRequest m_request;
  std::function<void(std::uint32_t, const std::vector<AppChooserPortal::AccessChoice>&)> m_answer;
  bool m_answered = false;
};

// A GIO icon name or path as a file under the icon theme dirs (hicolor, pixmaps), or "".
[[nodiscard]] std::string iconFileFor(const std::string& icon);
