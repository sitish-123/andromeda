#pragma once

#include "app/app_clipboard.h"
#include "app/app_loop.h"
#include "app/app_window.h"
#include "app/command_table.h"
#include "app/single_instance.h"
#include "app/state_store.h"
#include "core/timer_manager.h"

#include <memory>
#include <string>
#include <vector>

class Appearance;
class GlibSource;
class SessionBus;
class SessionBusPollSource;

// A first-party app process: run loop, session bus, single instance, state and the command table, with the
// standard window commands (window.close Ctrl+W, window.maximize, window.minimize, app.quit Ctrl+Q).
// Subclass it and implement activate() and open(); call run() from main().
class DeskApp {
public:
  struct Info {
    std::string id;   // reverse-DNS app id and D-Bus name, e.g. "dev.andromeda.Files"
    std::string name; // shown in the window title
  };

  explicit DeskApp(Info info);
  virtual ~DeskApp();

  DeskApp(const DeskApp&) = delete;
  DeskApp& operator=(const DeskApp&) = delete;

  // Arguments after argv[0] that are not options are files or URIs to open. Returns the process exit code.
  int run(int argc, char* argv[]);
  void quit(int code = 0) { m_loop.quit(code); }

  [[nodiscard]] const Info& info() const noexcept { return m_info; }
  [[nodiscard]] AppLoop& loop() noexcept { return m_loop; }
  [[nodiscard]] CommandTable& commands() noexcept { return m_commands; }
  [[nodiscard]] StateStore& state() noexcept { return m_state; }
  // Null when the session bus is not reachable.
  [[nodiscard]] SessionBus* bus() noexcept { return m_bus.get(); }

  // A new window that the app owns; it is destroyed after its close, and the app quits after the last one.
  AppWindow& addWindow(AppWindow::Config config);
  [[nodiscard]] const std::vector<std::unique_ptr<AppWindow>>& windows() const noexcept { return m_windows; }
  // The window with keyboard focus, else the newest one; null when none is open.
  [[nodiscard]] AppWindow* activeWindow() const;
  // Closes the window after the current dispatch (safe from its own input callbacks).
  void closeWindow(AppWindow& window);

protected:
  // A launch with no files (first start or a forwarded launch). Default: raise the active window, else open one.
  virtual void activate(const std::string& activationToken);
  virtual void open(const std::vector<std::string>& uris, const std::string& activationToken);
  // org.freedesktop.Application.ActivateAction: runs the command with that id unless a subclass takes it first.
  virtual void action(const std::string& name, const std::vector<std::string>& parameter, const std::string& activationToken);
  // Called once before the first activate/open, with the loop and bus up.
  virtual void startup() {}
  // After the loop ends (any quit), before the state is flushed.
  virtual void shutdown() {}
  [[nodiscard]] virtual AppWindow& createWindow() = 0;
  // Before a closed window is destroyed: drop what refers to it.
  virtual void windowClosed(AppWindow& /*window*/) {}
  // False for a service process that outlives its windows (the file picker); it then calls quit() itself.
  [[nodiscard]] virtual bool quitWithLastWindow() const { return true; }
  // For a subclass destructor whose members the windows' content uses: destroys them first, with no windowClosed.
  void destroyWindows() { m_windows.clear(); }

private:
  void addStandardCommands();
  void routePointer(const PointerEvent& event);
  void routeKey(const KeyboardEvent& event);
  [[nodiscard]] AppWindow* windowFor(wl_surface* surface) const;
  void reapClosed();

  Info m_info;
  AppLoop m_loop;
  std::unique_ptr<SessionBus> m_bus;
  std::unique_ptr<SessionBusPollSource> m_busSource;
  std::unique_ptr<Appearance> m_appearance; // after the bus: it uses it
  std::unique_ptr<PollSource> m_signals;
  std::unique_ptr<GlibSource> m_glib; // GIO async calls complete in the app loop
  std::unique_ptr<class AtspiPollSource> m_atspi;
  std::unique_ptr<SingleInstance> m_instance;
  std::unique_ptr<AppClipboard> m_clipboard;
  std::unique_ptr<class TextInputService> m_textInput;
  StateStore m_state;
  CommandTable m_commands;
  std::vector<std::unique_ptr<AppWindow>> m_windows;
  Timer m_serviceQuit; // a D-Bus activation start whose request never came
  wl_surface* m_pointerSurface = nullptr;
};
