#include "app/desk_app.h"

#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "dbus/session_bus.h"
#include "dbus/session_bus_poll_source.h"
#include "wayland/wayland_seat.h"

#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  constexpr Logger kLog("app");

  // SIGINT and SIGTERM through a self-pipe, so a quit runs on the loop and the state is flushed. Not a signalfd:
  // its blocked mask would pass to every child the app starts.
  int g_signalPipe[2] = {-1, -1};

  void onSignal(int sig) {
    const auto byte = static_cast<unsigned char>(sig);
    [[maybe_unused]] const ssize_t n = ::write(g_signalPipe[1], &byte, 1);
  }

  class SignalSource final : public PollSource {
  public:
    explicit SignalSource(AppLoop& loop) : m_loop(loop) {
      if (g_signalPipe[0] < 0 && ::pipe2(g_signalPipe, O_NONBLOCK | O_CLOEXEC) != 0) {
        return;
      }
      struct sigaction action{};
      action.sa_handler = onSignal;
      sigemptyset(&action.sa_mask);
      action.sa_flags = SA_RESTART;
      sigaction(SIGINT, &action, nullptr);
      sigaction(SIGTERM, &action, nullptr);
    }
    ~SignalSource() override {
      std::signal(SIGINT, SIG_DFL);
      std::signal(SIGTERM, SIG_DFL);
    }
    SignalSource(const SignalSource&) = delete;
    SignalSource& operator=(const SignalSource&) = delete;

    void dispatch(const std::vector<pollfd>& /*fds*/, std::size_t /*startIdx*/) override {
      unsigned char sig = 0;
      while (::read(g_signalPipe[0], &sig, 1) == 1) {
        kLog.info("signal {}; quitting", static_cast<int>(sig));
        m_loop.quit(0);
      }
    }

  private:
    void doAddPollFds(std::vector<pollfd>& fds) override {
      if (g_signalPipe[0] >= 0) {
        fds.push_back({.fd = g_signalPipe[0], .events = POLLIN, .revents = 0});
      }
    }

    AppLoop& m_loop;
  };

  std::string toUri(const std::string& arg) {
    if (arg.find("://") != std::string::npos) {
      return arg;
    }
    std::error_code ec;
    const auto abs = std::filesystem::absolute(arg, ec);
    return "file://" + (ec ? arg : abs.lexically_normal().string());
  }
} // namespace

DeskApp::DeskApp(Info info) : m_info(std::move(info)), m_state(StateStore::pathFor(m_info.id)) {}

DeskApp::~DeskApp() {
  m_windows.clear();
  m_state.flush();
  m_loop.removeSource(m_busSource.get());
  m_loop.removeSource(m_signals.get());
}

int DeskApp::run(int argc, char* argv[]) {
  initLogLevelFromEnvironment();
  std::vector<std::string> uris;
  for (int i = 1; i < argc; ++i) {
    if (argv[i][0] != '-') {
      uris.push_back(toUri(argv[i]));
    }
  }

  try {
    m_bus = std::make_unique<SessionBus>();
  } catch (const std::exception& e) {
    kLog.warn("no session bus ({}); single instance is off", e.what());
  }
  m_instance = std::make_unique<SingleInstance>(m_bus.get(), m_info.id);
  const SingleInstance::Role role = m_instance->claim(
      uris, {
                .activate = [this](const std::string& token) { activate(token); },
                .open = [this](const std::vector<std::string>& u, const std::string& token) { open(u, token); },
                .action = [this](const std::string& name, const std::string& token) { action(name, token); },
            }
  );
  if (role == SingleInstance::Role::Forwarded) {
    return 0;
  }

  if (!m_loop.initialize()) {
    return 1;
  }
  if (m_bus != nullptr) {
    m_busSource = std::make_unique<SessionBusPollSource>(*m_bus);
    m_loop.addSource(m_busSource.get());
  }
  m_signals = std::make_unique<SignalSource>(m_loop);
  m_loop.addSource(m_signals.get());

  m_state.load();
  addStandardCommands();
  m_loop.wayland().setPointerEventCallback([this](const PointerEvent& event) { routePointer(event); });
  m_loop.wayland().setKeyboardEventCallback([this](const KeyboardEvent& event) { routeKey(event); });

  startup();
  const char* token = std::getenv("XDG_ACTIVATION_TOKEN");
  const std::string launchToken = token != nullptr ? token : "";
  unsetenv("XDG_ACTIVATION_TOKEN");
  if (uris.empty()) {
    activate(launchToken);
  } else {
    open(uris, launchToken);
  }
  if (m_windows.empty()) {
    kLog.error("{} opened no window", m_info.id);
    return 1;
  }
  const int code = m_loop.run();
  m_state.flush();
  return code;
}

