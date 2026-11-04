#include "greeter/greeter_host.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "greeter/greetd_client.h"
#include "render/gl_shared_context.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "time/time_format.h"
#include "ui/auth/auth_chrome.h"
#include "ui/auth/auth_column.h"
#include "ui/builders.h"
#include "ui/controls/image.h"
#include "cursor-shape-v1-client-protocol.h"
#include "wayland/layer_surface.h"
#include "wayland/surface.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <malloc.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <print>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <format>
#include <gio/gio.h>
#include <linux/input-event-codes.h>
#include <pwd.h>
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

  std::filesystem::path stateDir() {
    const char* dir = std::getenv("NOCTALIA_GREETER_STATE_DIR");
    return dir != nullptr && dir[0] != '\0' ? dir : kDefaultStateDir;
  }

  std::string readState(const char* name) {
    std::ifstream in(stateDir() / name);
    std::string value;
    std::getline(in, value);
    return value;
  }

  void writeState(const char* name, const std::string& value) {
    std::ofstream out(stateDir() / name, std::ios::trunc);
    out << value << '\n';
    if (!out) {
      kLog.warn("cannot write {}", (stateDir() / name).string());
    }
  }

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
    const std::filesystem::path path = stateDir() / "sync.toml";
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

  struct SessionEntry {
    std::string id;
    std::string name;
    GreetdSessionCommand command;
  };

  // Installed wayland sessions, andromeda first, then by file name; a file name found twice keeps the first
  // XDG_DATA_DIRS entry.
  std::vector<SessionEntry> listSessions() {
    const char* env = std::getenv("XDG_DATA_DIRS");
    std::stringstream dirs(env != nullptr && env[0] != '\0' ? env : "/usr/local/share:/usr/share");
    std::vector<std::filesystem::path> entries;
    for (std::string dir; std::getline(dirs, dir, ':');) {
      std::error_code ec;
      for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(dir) / "wayland-sessions", ec)) {
        if (entry.path().extension() == ".desktop"
            && std::ranges::none_of(entries, [&](const auto& p) { return p.filename() == entry.path().filename(); })) {
          entries.push_back(entry.path());
        }
      }
    }
    std::ranges::sort(entries, {}, [](const auto& p) { return p.filename(); });
    std::ranges::stable_partition(entries, [](const auto& p) { return p.filename() == "andromeda.desktop"; });
    std::vector<SessionEntry> sessions;
    for (const auto& path : entries) {
      std::ifstream in(path);
      std::string exec;
      std::string tryExec;
      std::string desktopNames;
      std::string name;
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
        } else if (line.starts_with("Name=")) {
          name = line.substr(5);
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
      SessionEntry session;
      session.id = path.stem().string();
      session.name = name.empty() ? session.id : name;
      session.command.command = words.front();
      session.command.arguments.assign(words.begin() + 1, words.end());
      std::ranges::replace(desktopNames, ';', ':');
      while (desktopNames.ends_with(':')) {
        desktopNames.pop_back();
      }
      session.command.environment.push_back({"XDG_SESSION_TYPE", "wayland"});
      if (!desktopNames.empty()) {
        session.command.environment.push_back({"XDG_CURRENT_DESKTOP", desktopNames});
      }
      session.command.environment.push_back({"XDG_SESSION_DESKTOP", session.id});
      sessions.push_back(std::move(session));
    }
    return sessions;
  }

  struct UserEntry {
    std::string login;
    std::string display;
  };

  // Regular accounts: UID 1000-59999 with a login shell.
  std::vector<UserEntry> listUsers() {
    std::vector<UserEntry> users;
    setpwent();
    while (const passwd* pw = getpwent()) {
      const std::string_view shell = pw->pw_shell != nullptr ? pw->pw_shell : "";
      if (pw->pw_uid < 1000 || pw->pw_uid >= 60000 || shell.ends_with("nologin") || shell.ends_with("false")) {
        continue;
      }
      std::string gecos = pw->pw_gecos != nullptr ? pw->pw_gecos : "";
      gecos = gecos.substr(0, gecos.find(','));
      users.push_back({pw->pw_name, gecos.empty() ? pw->pw_name : gecos});
    }
    endpwent();
    std::ranges::sort(users, {}, &UserEntry::login);
    return users;
  }

  GDBusConnection* systemBus() {
    static GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
    return bus;
  }

  // Short timeouts: these run on the UI thread, and a missing service must not stall the greeter.
  GVariant* properties(const char* dest, const char* path, const char* iface) {
    if (systemBus() == nullptr) {
      return nullptr;
    }
    GVariant* reply = g_dbus_connection_call_sync(
        systemBus(), dest, path, "org.freedesktop.DBus.Properties", "GetAll", g_variant_new("(s)", iface),
        G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 300, nullptr, nullptr
    );
    if (reply == nullptr) {
      return nullptr;
    }
    GVariant* dict = g_variant_get_child_value(reply, 0);
    g_variant_unref(reply);
    return dict;
  }

  const char* batteryGlyph(double percentage, std::uint32_t state) {
    // UPower states: 1 charging, 4 fully charged, 5 pending charge.
    if (state == 1) {
      return "battery-charging";
    }
    if (state == 4 || state == 5) {
      return "battery-plugged";
    }
    return percentage >= 85.0 ? "battery-4"
        : percentage >= 55.0  ? "battery-3"
        : percentage >= 30.0  ? "battery-2"
        : percentage >= 10.0  ? "battery-1"
                              : "battery";
  }

  AuthColumn::Status readStatus() {
    AuthColumn::Status status;
    if (GVariant* dict = properties(
            "org.freedesktop.UPower", "/org/freedesktop/UPower/devices/DisplayDevice", "org.freedesktop.UPower.Device"
        )) {
      gboolean present = FALSE;
      gdouble percentage = 0.0;
      guint32 state = 0;
      g_variant_lookup(dict, "IsPresent", "b", &present);
      g_variant_lookup(dict, "Percentage", "d", &percentage);
      g_variant_lookup(dict, "State", "u", &state);
      if (present != FALSE) {
        status.batteryGlyph = batteryGlyph(percentage, state);
        status.batteryText = std::format("{}%", static_cast<int>(std::lround(percentage)));
      }
      g_variant_unref(dict);
    }
    if (GVariant* dict =
            properties("org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager", "org.freedesktop.NetworkManager")) {
      const gchar* type = nullptr;
      guint32 state = 0;
      g_variant_lookup(dict, "PrimaryConnectionType", "&s", &type);
      g_variant_lookup(dict, "State", "u", &state);
      // NM_STATE_CONNECTED_SITE = 60 and above.
      const std::string_view kind = type != nullptr ? type : "";
      status.networkGlyph = state < 60 ? "wifi-off" : kind == "802-11-wireless" ? "wifi" : "network";
      g_variant_unref(dict);
    }
    return status;
  }

  // logind with interactive = false: its polkit rules decide; the greeter never runs a command for this.
  void logindCall(const char* method) {
    if (systemBus() == nullptr) {
      kLog.error("no system bus; cannot {}", method);
      return;
    }
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        systemBus(), "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager", method,
        g_variant_new("(b)", FALSE), nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &error
    );
    if (reply != nullptr) {
      g_variant_unref(reply);
    } else {
      kLog.error("logind {} failed: {}", method, error != nullptr ? error->message : "unknown error");
      g_clear_error(&error);
    }
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

  // Startup leaves freed pages in the allocator; give them back once the first frames are on screen.
  void trimAllocator() { malloc_trim(0); }

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
    AuthChrome chrome;
    bool built = false;
  };

  class GreeterHost {
  public:
    explicit GreeterHost(std::string user)
        : m_user(std::move(user)), m_config(loadGreeterConfig()), m_users(listUsers()), m_sessions(listSessions()) {
      m_password.reserve(kPasswordCapacity);
      const auto known = [this](const std::string& login) {
        return std::ranges::any_of(m_users, [&](const UserEntry& u) { return u.login == login; });
      };
      if (m_user.empty()) {
        const std::string last = readState("last-user");
        m_user = known(last) ? last : m_users.empty() ? std::string{} : m_users.front().login;
      }
      const std::string lastSession = readState("last-session");
      for (std::size_t i = 0; i < m_sessions.size(); ++i) {
        if (m_sessions[i].id == lastSession) {
          m_sessionIndex = i;
        }
      }
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
      m_wayland.setPointerEventCallback([this](const PointerEvent& event) { onPointer(event); });
      m_wayland.setKeyboardLayoutChangeCallback([this] { relayout(); });
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
        if (m_trimmed) {
          const auto untilStatus = std::chrono::duration_cast<std::chrono::milliseconds>(
              m_nextStatus - std::chrono::steady_clock::now()
          );
          timeoutMs = std::min(timeoutMs, static_cast<int>(std::max<std::int64_t>(0, untilStatus.count())));
        }
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
        if (m_exiting && (m_exitsDone >= m_views.size() || std::chrono::steady_clock::now() >= m_exitDeadline)) {
          m_sessionStarted = true;
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
          m_nextStatus = std::chrono::steady_clock::now();
        }
        // UPower and NetworkManager are read after the first frame and then every 30 s, never before it.
        if (m_trimmed && std::chrono::steady_clock::now() >= m_nextStatus) {
          m_nextStatus = std::chrono::steady_clock::now() + std::chrono::seconds(30);
          if (auto status = readStatus(); status != m_status) {
            m_status = std::move(status);
            relayout();
          }
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
        const bool gone = std::ranges::none_of(m_wayland.outputs(), [&](const WaylandOutput& o) {
          return eligible(o) && o.output == view->output;
        });
        if (gone && (view.get() == m_menuView || view.get() == m_pointerView)) {
          m_menuView = nullptr;
          m_pointerView = nullptr;
          m_menu = AuthChrome::Menu::None;
        }
        return gone;
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
        view.chrome.build(*view.root);
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
      state.user = displayName(m_user);
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
      state.status = m_status;
      if (m_wayland.keyboardLayoutNames().size() > 1) {
        state.status.layoutLabel = m_wayland.currentKeyboardLayoutName();
      }
      view.auth.setState(std::move(state));
      view.auth.layout(renderer, w, h, 1.0F);
      view.chrome.setState(chromeState(view));
      view.chrome.layout(renderer, w, h, 1.0F);
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
      } else if (event.sym == XKB_KEY_Escape && m_menu != AuthChrome::Menu::None) {
        m_menu = AuthChrome::Menu::None;
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

    [[nodiscard]] std::string displayName(const std::string& login) const {
      for (const auto& user : m_users) {
        if (user.login == login) {
          return user.display;
        }
      }
      return login;
    }

    [[nodiscard]] static std::string initialOf(const std::string& name) {
      if (name.empty()) {
        return {};
      }
      const auto lead = static_cast<unsigned char>(name[0]);
      std::string out = name.substr(0, lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4);
      if (out.size() == 1 && out[0] >= 'a' && out[0] <= 'z') {
        out[0] = static_cast<char>(out[0] - 'a' + 'A');
      }
      return out;
    }

    // Menu entries; the host acts on the index in pickItem().
    [[nodiscard]] AuthChrome::State chromeState(const OutputView& view) const {
      AuthChrome::State state;
      state.usersChip = m_users.size() > 1;
      if (m_sessions.size() > 1 && m_sessionIndex < m_sessions.size()) {
        state.sessionLabel = m_sessions[m_sessionIndex].name;
      }
      if (&view != m_menuView) {
        return state;
      }
      state.menu = m_menu;
      state.hover = m_hover;
      if (m_menu == AuthChrome::Menu::Power) {
        state.items = {{"moon", "Sleep", {}}, {"refresh", "Restart", {}}, {"power", "Shut down", {}}};
      } else if (m_menu == AuthChrome::Menu::Session) {
        for (const auto& session : m_sessions) {
          state.items.push_back({{}, session.name, {}});
        }
        state.current = static_cast<int>(m_sessionIndex);
      } else if (m_menu == AuthChrome::Menu::Users) {
        for (std::size_t i = 0; i < m_users.size(); ++i) {
          state.items.push_back({{}, m_users[i].display, initialOf(m_users[i].display)});
          if (m_users[i].login == m_user) {
            state.current = static_cast<int>(i);
          }
        }
      }
      return state;
    }

    OutputView* viewFor(wl_surface* surface) {
      for (auto& view : m_views) {
        if (view->surface->wlSurface() == surface) {
          return view.get();
        }
      }
      return nullptr;
    }

    void onPointer(const PointerEvent& event) {
      if (event.type == PointerEvent::Type::Enter) {
        // A client sets its cursor on every enter; without it the compositor draws none over the greeter (G0).
        m_wayland.setCursorShape(event.serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
      }
      if (event.type == PointerEvent::Type::Enter || event.type == PointerEvent::Type::Motion) {
        if (event.surface != nullptr) {
          m_pointerView = viewFor(event.surface);
        }
        m_pointerX = static_cast<float>(event.sx);
        m_pointerY = static_cast<float>(event.sy);
        if (m_pointerView != nullptr && m_pointerView == m_menuView) {
          const auto hit = m_pointerView->chrome.hitTest(m_pointerX, m_pointerY);
          const int hover = hit.target == AuthChrome::Target::Item ? hit.index : -1;
          if (hover != m_hover) {
            m_hover = hover;
            relayout();
          }
        }
        return;
      }
      if (event.type != PointerEvent::Type::Button || !event.pressed || event.button != BTN_LEFT
          || m_pointerView == nullptr) {
        return;
      }
      const auto hit = m_pointerView->chrome.hitTest(m_pointerX, m_pointerY);
      const auto toggle = [this](AuthChrome::Menu menu) {
        const bool same = m_menu == menu && m_menuView == m_pointerView;
        m_menu = same ? AuthChrome::Menu::None : menu;
        m_menuView = same ? nullptr : m_pointerView;
        m_hover = -1;
      };
      switch (hit.target) {
      case AuthChrome::Target::Power:
        toggle(AuthChrome::Menu::Power);
        break;
      case AuthChrome::Target::Session:
        toggle(AuthChrome::Menu::Session);
        break;
      case AuthChrome::Target::Users:
        toggle(AuthChrome::Menu::Users);
        break;
      case AuthChrome::Target::Item:
        pickItem(hit.index);
        break;
      case AuthChrome::Target::Menu:
        return;
      case AuthChrome::Target::None:
        m_menu = AuthChrome::Menu::None;
        m_menuView = nullptr;
        break;
      }
      relayout();
    }

    void pickItem(int index) {
      const auto menu = m_menu;
      m_menu = AuthChrome::Menu::None;
      m_menuView = nullptr;
      m_hover = -1;
      if (index < 0) {
        return;
      }
      const auto i = static_cast<std::size_t>(index);
      if (menu == AuthChrome::Menu::Power) {
        static constexpr std::array<const char*, 3> kMethods{"Suspend", "Reboot", "PowerOff"};
        if (i < kMethods.size()) {
          logindCall(kMethods[i]);
        }
      } else if (menu == AuthChrome::Menu::Session && i < m_sessions.size()) {
        m_sessionIndex = i;
      } else if (menu == AuthChrome::Menu::Users && i < m_users.size() && m_users[i].login != m_user) {
        switchUser(m_users[i].login);
      }
    }

    // A new user needs a new PAM conversation: cancel the open one; its reply starts the next (Cancelling).
    void switchUser(const std::string& login) {
      if (m_phase == Phase::Posting || m_phase == Phase::Starting) {
        return;
      }
      m_user = login;
      wipe(m_password);
      m_submitQueued = false;
      m_hint.clear();
      m_hintError = false;
      if (m_phase == Phase::Creating || m_phase == Phase::Waiting) {
        m_phase = m_greetd.requestCancelSession() ? Phase::Cancelling : Phase::Idle;
      } else if (m_phase == Phase::Idle && m_greetd.isConnected()) {
        createSession();
      }
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
          beginExit();
        } else if (m_sessionIndex < m_sessions.size()) {
          writeState("last-user", m_user);
          writeState("last-session", m_sessions[m_sessionIndex].id);
          m_phase = m_greetd.requestStartSession(m_sessions[m_sessionIndex].command) ? Phase::Starting : Phase::Idle;
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
    std::vector<UserEntry> m_users;
    std::vector<SessionEntry> m_sessions;
    std::size_t m_sessionIndex = 0;
    AuthColumn::Status m_status;
    std::chrono::steady_clock::time_point m_nextStatus{};
    AuthChrome::Menu m_menu = AuthChrome::Menu::None;
    OutputView* m_menuView = nullptr;
    OutputView* m_pointerView = nullptr;
    float m_pointerX = 0.0F;
    float m_pointerY = 0.0F;
    int m_hover = -1;
    GreetdClient m_greetd;
    Phase m_phase = Phase::Idle;
    std::string m_password;
    std::string m_hint;
    bool m_hintError = false;
    bool m_submitQueued = false;
    bool m_sessionStarted = false;
    // Login accepted: the auth column Returns, then the greeter exits and leaves its last frame on screen for the
    // session compositor's held frame (.agent/DESIGN.md "Login transition design").
    bool m_exiting = false;
    std::size_t m_exitsDone = 0;
    std::chrono::steady_clock::time_point m_exitDeadline{};
    void beginExit() {
      if (m_exiting) {
        return;
      }
      m_exiting = true;
      m_exitDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
      for (auto& view : m_views) {
        view->auth.playAccepted(&view->chrome.layer(), [this] { ++m_exitsDone; });
        view->surface->requestRedraw();
      }
    }
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
  std::signal(SIGTERM, onSignal);
  std::signal(SIGINT, onSignal);
  kLog.info("greeter mode start");
  GreeterHost host(std::move(user));
  if (!host.initialize()) {
    return 1;
  }
  return host.run();
}
