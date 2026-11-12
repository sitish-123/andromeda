#include "provider/provider.h"

#include "core/log.h"
#include "server/server.h"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-server-core.h>

namespace andromeda {

  namespace {
    constexpr Logger kLog("provider");
    constexpr size_t kMaxLineSize = 65536;
    // A client that stops reading is dropped rather than buffered without bound; the broker reconnects and gets a
    // fresh snapshot.
    constexpr size_t kMaxBacklog = 4 * 1024 * 1024;
    constexpr std::array kFamilies = {Provider::Family::Windows, Provider::Family::Workspaces, Provider::Family::Outputs};
    constexpr std::array<std::string_view, 3> kFamilyNames = {"windows", "workspaces", "outputs"};
    constexpr std::array<std::string_view, 3> kEventNames = {"window.changed", "workspace.changed", "output.changed"};
    constexpr std::string_view kCallExample = R"({"op":"call","id":"a1","action":"window.list","args":{}})";

    size_t familyIndex(Provider::Family family) { return static_cast<size_t>(family); }
  } // namespace

  Provider::Provider(Server& server, const std::string& waylandSocketName) : m_server(&server), m_context(server) {
    const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
    if (runtimeDir == nullptr || runtimeDir[0] == '\0') {
      kLog.error("XDG_RUNTIME_DIR not set, provider socket disabled");
      return;
    }
    m_socketPath = std::string(runtimeDir) + "/andromeda-provider-" + waylandSocketName + ".sock";
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (m_socketPath.size() >= sizeof(addr.sun_path)) {
      kLog.error("provider socket path too long");
      m_socketPath.clear();
      return;
    }
    m_socketPath.copy(addr.sun_path, m_socketPath.size());
    m_listenFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (m_listenFd < 0) {
      kLog.error("failed to create provider socket: {}", strerror(errno));
      m_socketPath.clear();
      return;
    }
    unlink(m_socketPath.c_str());
    if (bind(m_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0
        || chmod(m_socketPath.c_str(), S_IRUSR | S_IWUSR) < 0
        || listen(m_listenFd, 4) < 0) {
      kLog.error("failed to set up provider socket: {}", strerror(errno));
      close(m_listenFd);
      m_listenFd = -1;
      unlink(m_socketPath.c_str());
      m_socketPath.clear();
      return;
    }
    m_listenSource = wl_event_loop_add_fd(
        wl_display_get_event_loop(server.display()), m_listenFd, WL_EVENT_READABLE, onListenReadable, this
    );
    if (m_listenSource == nullptr) {
      kLog.error("failed to register provider socket");
      close(m_listenFd);
      m_listenFd = -1;
      unlink(m_socketPath.c_str());
      m_socketPath.clear();
      return;
    }
    kLog.info("provider listening on {}", m_socketPath);
  }

  Provider::~Provider() {
    for (const auto& connection : m_connections) {
      connection->dead = true;
    }
    reap();
    if (m_listenSource != nullptr) {
      wl_event_source_remove(m_listenSource);
    }
    if (m_listenFd >= 0) {
      close(m_listenFd);
    }
    if (!m_socketPath.empty()) {
      unlink(m_socketPath.c_str());
    }
  }

  int Provider::onListenReadable(int /*fd*/, uint32_t /*mask*/, void* data) {
    static_cast<Provider*>(data)->acceptConnections();
    return 0;
  }

  void Provider::acceptConnections() {
    while (true) {
      const int fd = accept4(m_listenFd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
      if (fd < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
          kLog.warn("failed to accept a provider connection: {}", strerror(errno));
        }
        break;
      }
      ucred peer{};
      socklen_t size = sizeof(peer);
      if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 || peer.uid != geteuid()) {
        kLog.warn("refused a provider connection from uid {}", peer.uid);
        close(fd);
        continue;
      }
      // Clients already connected get what changed since the last idle notification before the newcomer's snapshot
      // is taken from the same state.
      for (const Family family : kFamilies) {
        refresh(family, nullptr);
      }
      auto connection = std::make_unique<Connection>();
      connection->owner = this;
      connection->fd = fd;
      connection->source = wl_event_loop_add_fd(
          wl_display_get_event_loop(m_server->display()), fd, WL_EVENT_READABLE, onConnectionEvent, connection.get()
      );
      if (connection->source == nullptr) {
        kLog.warn("failed to register a provider connection");
        close(fd);
        continue;
      }
      Connection& added = *m_connections.emplace_back(std::move(connection));
      send(added, {
          {"op", "hello"},
          {"provider", idl::kModule},
          {"schema_version", idl::kSchemaVersion},
          {"manifest_hash", idl::kManifestHash},
          {"actions", nlohmann::json::parse(idl::kHelpActions)},
      });
      send(added, {{"op", "snapshot"}, {"seq", 0}, {"state", m_state}, {"schema_version", idl::kSchemaVersion}});
    }
    reap();
  }

