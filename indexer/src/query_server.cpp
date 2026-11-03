#include "query_server.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>

namespace andromeda::index {

  namespace {
    constexpr std::size_t kMaxLine = 4096;
    constexpr int kFirstPassedFd = 3; // SD_LISTEN_FDS_START

    int passedSocket() {
      const char* pid = std::getenv("LISTEN_PID");
      const char* count = std::getenv("LISTEN_FDS");
      if (pid == nullptr || count == nullptr || std::atoi(pid) != getpid() || std::atoi(count) < 1) {
        return -1;
      }
      // systemd passes a blocking socket; accept() must not block the loop.
      fcntl(kFirstPassedFd, F_SETFL, fcntl(kFirstPassedFd, F_GETFL) | O_NONBLOCK);
      fcntl(kFirstPassedFd, F_SETFD, FD_CLOEXEC);
      return kFirstPassedFd;
    }
  } // namespace

  QueryServer::QueryServer(const IndexDb& db, int listenFd) : m_db(db), m_listen(listenFd) {}

  QueryServer::~QueryServer() {
    for (const auto& [fd, pending] : m_clients) {
      close(fd);
    }
    if (m_listen >= 0) {
      close(m_listen);
    }
  }

  int QueryServer::listenSocket(const std::filesystem::path& path) {
    if (const int fd = passedSocket(); fd >= 0) {
      return fd;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::string p = path.string();
    if (p.size() >= sizeof(addr.sun_path)) {
      return -1;
    }
    std::memcpy(addr.sun_path, p.c_str(), p.size() + 1);
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      return -1;
    }
    unlink(p.c_str());
    if (bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 || listen(fd, 16) != 0) {
      close(fd);
      return -1;
    }
    return fd;
  }

  void QueryServer::addPollFds(std::vector<pollfd>& fds) const {
    fds.push_back({.fd = m_listen, .events = POLLIN, .revents = 0});
    for (const auto& [fd, pending] : m_clients) {
      fds.push_back({.fd = fd, .events = POLLIN, .revents = 0});
    }
  }

  void QueryServer::handle(std::span<const pollfd> fds) {
    for (const pollfd& p : fds) {
      if (p.revents == 0) {
        continue;
      }
      if (p.fd == m_listen) {
        accept();
      } else if (!serve(p.fd)) {
        close(p.fd);
        m_clients.erase(p.fd);
      }
    }
  }

  void QueryServer::accept() {
    while (true) {
      const int fd = accept4(m_listen, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (fd < 0) {
        return;
      }
      m_clients.emplace(fd, std::string{});
    }
  }

  bool QueryServer::serve(int fd) {
    std::string& pending = m_clients[fd];
    std::array<char, 4096> buffer{};
    while (true) {
      const ssize_t n = read(fd, buffer.data(), buffer.size());
      if (n == 0) {
        return false;
      }
      if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
          return false;
        }
        break;
      }
      pending.append(buffer.data(), static_cast<std::size_t>(n));
    }
    std::string out;
    std::size_t start = 0;
    for (std::size_t end = pending.find('\n'); end != std::string::npos; end = pending.find('\n', start)) {
      out += answer(std::string_view(pending).substr(start, end - start));
      start = end + 1;
    }
    pending.erase(0, start);
    if (pending.size() > kMaxLine) {
      return false;
    }
    // Answers are at most kMaxLimit rows and fit the socket buffer; a client that does not read is dropped
    // rather than allowed to block the daemon.
    return out.empty() || send(fd, out.data(), out.size(), MSG_NOSIGNAL | MSG_DONTWAIT) == static_cast<ssize_t>(out.size());
  }

  std::string QueryServer::answer(std::string_view line) const {
    constexpr std::string_view kQuery = "query ";
    if (!line.starts_with(kQuery)) {
      return "\n";
    }
    line.remove_prefix(kQuery.size());
    int limit = 0;
    const auto [rest, error] = std::from_chars(line.data(), line.data() + line.size(), limit);
    if (error != std::errc{} || rest == line.data() + line.size() || *rest != ' ') {
      return "\n";
    }
    const std::string_view text = line.substr(static_cast<std::size_t>(rest - line.data()) + 1);
    std::string out;
    for (const FileRow& row : m_db.queryNames(text, std::clamp(limit, 1, kMaxLimit))) {
      // A path with a tab or a newline cannot be framed in this protocol.
      if (row.path.find_first_of("\t\n") != std::string::npos) {
        continue;
      }
      out += row.path;
      out += '\t';
      out += std::to_string(row.mtime);
      out += '\t';
      out += std::to_string(row.size);
      out += row.isDir ? "\td\n" : "\tf\n";
    }
    out += '\n';
    return out;
  }

} // namespace andromeda::index
