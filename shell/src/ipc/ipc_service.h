#pragma once

#include "cli/schema_msg.h"
#include "ipc/ipc_invocation_context.h"

#include <functional>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <utility>
#include <vector>

enum class IpcActionEditorVisibility {
  Shown,
  Hidden,
};

struct IpcHandlerOptions {
  IpcActionEditorVisibility actionEditorVisibility = IpcActionEditorVisibility::Shown;
};

class IpcService {
public:
  using Handler = std::function<std::string(const std::string& args)>;
  using ActionEditorVisibility = IpcActionEditorVisibility;
  using HandlerOptions = IpcHandlerOptions;

  // A registered command, for callers that need to present or validate the command set.
  // Description views borrow static CLI schema storage.
  struct HandlerInfo {
    std::string_view command;
    // Argument spec without the verb, e.g. "<id> [context]". Empty when the command takes none.
    std::string_view args;
    std::string_view description;
    // Whether to offer this command in the action editor. Hidden commands remain available to IPC
    // and hand-written config.
    ActionEditorVisibility actionEditorVisibility = ActionEditorVisibility::Shown;
    // Whether the command moves one position along an ordered set. Bound to a scroll gesture,
    // one of those runs once per flick: the several notches an eager wheel movement emits are
    // one intent, not a request to skip that many entries.
    bool cycles = false;

    // "panel-toggle <id> [context]" — the form shown in --help.
    [[nodiscard]] std::string signature() const;
  };

  // Sets the invocation context for its lifetime, restoring the previous value on destruction so
  // that a handler which re-enters execute() nests correctly.
  class InvocationScope {
  public:
    InvocationScope(const IpcService& ipc, std::optional<IpcInvocationContext> context);
    ~InvocationScope();

    InvocationScope(const InvocationScope&) = delete;
    InvocationScope& operator=(const InvocationScope&) = delete;
    InvocationScope(InvocationScope&&) = delete;
    InvocationScope& operator=(InvocationScope&&) = delete;

  private:
    const IpcService& m_ipc;
    std::optional<IpcInvocationContext> m_previous;
  };

  IpcService() = default;
  ~IpcService();

  IpcService(const IpcService&) = delete;
  IpcService& operator=(const IpcService&) = delete;

  // Creates and binds the Unix socket. Returns false if it fails (IPC disabled).
  bool start();

  // Returns the listening fd, or -1 if not started.
  [[nodiscard]] int listenFd() const noexcept { return m_listenFd; }

  // Returns the socket path used.
  [[nodiscard]] const std::string& socketPath() const noexcept { return m_socketPath; }

  // Called by IpcPollSource when POLLIN fires on the listening fd.
  void dispatch();

  // Execute a command line using the same handler registry as socket IPC.
  [[nodiscard]] std::string execute(const std::string& line) const;

  // When set, relative paths in IPC handlers should resolve against this directory
  // (the caller's cwd) instead of the daemon's cwd. Only populated during execute().
  [[nodiscard]] const std::optional<std::string>& callerCwd() const noexcept { return m_callerCwd; }

  // Where the running command was invoked from, when it did not arrive over the socket.
  // Empty for socket-originated commands.
  [[nodiscard]] const std::optional<IpcInvocationContext>& invocationContext() const noexcept {
    return m_invocationContext;
  }

  // Registered commands, sorted by name.
  [[nodiscard]] std::vector<HandlerInfo> handlers() const;
  [[nodiscard]] bool hasHandler(std::string_view command) const noexcept;

  // Bind a handler to a canonical `msg` schema command. The handler receives everything after
  // the first space as `args` and must return a string ending with '\n'. Command identity and
  // help metadata come from the schema.
  void bind(const noctalia::cli::Command& command, Handler handler, HandlerOptions options = {});

  // A command that steps one position along an ordered set (workspaces, tracks, power profiles).
  // Bound like any other; on a scroll gesture it runs once per flick instead of once per notch,
  // so an eager wheel movement moves one position rather than several.
  void bindCycle(const noctalia::cli::Command& command, Handler handler, HandlerOptions options = {});

  // True when `command` was registered with bindCycle().
  [[nodiscard]] bool handlerCycles(std::string_view command) const noexcept;

  // Structured reply of the running handler, sent as "data" to `msg --json` callers; text callers still get the
  // handler's string. Only valid during execute().
  void setReplyData(nlohmann::json data) const;

  // Sends one event line {"topic", "data"} to every `msg subscribe` connection that asked for `topic` (a name from
  // cli::msg::kSubscribeTopics). A subscriber that cannot take the line at once is dropped.
  void publish(std::string_view topic, const nlohmann::json& data);

  // Open `msg subscribe` connections, for the poll source.
  [[nodiscard]] std::vector<int> subscriberFds() const;
  // The poll source saw POLLHUP or POLLERR on a subscriber fd: drops it, if it is still one.
  void subscriberClosed(int fd);

  // Takes a connection that sent `askpass`: returns true when it keeps `fd`; on false the service closes it.
  // `peerPid` comes from SO_PEERCRED.
  using AskpassHandler = std::function<bool(int fd, pid_t peerPid)>;
  void setAskpassHandler(AskpassHandler handler) { m_askpassHandler = std::move(handler); }

private:
  struct HandlerEntry {
    Handler fn;
    std::string argsSpec;
    std::string_view description;
    ActionEditorVisibility actionEditorVisibility = ActionEditorVisibility::Shown;
    bool cycles = false;
  };

  struct Subscriber {
    int fd = -1;
    std::vector<std::string> topics; // empty = all
  };

  // Returns true when it kept `connFd` as a subscriber.
  bool handleConnection(int connFd, pid_t peerPid);
  bool subscribe(int connFd, const std::string& args, bool json);
  void dropSubscriber(int fd);
  std::string buildHelp() const;
  [[nodiscard]] std::string executeParsed(const std::string& command, const std::string& args) const;
  [[nodiscard]] static std::string resolveSocketPath();

  int m_listenFd = -1;
  AskpassHandler m_askpassHandler;
  std::string m_socketPath;
  mutable std::optional<std::string> m_callerCwd;
  mutable std::optional<IpcInvocationContext> m_invocationContext;
  mutable std::unique_ptr<nlohmann::json> m_replyData;
  std::vector<Subscriber> m_subscribers;
  // Registration order is retained; --help output is sorted for display.
  std::vector<std::pair<std::string_view, HandlerEntry>> m_handlers;
};