  int Provider::onConnectionEvent(int /*fd*/, uint32_t mask, void* data) {
    auto* connection = static_cast<Connection*>(data);
    Provider* owner = connection->owner;
    if ((mask & WL_EVENT_ERROR) != 0) {
      connection->dead = true;
    }
    if (!connection->dead && (mask & WL_EVENT_WRITABLE) != 0) {
      owner->flush(*connection);
    }
    if (!connection->dead && (mask & (WL_EVENT_READABLE | WL_EVENT_HANGUP)) != 0) {
      owner->readLines(*connection);
    }
    owner->reap();
    return 0;
  }

  void Provider::readLines(Connection& connection) {
    char chunk[4096];
    while (!connection.dead) {
      const ssize_t size = recv(connection.fd, chunk, sizeof(chunk), 0);
      if (size < 0 && errno == EINTR) {
        continue;
      }
      if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return;
      }
      if (size <= 0) {
        connection.dead = true;
        return;
      }
      connection.input.append(chunk, static_cast<size_t>(size));
      size_t newline = 0;
      while (!connection.dead && (newline = connection.input.find('\n')) != std::string::npos) {
        const std::string line = connection.input.substr(0, newline);
        connection.input.erase(0, newline + 1);
        handleLine(connection, line);
      }
      if (connection.input.size() > kMaxLineSize) {
        kLog.warn("provider client sent a line over {} bytes, disconnecting", kMaxLineSize);
        connection.dead = true;
      }
    }
  }

  void Provider::handleLine(Connection& connection, std::string_view line) {
    const nlohmann::json request = nlohmann::json::parse(line, nullptr, false);
    nlohmann::json id = nullptr;
    const auto fail = [&](idl::ErrorCode code, std::string message, std::string hint, std::string_view example) {
      send(connection, {
          {"op", "result"},
          {"id", id},
          {"ok", false},
          {"error", idl::write(idl::Error{code, std::move(message), std::move(hint), std::string(example)})},
          {"schema_version", idl::kSchemaVersion},
      });
    };
    if (request.is_discarded() || !request.is_object()) {
      fail(idl::ErrorCode::InvalidArgument, "a request is one JSON object per line", "", kCallExample);
      return;
    }
    if (const auto field = request.find("id"); field != request.end() && field->is_string()) {
      id = *field;
    }
    const auto op = request.find("op");
    if (op == request.end() || *op != "call") {
      fail(idl::ErrorCode::InvalidArgument, "unknown op", "the provider takes op \"call\"", kCallExample);
      return;
    }
    const auto action = request.find("action");
    if (!id.is_string() || action == request.end() || !action->is_string()) {
      fail(idl::ErrorCode::InvalidArgument, "a call needs a string id and action", "", kCallExample);
      return;
    }
    const std::string& name = action->get_ref<const std::string&>();
    const auto entry = std::ranges::find(idl::kActions, std::string_view(name), &idl::ActionEntry::name);
    if (entry == idl::kActions.end()) {
      std::vector<std::string> names;
      for (const idl::ActionEntry& known : idl::kActions) {
        names.emplace_back(known.name);
      }
      fail(
          idl::ErrorCode::NotFound, "unknown action " + name, "closest actions: " + closestMatches(name, names),
          "andromeda help --json"
      );
      return;
    }
    const auto args = request.find("args");
    const nlohmann::json noArgs = nlohmann::json::object();
    // Changes from before this call reach the clients without its cause.
    for (const Family family : kFamilies) {
      refresh(family, nullptr);
    }
    const idl::Outcome outcome = entry->invoke(m_context, args == request.end() || args->is_null() ? noArgs : *args);
    nlohmann::json changed = nlohmann::json::object();
    for (const Family family : kFamilies) {
      if (refresh(family, id)) {
        const std::string key(kFamilyNames[familyIndex(family)]);
        changed[key] = m_state[key];
      }
    }
    if (!outcome) {
      send(connection, {
          {"op", "result"},
          {"id", id},
          {"ok", false},
          {"error", idl::write(outcome.error())},
          {"schema_version", idl::kSchemaVersion},
      });
      return;
    }
    send(connection, {
        {"op", "result"},
        {"id", id},
        {"ok", true},
        {"value", *outcome},
        {"changed", std::move(changed)},
        {"schema_version", idl::kSchemaVersion},
    });
  }

  void Provider::notifyChanged(Family family) {
    if (m_connections.empty()) {
      return;
    }
    refresh(family, nullptr);
    reap();
  }

  nlohmann::json Provider::collect(Family family) {
    switch (family) {
    case Family::Windows:
      return idl::write(m_context.windows());
    case Family::Workspaces:
      return idl::write(m_context.workspaces());
    case Family::Outputs:
      return idl::write(m_context.outputs());
    }
    return nlohmann::json::array();
  }

  bool Provider::refresh(Family family, const nlohmann::json& cause) {
    const std::string key(kFamilyNames[familyIndex(family)]);
    nlohmann::json next = collect(family);
    if (const auto previous = m_state.find(key); previous != m_state.end() && *previous == next) {
      return false;
    }
    m_state[key] = std::move(next);
    const nlohmann::json delta{{key, m_state[key]}};
    for (const auto& connection : m_connections) {
      send(*connection, {
          {"op", "event"},
          {"seq", ++connection->seq},
          {"name", kEventNames[familyIndex(family)]},
          {"delta", delta},
          {"cause", cause},
          {"schema_version", idl::kSchemaVersion},
      });
    }
    return true;
  }

  void Provider::send(Connection& connection, const nlohmann::json& message) {
    if (connection.dead) {
      return;
    }
    // Window titles come from clients and need not be valid UTF-8.
    std::string line = message.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    line += '\n';
    if (connection.output.size() - connection.writeOffset + line.size() > kMaxBacklog) {
      kLog.warn("provider client fd {} is not reading, disconnecting", connection.fd);
      connection.dead = true;
      return;
    }
    connection.output += line;
    flush(connection);
  }

  void Provider::flush(Connection& connection) {
    while (connection.writeOffset < connection.output.size()) {
      const ssize_t size = ::send(
          connection.fd, connection.output.data() + connection.writeOffset,
          connection.output.size() - connection.writeOffset, MSG_NOSIGNAL
      );
      if (size > 0) {
        connection.writeOffset += static_cast<size_t>(size);
        continue;
      }
      if (size < 0 && errno == EINTR) {
        continue;
      }
      if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        if (!connection.wantsWrite) {
          connection.wantsWrite = true;
          wl_event_source_fd_update(connection.source, WL_EVENT_READABLE | WL_EVENT_WRITABLE);
        }
        return;
      }
      connection.dead = true;
      return;
    }
    connection.output.clear();
    connection.writeOffset = 0;
    if (connection.wantsWrite) {
      connection.wantsWrite = false;
      wl_event_source_fd_update(connection.source, WL_EVENT_READABLE);
    }
  }

  void Provider::reap() {
    std::erase_if(m_connections, [](const std::unique_ptr<Connection>& connection) {
      if (!connection->dead) {
        return false;
      }
      wl_event_source_remove(connection->source);
      close(connection->fd);
      return true;
    });
  }

} // namespace andromeda
