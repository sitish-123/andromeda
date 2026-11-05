#include "ipc/ipc_service.h"

#include "cli/help.h"
#include "cli/schema_msg.h"
#include "core/log.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <memory>
#include <nlohmann/json.hpp>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("ipc");
  constexpr std::size_t kMaxCommandBytes = 64 * 1024;
  constexpr int kRecvTimeoutMs = 100;
  constexpr char kCallerCwdSeparator = '\x1e';
  constexpr std::size_t kMaxSubscribers = 32;
  constexpr std::string_view kJsonFlag = "--json ";

  void sendAll(int fd, std::string_view text) {
    std::size_t sent = 0;
    while (sent < text.size()) {
      const auto n = ::send(fd, text.data() + sent, text.size() - sent, MSG_NOSIGNAL);
      if (n <= 0) {
        break;
      }
      sent += static_cast<std::size_t>(n);
    }
  }

  [[nodiscard]] std::optional<std::string> parseCallerCwdPrefix(std::string& line) {
    const auto separator = line.find(kCallerCwdSeparator);
    if (separator == std::string::npos) {
      return std::nullopt;
    }

    std::string cwd = line.substr(0, separator);
    line.erase(0, separator + 1);
    if (cwd.empty()) {
      return std::nullopt;
    }

    std::error_code ec;
    const std::filesystem::path cwdPath(cwd);
    if (!cwdPath.is_absolute() || !std::filesystem::is_directory(cwdPath, ec) || ec) {
      return std::nullopt;
    }
    return cwd;
  }

} // namespace

IpcService::InvocationScope::InvocationScope(const IpcService& ipc, std::optional<IpcInvocationContext> context)
    : m_ipc(ipc), m_previous(std::move(ipc.m_invocationContext)) {
  m_ipc.m_invocationContext = std::move(context);
}

IpcService::InvocationScope::~InvocationScope() { m_ipc.m_invocationContext = std::move(m_previous); }

IpcService::~IpcService() {
  for (const Subscriber& subscriber : m_subscribers) {
    ::close(subscriber.fd);
  }
  if (m_listenFd >= 0) {
    ::close(m_listenFd);
    m_listenFd = -1;
  }
  if (!m_socketPath.empty()) {
    ::unlink(m_socketPath.c_str());
  }
}

bool IpcService::start() {
  m_socketPath = resolveSocketPath();
  if (m_socketPath.empty()) {
    kLog.warn("IPC disabled: could not determine socket path");
    return false;
  }

  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    kLog.warn("IPC disabled: socket() failed: {}", std::strerror(errno));
    return false;
  }

  // Remove stale socket file before binding
  ::unlink(m_socketPath.c_str());

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (m_socketPath.size() >= sizeof(addr.sun_path)) {
    kLog.warn("IPC disabled: socket path too long");
    ::close(fd);
    return false;
  }
  std::memcpy(addr.sun_path, m_socketPath.c_str(), m_socketPath.size() + 1);

  if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
    kLog.warn("IPC disabled: bind() failed: {}", std::strerror(errno));
    ::close(fd);
    return false;
  }

  if (::chmod(m_socketPath.c_str(), S_IRUSR | S_IWUSR) < 0) {
    kLog.warn("IPC disabled: chmod() failed: {}", std::strerror(errno));
    ::close(fd);
    ::unlink(m_socketPath.c_str());
    return false;
  }

  if (::listen(fd, 128) < 0) {
    kLog.warn("IPC disabled: listen() failed: {}", std::strerror(errno));
    ::close(fd);
    ::unlink(m_socketPath.c_str());
    return false;
  }

  m_listenFd = fd;
  return true;
}

void IpcService::bind(const noctalia::cli::Command& command, Handler handler, HandlerOptions options) {
  const noctalia::cli::Command* spec = noctalia::cli::findMsgCommand(command.name);
  if (spec == nullptr) {
    kLog.error("cannot bind non-msg CLI command '{}'", command.name);
    return;
  }

  std::erase_if(m_handlers, [spec](const auto& entry) { return entry.first == spec->name; });
  m_handlers.emplace_back(
      spec->name,
      HandlerEntry{
          .fn = std::move(handler),
          .argsSpec = noctalia::cli::renderArgsSpec(*spec),
          .description = spec->summary,
          .actionEditorVisibility = options.actionEditorVisibility,
          .cycles = false,
      }
  );
}

void IpcService::bindCycle(const noctalia::cli::Command& command, Handler handler, HandlerOptions options) {
  bind(command, std::move(handler), options);
  const auto it =
      std::ranges::find_if(m_handlers, [&command](const auto& entry) { return entry.first == command.name; });
  if (it != m_handlers.end())
    it->second.cycles = true;
}

bool IpcService::handlerCycles(std::string_view command) const noexcept {
  const auto it = std::ranges::find_if(m_handlers, [command](const auto& e) { return e.first == command; });
  return it != m_handlers.end() && it->second.cycles;
}

