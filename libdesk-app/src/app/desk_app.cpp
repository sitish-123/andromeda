#include "app/desk_app.h"

#include "a11y/atspi_bridge.h"
#include "a11y/atspi_poll_source.h"

#include "app/appearance.h"
#include "app/glib_source.h"

#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "dbus/session_bus.h"
#include "dbus/session_bus_poll_source.h"
#include "ui/controls/input.h"
#include "ui/tooltip/tooltip_manager.h"
#include "wayland/text_input_service.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <string_view>
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
  TooltipManager::instance().forceDestroy();
  m_windows.clear();
  TooltipManager::instance().shutdown();
  Input::setTextClipboard(nullptr);
  Input::setContextMenuHandler({});
  if (m_textInput != nullptr) {
    m_loop.wayland().setTextInputService(nullptr);
  }
  m_state.flush();
  m_loop.removeSource(m_busSource.get());
  if (m_glib != nullptr) {
    m_loop.removeSource(m_glib.get());
  }
  m_loop.removeSource(m_signals.get());
  if (m_atspi != nullptr) {
    m_loop.removeSource(m_atspi.get());
    AtspiBridge::instance().stop();
  }
}

int DeskApp::run(int argc, char* argv[]) {
  initLogLevelFromEnvironment();
  std::vector<std::string> uris;
  bool serviceStart = false; // D-Bus activation: the Activate or Open call that started us makes the window
  std::string startAction;   // --action=<name>: the files go to action(name, uris) instead of open()
  for (int i = 1; i < argc; ++i) {
    serviceStart = serviceStart || std::string_view(argv[i]) == "--gapplication-service";
    if (const std::string_view arg = argv[i]; arg.starts_with("--action=")) {
      startAction = arg.substr(std::string_view("--action=").size());
    }
    if (argv[i][0] != '-') {
      uris.push_back(toUri(argv[i]));
    }
  }

  try {
    m_bus = std::make_unique<SessionBus>();
    // The bus ends with the session: quit through the loop so the state is flushed.
    m_bus->setDisconnectedCallback([this] {
      kLog.warn("session bus lost; quitting");
      m_loop.quit(0);
    });
  } catch (const std::exception& e) {
    kLog.warn("no session bus ({}); single instance is off", e.what());
  }
  m_instance = std::make_unique<SingleInstance>(m_bus.get(), m_info.id);
  const SingleInstance::Role role = m_instance->claim(
      uris, {
                .activate = [this](const std::string& token) { activate(token); },
                .open = [this](const std::vector<std::string>& u, const std::string& token) { open(u, token); },
                .action = [this](const std::string& name, const std::vector<std::string>& parameter,
                                 const std::string& token) { action(name, parameter, token); },
            },
      startAction
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
    m_appearance = std::make_unique<Appearance>(*m_bus, [this] {
      for (const auto& w : m_windows) {
        w->requestRebuild(); // colours resolve at build time in places, so every node gets the new scheme
      }
    });
  }
  m_signals = std::make_unique<SignalSource>(m_loop);
  m_loop.addSource(m_signals.get());
  m_glib = std::make_unique<GlibSource>();
  m_loop.addSource(m_glib.get());
  if (m_bus != nullptr) {
    // Screen readers (Orca) see the app's windows: AppWindow surfaces add their scene roots.
    AtspiBridge::instance().start(m_bus->connection(), m_info.name);
    m_atspi = std::make_unique<AtspiPollSource>();
    m_loop.addSource(m_atspi.get());
  }

  m_state.load();
  m_textInput = std::make_unique<TextInputService>();
  m_loop.wayland().setTextInputService(m_textInput.get());
  m_clipboard = std::make_unique<AppClipboard>(m_loop.wayland());
  TooltipManager::instance().initialize(
      m_loop.wayland(), [this] { return m_windows.empty() ? 1.0F : m_windows.front()->uiScale(); }, &m_loop.renderContext()
  );
  Input::setTextClipboard(m_clipboard.get());
  Input::setContextMenuHandler([this](Input& input, float x, float y) {
    for (const auto& window : m_windows) {
      if (window->openTextMenu(input, x, y)) {
        return;
      }
    }
  });
  addStandardCommands();
  m_loop.wayland().setPointerEventCallback([this](const PointerEvent& event) { routePointer(event); });
  m_loop.wayland().setPinchEventCallback([this](const PinchEvent& event) {
    if (AppWindow* window = windowFor(event.surface)) {
      window->onPinchEvent(event);
    }
  });
  m_loop.wayland().setKeyboardEventCallback([this](const KeyboardEvent& event) { routeKey(event); });

  startup();
  const char* token = std::getenv("XDG_ACTIVATION_TOKEN");
  const std::string launchToken = token != nullptr ? token : "";
  unsetenv("XDG_ACTIVATION_TOKEN");
  if (serviceStart && uris.empty()) {
    m_serviceQuit.start(std::chrono::seconds(30), [this] {
      if (m_windows.empty() && quitWithLastWindow()) {
        m_loop.quit(0);
      }
    });
  } else if (!startAction.empty()) {
    action(startAction, uris, launchToken);
  } else if (uris.empty()) {
    activate(launchToken);
  } else {
    open(uris, launchToken);
  }
  if (!serviceStart && m_windows.empty() && quitWithLastWindow()) {
    kLog.error("{} opened no window", m_info.id);
    return 1;
  }
  const int code = m_loop.run();
  shutdown();
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

void DeskApp::closeWindow(AppWindow& window) {
  DeferredCall::callLater([this, w = &window] {
    for (const auto& owned : m_windows) {
      if (owned.get() == w) {
        w->close();
      }
    }
    reapClosed();
  });
}

void DeskApp::reapClosed() {
  for (const auto& w : m_windows) {
    if (!w->isOpen()) {
      windowClosed(*w);
    }
  }
  std::erase_if(m_windows, [](const auto& w) { return !w->isOpen(); });
  if (m_windows.empty() && quitWithLastWindow()) {
    m_loop.quit(0);
  }
}

AppWindow* DeskApp::windowFor(wl_surface* surface) const {
  if (surface == nullptr) {
    return nullptr;
  }
  for (const auto& w : m_windows) {
    if (w->ownsSurface(surface)) {
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

void DeskApp::action(
    const std::string& name, const std::vector<std::string>& /*parameter*/, const std::string& activationToken
) {
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
          closeWindow(*w);
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
