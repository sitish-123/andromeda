#include "viewer_app.h"

#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "viewer_window.h"

#include <gio/gio.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace viewer {

  namespace {
    constexpr Logger kLog("viewer");

    std::filesystem::path pathFromUri(const std::string& uri) {
      if (uri.find("://") == std::string::npos) {
        return std::filesystem::absolute(uri);
      }
      char* path = g_filename_from_uri(uri.c_str(), nullptr, nullptr);
      std::filesystem::path out = path != nullptr ? path : "";
      g_free(path);
      return out;
    }
  } // namespace

  ViewerApp::ViewerApp() : DeskApp({.id = "dev.andromeda.Viewer", .name = "Viewer"}) {}

  ViewerApp::~ViewerApp() = default;

  void ViewerApp::startup() {
    addCommands();
  }

  void ViewerApp::addCommands() {
    const auto add = [this](const char* id, const char* label, const char* group, std::uint32_t sym,
                            std::uint32_t mods, void (ViewerWindow::*run)()) {
      commands().add({.id = id, .label = label, .group = group,
                      .shortcut = sym != 0 ? std::optional<KeyChord>(KeyChord{sym, mods}) : std::nullopt,
                      .enabled = [this] { return active() != nullptr; },
                      .run = [this, run] {
                        if (ViewerWindow* w = active(); w != nullptr) {
                          (w->*run)();
                        }
                      }});
    };
    add("edit.find", "Find", "Edit", XKB_KEY_f, KeyMod::Ctrl, &ViewerWindow::focusSearch);
    add("edit.copy", "Copy", "Edit", XKB_KEY_c, KeyMod::Ctrl, &ViewerWindow::copySelection);
    add("view.zoom-in", "Zoom In", "View", XKB_KEY_plus, KeyMod::Ctrl, &ViewerWindow::zoomIn);
    add("view.zoom-out", "Zoom Out", "View", XKB_KEY_minus, KeyMod::Ctrl, &ViewerWindow::zoomOut);
    add("view.zoom-fit", "Fit Width", "View", XKB_KEY_0, KeyMod::Ctrl, &ViewerWindow::zoomFit);
    add("edit.undo", "Undo", "Edit", XKB_KEY_z, KeyMod::Ctrl, &ViewerWindow::undo);
    add("file.save", "Save", "File", XKB_KEY_s, KeyMod::Ctrl, &ViewerWindow::save);
    add("view.toggle-sidebar", "Show Sidebar", "View", 0, 0, &ViewerWindow::toggleSidebar);
  }

  ViewerWindow* ViewerApp::active() const {
    AppWindow* window = activeWindow();
    const auto it = window != nullptr ? m_windows.find(window) : m_windows.end();
    return it != m_windows.end() ? it->second.get() : nullptr;
  }

  AppWindow& ViewerApp::createWindow() {
    AppWindow& window = addWindow({.appId = info().id, .title = info().name, .width = 900, .height = 760});
    m_windows[&window] = std::make_unique<ViewerWindow>(loop(), window);
    return window;
  }

  void ViewerApp::open(const std::vector<std::string>& uris, const std::string& activationToken) {
    for (const std::string& uri : uris) {
      const auto path = pathFromUri(uri);
      if (path.empty()) {
        kLog.warn("cannot open {}", uri);
        continue;
      }
      AppWindow& window = createWindow();
      m_windows[&window]->open(path);
      window.open();
      window.activate(activationToken);
    }
  }

  void ViewerApp::action(const std::string& name, const std::vector<std::string>& parameter, const std::string& token) {
    if (name == "search" && parameter.size() == 1) {
      if (ViewerWindow* w = active(); w != nullptr) {
        w->searchFor(parameter.front());
      }
      return;
    }
    DeskApp::action(name, parameter, token);
  }

  void ViewerApp::windowClosed(AppWindow& window) { m_windows.erase(&window); }

} // namespace viewer
