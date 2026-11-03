#include "greeter/greeter_host.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "greeter/greetd_client.h"
#include "render/gl_shared_context.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "time/time_format.h"
#include "ui/auth/auth_column.h"
#include "ui/builders.h"
#include "ui/controls/image.h"
#include "wayland/layer_surface.h"
#include "wayland/surface.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <array>
#include <atomic>
#ifdef NOCTALIA_USE_JEMALLOC
#include <jemalloc/jemalloc.h>
#else
#include <malloc.h>
#endif
#include <cerrno>
#include <chrono>
#include <csignal>
#include <print>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <poll.h>
#include <unistd.h>
#include <sodium.h>
#include <sstream>
#include <string>
#include <toml++/toml.hpp>
#include <vector>
#include <wayland-client-core.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  constexpr Logger kLog("greeter");
  // Root-owned, written by the appearance sync helper; the only config greeter mode reads.
  constexpr const char* kDefaultStateDir = "/var/lib/noctalia-greeter";

  std::atomic<bool> g_quit{false};
  // Fixed capacity, so typing never reallocates and leaves password prefixes in freed memory.
  constexpr std::size_t kPasswordCapacity = 1024;

  void onSignal(int /*signal*/) { g_quit.store(true, std::memory_order_relaxed); }

  struct GreeterConfig {
    std::optional<std::string> wallpaper;
    std::vector<std::pair<std::string, std::string>> wallpapersByOutput;

    [[nodiscard]] std::optional<std::string> wallpaperFor(const std::string& connector) const {
      for (const auto& [name, path] : wallpapersByOutput) {
        if (name == connector) {
          return path;
        }
      }
      return wallpaper;
    }
  };

  GreeterConfig loadGreeterConfig() {
    GreeterConfig config;
    const char* dir = std::getenv("NOCTALIA_GREETER_STATE_DIR");
    const std::filesystem::path path = std::filesystem::path(dir != nullptr ? dir : kDefaultStateDir) / "sync.toml";
    toml::table table;
    try {
      table = toml::parse_file(path.string());
    } catch (const toml::parse_error& e) {
      kLog.info("no greeter appearance at {}: {}", path.string(), e.description());
      return config;
    }
    if (const auto p = table["appearance"]["wallpaper"]["path"].value<std::string>()) {
      config.wallpaper = *p;
    }
    if (const auto* outputs = table["appearance"]["wallpapers"].as_table()) {
      for (const auto& [connector, node] : *outputs) {
        if (const auto p = node.at_path("path").value<std::string>()) {
          config.wallpapersByOutput.emplace_back(std::string(connector.str()), *p);
        }
      }
    }
    return config;
  }

  // Exec= split per the Desktop Entry spec: double quotes group, a backslash escapes the next character, and field
  // codes (%f, %U...) drop out.
  std::vector<std::string> splitExec(const std::string& exec) {
    std::vector<std::string> words;
    std::string word;
    bool inWord = false;
    bool quoted = false;
    for (std::size_t i = 0; i < exec.size(); ++i) {
      const char c = exec[i];
      if (quoted) {
        if (c == '\\' && i + 1 < exec.size()) {
          word += exec[++i];
        } else if (c == '"') {
          quoted = false;
        } else {
          word += c;
        }
      } else if (c == '"') {
        quoted = true;
        inWord = true;
      } else if (c == ' ' || c == '\t') {
        if (inWord) {
          words.push_back(std::move(word));
          word.clear();
          inWord = false;
        }
      } else {
        word += c;
        inWord = true;
      }
    }
    if (inWord) {
      words.push_back(std::move(word));
    }
    std::erase_if(words, [](const std::string& w) { return w.size() == 2 && w[0] == '%'; });
    return words;
  }

  bool onPath(const std::string& program) {
    if (program.find('/') != std::string::npos) {
      return access(program.c_str(), X_OK) == 0;
    }
    const char* path = std::getenv("PATH");
    std::stringstream dirs(path != nullptr ? path : "/usr/local/bin:/usr/bin");
    for (std::string dir; std::getline(dirs, dir, ':');) {
      if (access((std::filesystem::path(dir) / program).c_str(), X_OK) == 0) {
        return true;
      }
    }
    return false;
  }

  // The session to start: andromeda.desktop when installed, else the first other wayland session by file name.
  std::optional<GreetdSessionCommand> findSession() {
    const char* env = std::getenv("XDG_DATA_DIRS");
    std::stringstream dirs(env != nullptr && env[0] != '\0' ? env : "/usr/local/share:/usr/share");
    std::vector<std::filesystem::path> entries;
    for (std::string dir; std::getline(dirs, dir, ':');) {
      std::error_code ec;
      for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(dir) / "wayland-sessions", ec)) {
        if (entry.path().extension() == ".desktop") {
          entries.push_back(entry.path());
        }
      }
    }
    std::ranges::sort(entries, {}, [](const auto& p) { return p.filename(); });
    std::ranges::stable_partition(entries, [](const auto& p) { return p.filename() == "andromeda.desktop"; });
    for (const auto& path : entries) {
      std::ifstream in(path);
      std::string exec;
      std::string tryExec;
      std::string desktopNames;
      bool hidden = false;
      bool inEntry = false;
      for (std::string line; std::getline(in, line);) {
        if (line.starts_with('[')) {
          inEntry = line == "[Desktop Entry]";
        } else if (!inEntry) {
          continue;
        } else if (line.starts_with("Exec=")) {
          exec = line.substr(5);
        } else if (line.starts_with("TryExec=")) {
          tryExec = line.substr(8);
        } else if (line.starts_with("DesktopNames=")) {
          desktopNames = line.substr(13);
        } else if (line == "Hidden=true" || line == "NoDisplay=true") {
          hidden = true;
        }
      }
      if (hidden || (!tryExec.empty() && !onPath(tryExec))) {
        continue;
      }
      auto words = splitExec(exec);
      if (words.empty()) {
        continue;
      }
      GreetdSessionCommand command;
      command.command = words.front();
      command.arguments.assign(words.begin() + 1, words.end());
      std::ranges::replace(desktopNames, ';', ':');
      while (desktopNames.ends_with(':')) {
        desktopNames.pop_back();
      }
      command.environment.push_back({"XDG_SESSION_TYPE", "wayland"});
      if (!desktopNames.empty()) {
        command.environment.push_back({"XDG_CURRENT_DESKTOP", desktopNames});
      }
      command.environment.push_back({"XDG_SESSION_DESKTOP", path.stem().string()});
      return command;
    }
    return std::nullopt;
  }

  std::size_t codepointCount(const std::string& text) {
    return static_cast<std::size_t>(std::ranges::count_if(text, [](char c) { return (c & 0xC0) != 0x80; }));
  }

  void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  // The shell's malloc_conf (main.cpp) keeps freed pages for seconds; the greeter allocates little after start, so it
  // returns them at once.
  void returnFreedPagesAtOnce() {
#ifdef NOCTALIA_USE_JEMALLOC
    ssize_t zero = 0;
    unsigned arenas = 0;
    std::size_t size = sizeof(arenas);
    (void)mallctl("arenas.dirty_decay_ms", nullptr, nullptr, &zero, sizeof(zero));
    (void)mallctl("arenas.muzzy_decay_ms", nullptr, nullptr, &zero, sizeof(zero));
    if (mallctl("arenas.narenas", &arenas, &size, nullptr, 0) == 0) {
      for (unsigned i = 0; i < arenas; ++i) {
        (void)mallctl(std::format("arena.{}.dirty_decay_ms", i).c_str(), nullptr, nullptr, &zero, sizeof(zero));
        (void)mallctl(std::format("arena.{}.muzzy_decay_ms", i).c_str(), nullptr, nullptr, &zero, sizeof(zero));
      }
    }
#endif
  }

  // Startup leaves freed pages in the allocator; give them back once the first frames are on screen.
  void trimAllocator() {
#ifdef NOCTALIA_USE_JEMALLOC
    const std::string purge = std::format("arena.{}.purge", MALLCTL_ARENAS_ALL);
    if (const int rc = mallctl(purge.c_str(), nullptr, nullptr, nullptr, 0); rc != 0) {
      kLog.warn("jemalloc purge failed: {}", std::strerror(rc));
    }
#else
    malloc_trim(0);
#endif
  }

  void wipe(std::string& secret) {
    sodium_memzero(secret.data(), secret.size());
    secret.clear();
  }

  struct OutputView {
    wl_output* output = nullptr;
    std::string connector;
    std::unique_ptr<LayerSurface> surface;
    AnimationManager animations;
    std::unique_ptr<Node> root;
    Image* wallpaper = nullptr;
    AuthColumn auth;
    bool built = false;
  };

  class GreeterHost {
  public:
    explicit GreeterHost(std::string user) : m_user(std::move(user)), m_config(loadGreeterConfig()) {
      m_password.reserve(kPasswordCapacity);
    }
    ~GreeterHost() { wipe(m_password); }

    bool initialize() {
      if (!m_wayland.connect()) {
        kLog.error("failed to connect to the Wayland compositor");
        return false;
      }
      m_gl.initialize(m_wayland.display());
      m_render.initialize(m_gl);
      m_wayland.setOutputChangeCallback([this] { syncOutputs(); });
      m_wayland.setKeyboardEventCallback([this](const KeyboardEvent& event) { onKey(event); });
      syncOutputs();
      if (const char* sock = std::getenv("GREETD_SOCK"); sock != nullptr && m_greetd.connect(sock)) {
        createSession();
      } else {
        kLog.warn("greetd is not reachable; login is off");
        m_hint = "Login is not available";
        m_hintError = true;
      }
      return true;
    }

    int run() {
      wl_display* display = m_wayland.display();
      std::string shownMinute = formatLocalTime("%H:%M");
      while (!g_quit.load(std::memory_order_relaxed)) {
        for (auto& fn : DeferredCall::takePending()) {
          if (fn) {
            fn();
          }
        }
        while (wl_display_prepare_read(display) != 0) {
          if (wl_display_dispatch_pending(display) < 0) {
            kLog.error("Wayland dispatch failed");
            return 1;
          }
        }
        if (wl_display_flush(display) < 0 && errno != EAGAIN) {
          wl_display_cancel_read(display);
          kLog.error("Wayland flush failed");
          return 1;
        }

        int timeoutMs = msToNextMinute();
        if (Surface::hasPendingFrameWork() || Surface::hasPendingRenders()) {
          timeoutMs = 0;
        }
        const int greetdTimeout = m_greetd.requestPollTimeoutMs();
        if (greetdTimeout >= 0 && (timeoutMs < 0 || greetdTimeout < timeoutMs)) {
          timeoutMs = greetdTimeout;
        }
        std::array<pollfd, 2> pfds{
            pollfd{.fd = wl_display_get_fd(display), .events = POLLIN, .revents = 0},
            pollfd{.fd = m_greetd.fd(), .events = POLLIN, .revents = 0},
        };
        pollfd& pfd = pfds[0];
        const nfds_t count = m_greetd.fd() >= 0 ? 2 : 1;
        if (poll(pfds.data(), count, timeoutMs) > 0 && (pfd.revents & POLLIN) != 0) {
          if (wl_display_read_events(display) < 0) {
            kLog.error("Wayland read failed");
            return 1;
          }
        } else {
          wl_display_cancel_read(display);
        }
        if (wl_display_dispatch_pending(display) < 0) {
          kLog.error("Wayland dispatch failed");
          return 1;
        }
        if ((pfd.revents & (POLLHUP | POLLERR)) != 0) {
          kLog.info("compositor closed the connection");
          return 0;
        }
        if (count == 2 && (pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
          onGreetdReadable();
        }
        if (const auto late = m_greetd.timedOutRequest()) {
          kLog.error("greetd did not answer {} in time", greetdRequestTypeName(late->request));
          m_greetd.disconnect();
          m_greetdLost = true;
        }
        if (m_greetdLost) {
          return 1;
        }
        if (m_sessionStarted) {
          kLog.info("session started; greeter exits");
          return 0;
        }

        if (const std::string minute = formatLocalTime("%H:%M"); minute != shownMinute) {
          shownMinute = minute;
          for (auto& view : m_views) {
            view->surface->requestLayout();
          }
        }
        Surface::drainPendingFrameWork();
        Surface::drainPendingRenders();
        if (!m_trimmed && !m_views.empty() && std::ranges::all_of(m_views, [](const auto& v) { return v->built; })
            && !Surface::hasPendingFrameWork() && !Surface::hasPendingRenders()) {
          m_trimmed = true;
          trimAllocator();
        }
      }
      return 0;
    }

  private:
    static int msToNextMinute() {
      const auto now = std::chrono::system_clock::now().time_since_epoch();
      const auto intoMinute = std::chrono::duration_cast<std::chrono::milliseconds>(now) % 60000;
      return static_cast<int>((std::chrono::milliseconds(60000) - intoMinute).count()) + 5;
    }

    static bool eligible(const WaylandOutput& output) {
      return output.done && output.output != nullptr && output.hasUsableGeometry();
    }

    void syncOutputs() {
      std::erase_if(m_views, [this](const std::unique_ptr<OutputView>& view) {
        return std::ranges::none_of(m_wayland.outputs(), [&](const WaylandOutput& o) {
          return eligible(o) && o.output == view->output;
        });
      });
      for (const auto& output : m_wayland.outputs()) {
        if (!eligible(output) || std::ranges::any_of(m_views, [&](const auto& v) { return v->output == output.output; })) {
          continue;
        }
        auto view = std::make_unique<OutputView>();
        view->output = output.output;
        view->connector = output.connectorName;
        view->surface = std::make_unique<LayerSurface>(
            m_wayland,
            LayerSurfaceConfig{
                .nameSpace = "noctalia-greeter",
                .layer = LayerShellLayer::Overlay,
                .anchor = LayerShellAnchor::Top | LayerShellAnchor::Bottom | LayerShellAnchor::Left
                    | LayerShellAnchor::Right,
                .width = 0,
                .height = 0,
                .exclusiveZone = -1,
                .keyboard = LayerShellKeyboard::Exclusive,
                .defaultWidth = 1920,
                .defaultHeight = 1080,
            }
        );
        OutputView* raw = view.get();
        raw->surface->setConfigureCallback([raw](std::uint32_t, std::uint32_t) { raw->surface->requestLayout(); });
        raw->surface->setPrepareFrameCallback([this, raw](bool needsUpdate, bool needsLayout) {
          prepareFrame(*raw, needsUpdate, needsLayout);
        });
        raw->surface->setAnimationManager(&raw->animations);
        raw->surface->setRenderContext(&m_render);
        if (!raw->surface->initialize(output.output)) {
          kLog.warn("no layer surface on {}", output.connectorName);
          continue;
        }
        m_views.push_back(std::move(view));
      }
    }

    void prepareFrame(OutputView& view, bool needsUpdate, bool needsLayout) {
      const auto width = view.surface->width();
      const auto height = view.surface->height();
      if (width == 0 || height == 0) {
        return;
      }
      m_render.makeCurrent(view.surface->renderTarget());
      Renderer& renderer = view.surface->renderTarget().renderer();
      const auto w = static_cast<float>(width);
      const auto h = static_cast<float>(height);
      UiPhaseScope layoutPhase(UiPhase::Layout);
      if (!view.built) {
        view.root = ui::node({});
        auto wallpaper = ui::image({});
        wallpaper->setFit(ImageFit::Cover);
        if (const auto path = m_config.wallpaperFor(view.connector)) {
          if (!wallpaper->setSourceFile(renderer, *path)) {
            kLog.warn("cannot load wallpaper {}", *path);
          }
        }
        view.wallpaper = static_cast<Image*>(view.root->addChild(std::move(wallpaper)));
        view.auth.build(*view.root, view.animations);
        view.surface->setSceneRoot(view.root.get());
        view.built = true;
        needsLayout = true;
      }
      if (!needsLayout && !needsUpdate) {
        return;
      }
      view.root->setSize(w, h);
      view.wallpaper->setPosition(0.0F, 0.0F);
      view.wallpaper->setSize(w, h);
      AuthColumn::State state;
      state.user = m_user;
      state.placeholder = "Password";
      state.passwordLength = codepointCount(m_password);
      state.trailing = m_phase == Phase::Posting || m_phase == Phase::Starting ? AuthColumn::Trailing::Checking
          : m_password.empty()                                                 ? AuthColumn::Trailing::None
                                                                               : AuthColumn::Trailing::Go;
      state.inputOff = !m_greetd.isConnected();
      state.hint = m_hint;
      state.hintError = m_hintError;
      state.time = formatLocalTime("%H:%M");
      state.date = formatLocalTime("%A, %-d %B");
      view.auth.setState(std::move(state));
      view.auth.layout(renderer, w, h, 1.0F);
    }

    // greetd is strictly one request, one reply: Creating -> (Waiting) -> Posting -> Starting, or Cancelling -> Creating
    // after a wrong password.
    enum class Phase : std::uint8_t { Idle, Creating, Waiting, Posting, Starting, Cancelling };

    void createSession() {
      if (m_user.empty()) {
        setHint("No user selected", true);
        m_phase = Phase::Idle;
        return;
      }
      m_phase = m_greetd.requestCreateSession(m_user) ? Phase::Creating : Phase::Idle;
    }

    void setHint(std::string hint, bool error) {
      m_hint = std::move(hint);
      m_hintError = error;
      relayout();
    }

    void relayout() {
      for (auto& view : m_views) {
        view->surface->requestLayout();
      }
    }

    void submit() {
      if (m_password.empty() || !m_greetd.isConnected()) {
        return;
      }
      if (m_phase == Phase::Waiting) {
        postPassword();
      } else {
        m_submitQueued = true;
      }
    }

    void postPassword() {
      m_submitQueued = false;
      if (m_password.empty()) {
        return;
      }
      const bool sent = m_greetd.requestPostAuthData(m_password);
      wipe(m_password);
      m_phase = sent ? Phase::Posting : Phase::Idle;
      m_hint.clear();
      relayout();
    }

    void onKey(const KeyboardEvent& event) {
      if (!event.pressed || event.preedit || m_phase == Phase::Posting || m_phase == Phase::Starting) {
        return;
      }
      if (event.sym == XKB_KEY_Return || event.sym == XKB_KEY_KP_Enter) {
        submit();
      } else if (event.sym == XKB_KEY_BackSpace) {
        while (!m_password.empty() && (m_password.back() & 0xC0) == 0x80) {
          m_password.pop_back();
        }
        if (!m_password.empty()) {
          m_password.pop_back();
        }
      } else if (event.sym == XKB_KEY_Escape) {
        wipe(m_password);
        m_submitQueued = false;
      } else if (event.utf32 >= 0x20 && event.utf32 != 0x7F && m_password.size() + 4 <= kPasswordCapacity) {
        appendUtf8(m_password, event.utf32);
      } else {
        return;
      }
      relayout();
    }

    void onGreetdReadable() {
      while (auto response = m_greetd.readMessage()) {
        onGreetdResponse(*response);
      }
      // A closed socket or a frame that does not parse leaves the connection unusable. The greeter exits: the
      // compositor ends the greeter session and greetd starts a new one.
      if (const auto& error = m_greetd.lastError(); error.has_value()) {
        kLog.error("greetd connection lost: {}", error->description);
        m_greetd.disconnect();
        m_greetdLost = true;
      }
    }

    void onGreetdResponse(const GreetdResponse& response) {
      switch (response.type) {
      case GreetdResponseType::AuthMessage:
        if (response.authMessage.type == GreetdAuthMessageType::Secret
            || response.authMessage.type == GreetdAuthMessageType::Visible) {
          m_phase = Phase::Waiting;
          if (m_submitQueued) {
            postPassword();
          }
        } else {
          // Info and error lines are shown and acknowledged with an empty response, as greetd requires.
          if (!response.authMessage.message.empty()) {
            setHint(response.authMessage.message, response.authMessage.type == GreetdAuthMessageType::Error);
          }
          (void)m_greetd.requestPostAuthData({});
        }
        break;
      case GreetdResponseType::Success:
        if (m_phase == Phase::Cancelling) {
          createSession();
        } else if (m_phase == Phase::Starting) {
          m_sessionStarted = true;
        } else if (const auto session = findSession()) {
          m_phase = m_greetd.requestStartSession(*session) ? Phase::Starting : Phase::Idle;
          relayout();
        } else {
          kLog.error("no wayland session is installed");
          setHint("No session is installed", true);
          (void)m_greetd.requestCancelSession();
          m_phase = Phase::Cancelling;
        }
        break;
      case GreetdResponseType::Error:
        if (response.error.type == GreetdErrorType::AuthError) {
          setHint("Wrong password", true);
          for (auto& view : m_views) {
            view->auth.playRejected();
          }
        } else {
          setHint(response.error.description, true);
        }
        // A failed create_session (unknown user) is not retried: that would loop as fast as greetd answers.
        if (m_phase == Phase::Creating || m_phase == Phase::Cancelling) {
          kLog.error("greetd refused a session for '{}': {}", m_user, response.error.description);
          m_phase = Phase::Idle;
        } else {
          m_phase = m_greetd.requestCancelSession() ? Phase::Cancelling : Phase::Idle;
        }
        break;
      }
    }

    std::string m_user;
    GreeterConfig m_config;
    GreetdClient m_greetd;
    Phase m_phase = Phase::Idle;
    std::string m_password;
    std::string m_hint;
    bool m_hintError = false;
    bool m_submitQueued = false;
    bool m_sessionStarted = false;
    bool m_trimmed = false;
    bool m_greetdLost = false;
    WaylandConnection m_wayland;
    GlSharedContext m_gl;
    RenderContext m_render;
    std::vector<std::unique_ptr<OutputView>> m_views;
  };

} // namespace

int runGreeterHost(int argc, char* argv[]) {
  std::string user;
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--user") == 0 && i + 1 < argc) {
      user = argv[++i];
    } else {
      std::println(stderr, "usage: noctalia greeter [--user NAME]");
      return 2;
    }
  }
  returnFreedPagesAtOnce();
  std::signal(SIGTERM, onSignal);
  std::signal(SIGINT, onSignal);
  kLog.info("greeter mode start");
  GreeterHost host(std::move(user));
  if (!host.initialize()) {
    return 1;
  }
  return host.run();
}
