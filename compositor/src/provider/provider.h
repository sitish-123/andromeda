#pragma once

#include "compositor_actions.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct wl_event_source;

namespace andromeda {

  class Output;
  class Server;
  class View;
  class Workspace;

  // What the action handlers see: the server and the short ids (w<N>, ws<N>, out:<name>) the provider hands out.
  // A short id stays with its window or workspace until that object is destroyed, and is never reused.
  // The (at most) three of `candidates` nearest to `wanted` by edit distance, comma separated.
  std::string closestMatches(std::string_view wanted, std::vector<std::string> candidates);

  class ProviderContext {
  public:
    explicit ProviderContext(Server& server) : m_server(&server) {}

    [[nodiscard]] Server& server() const { return *m_server; }

    std::vector<idl::Window> windows();
    std::vector<idl::Workspace> workspaces();
    std::vector<idl::Output> outputs();

    std::expected<View*, idl::Error> resolveWindow(const idl::Id& id, std::string_view example);
    idl::Window windowEntry(const View& view);
    idl::Workspace workspaceEntry(const Workspace& workspace);
    // Runs the existing msg action `binds` with `arg`, as `andromeda msg <binds>:<arg>` does.
    std::expected<void, idl::Error> runBound(std::string_view binds, std::string_view arg, std::string_view example);

  private:
    idl::Id windowId(const View& view);
    idl::Id workspaceId(const Workspace& workspace);

    Server* m_server;
    // Keyed by the toplevel's ext-foreign-toplevel identifier and the workspace's protocol id: both are unique for the
    // object's lifetime, unlike its address.
    std::unordered_map<std::string, uint32_t> m_windowIds;
    std::unordered_map<std::string, uint32_t> m_workspaceIds;
    uint32_t m_nextWindowId = 1;
    uint32_t m_nextWorkspaceId = 1;
  };

  // The provider socket ($XDG_RUNTIME_DIR/andromeda-provider-<display>.sock): JSON lines, see tools/idl/gen.py and
  // .agent/TASK-vision-p0.md "Contracts". Each client gets hello and a snapshot, then events on real changes only.
  class Provider {
  public:
    enum class Family : uint8_t { Windows, Workspaces, Outputs };

    Provider(Server& server, const std::string& waylandSocketName);
    ~Provider();

    Provider(const Provider&) = delete;
    Provider& operator=(const Provider&) = delete;

    // Called from the server's coalesced IPC idle sources. Costs nothing while no client is connected.
    void notifyChanged(Family family);

  private:
    struct Connection {
      Provider* owner = nullptr;
      int fd = -1;
      wl_event_source* source = nullptr;
      std::string input;
      std::string output;
      size_t writeOffset = 0;
      uint64_t seq = 0;
      bool wantsWrite = false;
      bool dead = false;
    };
    static int onListenReadable(int fd, uint32_t mask, void* data);
    static int onConnectionEvent(int fd, uint32_t mask, void* data);

    void acceptConnections();
    void readLines(Connection& connection);
    void handleLine(Connection& connection, std::string_view line);
    void send(Connection& connection, const nlohmann::json& message);
    void flush(Connection& connection);
    void reap();
    nlohmann::json collect(Family family);
    // Recomputes `family` and, when it changed, stores it and sends every client one event whose delta is a JSON
    // merge patch (RFC 7386) carrying the family's full list. Returns whether it changed.
    bool refresh(Family family, const nlohmann::json& cause);

    Server* m_server;
    ProviderContext m_context;
    std::string m_socketPath;
    int m_listenFd = -1;
    wl_event_source* m_listenSource = nullptr;
    std::vector<std::unique_ptr<Connection>> m_connections;
    // Snapshot state: {"windows": [...], "workspaces": [...], "outputs": [...]} as last sent.
    nlohmann::json m_state = nlohmann::json::object();
  };

} // namespace andromeda