AppWindow& DeskApp::addWindow(AppWindow::Config config) {
  if (config.appId.empty()) {
    config.appId = m_info.id;
  }
  if (config.title.empty()) {
    config.title = m_info.name;
  }
  auto& window = *m_windows.emplace_back(std::make_unique<AppWindow>(m_loop, m_commands, m_state, std::move(config)));
  window.setClosedCallback([this] { reapClosed(); });
  return window;
}

void DeskApp::reapClosed() {
  std::erase_if(m_windows, [](const auto& w) { return !w->isOpen(); });
  if (m_windows.empty()) {
    m_loop.quit(0);
  }
}

AppWindow* DeskApp::windowFor(wl_surface* surface) const {
  if (surface == nullptr) {
    return nullptr;
  }
  for (const auto& w : m_windows) {
    if (w->wlSurface() == surface) {
      return w.get();
    }
  }
  return nullptr;
}

AppWindow* DeskApp::activeWindow() const {
  if (AppWindow* focused = windowFor(m_loop.wayland().lastKeyboardSurface())) {
    return focused;
  }
  for (auto it = m_windows.rbegin(); it != m_windows.rend(); ++it) {
    if ((*it)->isOpen()) {
      return it->get();
    }
  }
  return nullptr;
}

void DeskApp::activate(const std::string& activationToken) {
  if (AppWindow* window = activeWindow()) {
    window->activate(activationToken);
    return;
  }
  AppWindow& window = createWindow();
  if (!window.open()) {
    reapClosed();
    return;
  }
  window.activate(activationToken);
}

void DeskApp::open(const std::vector<std::string>& /*uris*/, const std::string& activationToken) {
  activate(activationToken);
}

void DeskApp::action(const std::string& name, const std::string& activationToken) {
  if (!m_commands.run(name)) {
    kLog.warn("unknown or disabled action {}", name);
  }
  if (AppWindow* window = activeWindow()) {
    window->activate(activationToken);
  }
}

void DeskApp::addStandardCommands() {
  const auto chord = [](std::uint32_t sym, std::uint32_t mods) { return KeyChord{.sym = sym, .modifiers = mods}; };
  const auto withWindow = [this](void (*fn)(AppWindow&)) {
    return [this, fn] {
      if (AppWindow* w = activeWindow()) {
        fn(*w);
      }
    };
  };
  m_commands.add({
      .id = "window.close",
      .label = "Close Window",
      .group = "Window",
      .shortcut = chord(XKB_KEY_w, KeyMod::Ctrl),
      .run = [this] {
        if (AppWindow* w = activeWindow()) {
          DeferredCall::callLater([this, w] {
            for (const auto& owned : m_windows) {
              if (owned.get() == w) {
                w->close();
              }
            }
            reapClosed();
          });
        }
      },
  });
  m_commands.add({
      .id = "window.maximize",
      .label = "Zoom",
      .group = "Window",
      .run = withWindow([](AppWindow& w) { w.toggleMaximized(); }),
  });
  m_commands.add({
      .id = "window.minimize",
      .label = "Minimize",
      .group = "Window",
      .shortcut = chord(XKB_KEY_m, KeyMod::Ctrl),
      .run = withWindow([](AppWindow& w) {
        if (w.surface() != nullptr) {
          w.surface()->setMinimized();
        }
      }),
  });
  m_commands.add({
      .id = "app.quit",
      .label = "Quit",
      .group = "File",
      .shortcut = chord(XKB_KEY_q, KeyMod::Ctrl),
      .run = [this] { DeferredCall::callLater([this] { m_loop.quit(0); }); },
  });
}

void DeskApp::routePointer(const PointerEvent& event) {
  if (event.type == PointerEvent::Type::Enter) {
    m_pointerSurface = event.surface;
  }
  AppWindow* window = windowFor(event.surface != nullptr ? event.surface : m_pointerSurface);
  if (event.type == PointerEvent::Type::Leave && event.surface == m_pointerSurface) {
    m_pointerSurface = nullptr;
  }
  if (window != nullptr) {
    window->onPointerEvent(event);
  }
}

void DeskApp::routeKey(const KeyboardEvent& event) {
  if (AppWindow* window = windowFor(m_loop.wayland().lastKeyboardSurface())) {
    window->onKeyboardEvent(event);
  }
}
