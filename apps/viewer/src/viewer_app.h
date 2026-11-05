#pragma once

#include "app/desk_app.h"
#include "clipboard_source.h"

#include <filesystem>
#include <map>
#include <memory>

namespace viewer {

  class ViewerWindow;

  // andromeda-viewer: one process (DeskApp single instance); each opened file gets its own window.
  class ViewerApp final : public DeskApp {
  public:
    ViewerApp();
    ~ViewerApp() override;

  protected:
    void startup() override;
    void open(const std::vector<std::string>& uris, const std::string& activationToken) override;
    // "search" with one string: searches the active window (the same path as typing in its search field).
    void action(const std::string& name, const std::vector<std::string>& parameter, const std::string& token) override;
    AppWindow& createWindow() override;
    void windowClosed(AppWindow& window) override;

  private:
    void addCommands();
    [[nodiscard]] ViewerWindow* active() const;

    std::unique_ptr<ClipboardSource> m_clipboard;
    std::map<AppWindow*, std::unique_ptr<ViewerWindow>> m_windows;
  };

} // namespace viewer