std::vector<IpcService::HandlerInfo> IpcService::handlers() const {
  std::vector<HandlerInfo> infos;
  infos.reserve(m_handlers.size());
  for (const auto& [command, entry] : m_handlers) {
    infos.push_back(
        HandlerInfo{
            .command = command,
            .args = entry.argsSpec,
            .description = entry.description,
            .actionEditorVisibility = entry.actionEditorVisibility,
            .cycles = entry.cycles,
        }
    );
  }
  std::ranges::sort(infos, {}, &HandlerInfo::command);
  return infos;
}

bool IpcService::hasHandler(std::string_view command) const noexcept {
  return std::ranges::any_of(m_handlers, [command](const auto& entry) { return entry.first == command; });
}

void IpcService::dispatch() {
  while (true) {
    const int connFd = ::accept4(m_listenFd, nullptr, nullptr, SOCK_CLOEXEC);
    if (connFd < 0) {
      break; // EAGAIN / EWOULDBLOCK — no more pending connections
    }
    ucred peer{};
    socklen_t size = sizeof(peer);
    if (::getsockopt(connFd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 || peer.uid != ::geteuid()) {
      kLog.warn("IPC: refused a connection from uid {}", peer.uid);
      ::close(connFd);
      continue;
    }
    if (!handleConnection(connFd)) {
      ::close(connFd);
    }
  }
}

std::string IpcService::execute(const std::string& line) const {
  auto* self = const_cast<IpcService*>(this);
  self->m_callerCwd.reset();

  std::string commandLine = line;
  if (auto cwd = parseCallerCwdPrefix(commandLine); cwd.has_value()) {
    self->m_callerCwd = std::move(*cwd);
  }

  const bool json = commandLine.starts_with(kJsonFlag);
  if (json) {
    commandLine.erase(0, kJsonFlag.size());
  }

  std::string command;
  std::string args;
  const auto spacePos = commandLine.find(' ');
  if (spacePos == std::string::npos) {
    command = commandLine;
  } else {
    command = commandLine.substr(0, spacePos);
    args = commandLine.substr(spacePos + 1);
  }

  if (command == "--help" || command == "-h") {
    return buildHelp();
  }
  // A handler may re-enter execute(); each level keeps its own reply data.
  std::unique_ptr<nlohmann::json> outer = std::move(m_replyData);
  std::string text = executeParsed(command, args);
  std::unique_ptr<nlohmann::json> data = std::exchange(m_replyData, std::move(outer));
  if (!json) {
    return text;
  }
  nlohmann::json reply = {{"ok", !text.starts_with("error:")}};
  if (text.ends_with('\n')) {
    text.pop_back();
  }
  reply["text"] = std::move(text);
  reply["data"] = data != nullptr ? std::move(*data) : nlohmann::json(nullptr);
  return reply.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

void IpcService::setReplyData(nlohmann::json data) const {
  m_replyData = std::make_unique<nlohmann::json>(std::move(data));
}

bool IpcService::subscribe(int connFd, const std::string& args, bool json) {
  const auto refuse = [connFd, json](const std::string& error) {
    if (json) {
      sendAll(connFd, nlohmann::json{{"ok", false}, {"text", error}, {"data", nullptr}}.dump() + "\n");
    } else {
      sendAll(connFd, error + "\n");
    }
    return false;
  };
  std::vector<std::string> topics;
  std::string_view rest = args;
  while (!rest.empty()) {
    const auto start = rest.find_first_not_of(" \t\n");
    if (start == std::string_view::npos) {
      break;
    }
    rest.remove_prefix(start);
    const auto end = std::min(rest.find_first_of(" \t\n"), rest.size());
    const std::string_view topic = rest.substr(0, end);
    if (std::ranges::find(noctalia::cli::msg::kSubscribeTopics, topic) == noctalia::cli::msg::kSubscribeTopics.end()) {
      std::string error = std::format("error: unknown topic '{}' (topics:", topic);
      for (const std::string_view known : noctalia::cli::msg::kSubscribeTopics) {
        error += ' ';
        error += known;
      }
      return refuse(error + ")");
    }
    topics.emplace_back(topic);
    rest.remove_prefix(end);
  }
  if (m_subscribers.size() >= kMaxSubscribers) {
    return refuse("error: too many subscribers");
  }
  const int flags = ::fcntl(connFd, F_GETFL);
  if (flags < 0 || ::fcntl(connFd, F_SETFL, flags | O_NONBLOCK) < 0) {
    return refuse("error: subscribe failed");
  }
  nlohmann::json ack = {{"subscribed", topics.empty() ? nlohmann::json(noctalia::cli::msg::kSubscribeTopics) : nlohmann::json(topics)}};
  sendAll(connFd, ack.dump() + "\n");
  m_subscribers.push_back({.fd = connFd, .topics = std::move(topics)});
  return true;
}

void IpcService::publish(std::string_view topic, const nlohmann::json& data) {
  if (m_subscribers.empty()) {
    return;
  }
  const std::string line =
      nlohmann::json{{"topic", topic}, {"data", data}}.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
  std::vector<int> slow;
  for (const Subscriber& subscriber : m_subscribers) {
    if (!subscriber.topics.empty() && std::ranges::find(subscriber.topics, topic) == subscriber.topics.end()) {
      continue;
    }
    // Non-blocking: a partial line would corrupt the stream, so a subscriber that cannot take it whole is dropped.
    const auto n = ::send(subscriber.fd, line.data(), line.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n != static_cast<ssize_t>(line.size())) {
      slow.push_back(subscriber.fd);
    }
  }
  for (const int fd : slow) {
    kLog.warn("IPC: dropped a subscriber that fell behind");
    dropSubscriber(fd);
  }
}

std::vector<int> IpcService::subscriberFds() const {
  std::vector<int> fds;
  fds.reserve(m_subscribers.size());
  for (const Subscriber& subscriber : m_subscribers) {
    fds.push_back(subscriber.fd);
  }
  return fds;
}

void IpcService::subscriberClosed(int fd) { dropSubscriber(fd); }

void IpcService::dropSubscriber(int fd) {
  const auto it = std::ranges::find(m_subscribers, fd, &Subscriber::fd);
  if (it != m_subscribers.end()) {
    ::close(fd);
    m_subscribers.erase(it);
  }
}

bool IpcService::handleConnection(int connFd) {
  // Set receive timeout so a slow client doesn't stall the main loop
  timeval tv{};
  tv.tv_sec = 0;
  tv.tv_usec = kRecvTimeoutMs * 1000;
  ::setsockopt(connFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  // Read until the client closes its write side. Newlines are valid command
  // payload, so they cannot be used as the frame delimiter.
  std::string command;
  char buf[4096];
  bool reachedEof = false;
  while (command.size() < kMaxCommandBytes) {
    const std::size_t limit = std::min(sizeof(buf), kMaxCommandBytes - command.size());
    const auto n = ::read(connFd, buf, limit);
    if (n > 0) {
      command.append(buf, static_cast<std::size_t>(n));
      continue;
    }
    if (n == 0) {
      reachedEof = true;
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }
    kLog.warn("IPC read failed: {}", std::strerror(errno));
    break;
  }

  if (command.size() >= kMaxCommandBytes && !reachedEof) {
    sendAll(connFd, "error: IPC command too large\n");
    return false;
  }

  // Legacy external clients may still send one newline-terminated command and
  // keep the socket open while waiting for the response.
  if (!reachedEof) {
    if (const auto newline = command.find('\n'); newline != std::string::npos) {
      command.resize(newline);
    }
    if (!command.empty() && command.back() == '\r') {
      command.pop_back();
    }
  }

  if (command.empty()) {
    // Client closed the connection without sending anything (e.g. a liveness probe).
    return false;
  }

  std::string line = command;
  parseCallerCwdPrefix(line);
  const bool json = line.starts_with(kJsonFlag);
  if (json) {
    line.erase(0, kJsonFlag.size());
  }
  if (line == "subscribe" || line.starts_with("subscribe ")) {
    return subscribe(connFd, line.size() > 9 ? line.substr(10) : std::string(), json);
  }

  // A socket command has no in-process origin, even when the dispatch re-enters from a handler
  // that pumped the event loop.
  const InvocationScope socketScope(*this, std::nullopt);
  sendAll(connFd, execute(command));
  return false;
}

std::string IpcService::HandlerInfo::signature() const {
  std::string out(command);
  if (!args.empty()) {
    out += ' ';
    out += args;
  }
  return out;
}

std::string IpcService::buildHelp() const {
  auto infos = handlers();

  std::vector<std::string> signatures;
  signatures.reserve(infos.size());
  std::size_t maxSignature = 0;
  for (const auto& info : infos) {
    signatures.push_back(info.signature());
    maxSignature = std::max(maxSignature, signatures.back().size());
  }

  std::string out = "Usage: noctalia msg <command> [args]\n\nCommands:\n";
  for (std::size_t i = 0; i < infos.size(); ++i) {
    out += "  ";
    out += signatures[i];
    if (!infos[i].description.empty()) {
      out += std::string(maxSignature - signatures[i].size() + 2, ' ');
      out += infos[i].description;
    }
    out += '\n';
  }
  return out;
}

std::string IpcService::executeParsed(const std::string& command, const std::string& args) const {
  const auto it = std::ranges::find_if(m_handlers, [&command](const auto& e) { return e.first == command; });
  if (it == m_handlers.end()) {
    return "error: unknown command (try: noctalia msg --help)\n";
  }
  return it->second.fn(args);
}

// Only the per-user runtime directory: a world-writable fallback such as /tmp lets another user take the path first.
std::string IpcService::resolveSocketPath() {
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  if (runtime == nullptr || runtime[0] == '\0') {
    return {};
  }
  const char* display = std::getenv("WAYLAND_DISPLAY");
  if (display == nullptr || display[0] == '\0') {
    display = "wayland-0";
  }
  return std::string(runtime) + "/noctalia-" + display + ".sock";
}
