#include "ipc/ipc_client.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <print>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

  constexpr char kCallerCwdSeparator = '\x1e';

  std::string resolveSocketPath() {
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

} // namespace

int IpcClient::send(const std::string& command, bool stream) {
  const std::string path = resolveSocketPath();
  if (path.empty()) {
    std::println(stderr, "error: XDG_RUNTIME_DIR is not set");
    return 1;
  }

  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    std::println(stderr, "error: socket() failed: {}", std::strerror(errno));
    return 1;
  }

  // Set connect/send/recv timeout to 2 seconds
  timeval tv{};
  tv.tv_sec = 2;
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    std::println(stderr, "error: socket path too long");
    ::close(fd);
    return 1;
  }
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

  if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
    std::println(stderr, "error: noctalia is not running");
    ::close(fd);
    return 1;
  }

  // Prefix the caller cwd so the daemon resolves relative paths correctly.
  std::string line;
  std::error_code ec;
  const std::filesystem::path cwd = std::filesystem::current_path(ec);
  if (!ec && cwd.is_absolute()) {
    line = cwd.string();
    line += kCallerCwdSeparator;
  }
  line += command;

  std::size_t sent = 0;
  while (sent < line.size()) {
    const auto written = ::write(fd, line.data() + sent, line.size() - sent);
    if (written < 0) {
      std::println(stderr, "error: write() failed: {}", std::strerror(errno));
      ::close(fd);
      return 1;
    }
    sent += static_cast<std::size_t>(written);
  }

  if (::shutdown(fd, SHUT_WR) < 0) {
    std::println(stderr, "error: shutdown() failed: {}", std::strerror(errno));
    ::close(fd);
    return 1;
  }

  if (stream) {
    timeval none{};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof(none));
  }

  // Read response until EOF (server closes connection after writing)
  std::string response;
  char buf[4096];
  for (;;) {
    const auto n = ::read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::println(stderr, "error: read() failed: {}", std::strerror(errno));
      ::close(fd);
      return 1;
    }
    if (n == 0) {
      break;
    }
    if (stream) {
      std::fwrite(buf, 1, static_cast<std::size_t>(n), stdout);
      std::fflush(stdout);
      if (response.empty()) {
        response.assign(buf, static_cast<std::size_t>(std::min<ssize_t>(n, 6)));
      }
      continue;
    }
    response.append(buf, static_cast<std::size_t>(n));
  }

  ::close(fd);

  if (stream) {
    // A stream ends only when the shell exits or drops this subscriber.
    std::fputs("error: the event stream ended\n", stderr);
    return 1;
  }
  std::fputs(response.c_str(), stdout);
  if (response.starts_with('{')) {
    const auto reply = nlohmann::json::parse(response, nullptr, false);
    if (reply.is_object() && reply.contains("ok") && reply["ok"].is_boolean()) {
      return reply["ok"].get<bool>() ? 0 : 1;
    }
  }
  return response.starts_with("error:") ? 1 : 0;
}
