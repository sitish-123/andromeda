#include "helper_client.h"

#include "core/log.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#ifndef ANDROMEDA_PREVIEW_HELPER_PATH
#define ANDROMEDA_PREVIEW_HELPER_PATH "/usr/lib/andromeda/andromeda-preview-helper"
#endif

namespace preview {

  namespace {
    constexpr Logger kLog("preview-helper");
    constexpr std::size_t kMsgSize = 24;
    constexpr int kHelperSocketFd = 3;

    std::array<std::uint8_t, kMsgSize> encode(const HelperMsg& m) {
      std::array<std::uint8_t, kMsgSize> out{};
      const std::array<std::uint32_t, 6> words{m.op, m.a, m.b, m.c, m.d, m.e};
      for (std::size_t i = 0; i < words.size(); ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
          out[i * 4 + j] = static_cast<std::uint8_t>(words[i] >> (8 * j));
        }
      }
      return out;
    }

    HelperMsg decode(const std::array<std::uint8_t, kMsgSize>& in) {
      auto w = [&](std::size_t i) {
        return static_cast<std::uint32_t>(in[i * 4]) | (static_cast<std::uint32_t>(in[i * 4 + 1]) << 8)
            | (static_cast<std::uint32_t>(in[i * 4 + 2]) << 16) | (static_cast<std::uint32_t>(in[i * 4 + 3]) << 24);
      };
      return {w(0), w(1), w(2), w(3), w(4), w(5)};
    }
  } // namespace

  std::string helperPath() {
    if (const char* env = std::getenv("ANDROMEDA_PREVIEW_HELPER"); env != nullptr && *env != '\0') {
      return env;
    }
    return ANDROMEDA_PREVIEW_HELPER_PATH;
  }

  std::shared_ptr<MappedFd> MappedFd::map(int fd) {
    // The helper is untrusted: an unsealed memfd could shrink under the mapping and fault the window process.
    constexpr int kSeals = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
    struct stat st {};
    const int seals = fcntl(fd, F_GET_SEALS);
    if (seals < 0 || (seals & kSeals) != kSeals || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
      ::close(fd);
      return nullptr;
    }
    const auto size = static_cast<std::size_t>(st.st_size);
    void* data = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (data == MAP_FAILED) {
      return nullptr;
    }
    return std::shared_ptr<MappedFd>(new MappedFd(static_cast<const std::uint8_t*>(data), size));
  }

  MappedFd::~MappedFd() { munmap(const_cast<std::uint8_t*>(m_data), m_size); }

  std::unique_ptr<HelperClient> HelperClient::start(HelperKind kind, const std::string& path) {
    std::array<int, 2> sv{-1, -1};
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv.data()) != 0) {
      kLog.warn("socketpair: {}", std::strerror(errno));
      return nullptr;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    // dup2 clears FD_CLOEXEC on the target, so the helper gets fd 3 and nothing else past stderr.
    posix_spawn_file_actions_adddup2(&actions, sv[1], kHelperSocketFd);
    posix_spawn_file_actions_addclosefrom_np(&actions, kHelperSocketFd + 1);

    const std::string helper = helperPath();
    const std::string kindArg = kind == HelperKind::Pdf ? "pdf" : "text";
    std::vector<std::string> envStrings;
    for (const char* name : {"HOME", "XDG_CACHE_HOME", "RUST_BACKTRACE"}) {
      if (const char* value = std::getenv(name); value != nullptr) {
        envStrings.push_back(std::string(name) + "=" + value);
      }
    }
    std::vector<char*> env;
    for (auto& s : envStrings) {
      env.push_back(s.data());
    }
    env.push_back(nullptr);
    std::array<char*, 4> argv{
        const_cast<char*>(helper.c_str()), const_cast<char*>(kindArg.c_str()), const_cast<char*>(path.c_str()), nullptr
    };
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, helper.c_str(), &actions, nullptr, argv.data(), env.data());
    posix_spawn_file_actions_destroy(&actions);
    ::close(sv[1]);
    if (rc != 0) {
      kLog.warn("cannot start {}: {}", helper, std::strerror(rc));
      ::close(sv[0]);
      return nullptr;
    }
    return std::unique_ptr<HelperClient>(new HelperClient(sv[0], pid));
  }

  HelperClient::~HelperClient() {
    if (m_fd >= 0) {
      ::close(m_fd);
    }
    if (m_pid > 0) {
      // The helper exits on EOF; SIGKILL covers one stuck in a decoder.
      kill(m_pid, SIGKILL);
      waitpid(m_pid, nullptr, 0);
    }
  }

  void HelperClient::request(const HelperMsg& msg, ReplyCallback callback) {
    if (m_fd < 0) {
      callback(HelperReply{});
      return;
    }
    const auto bytes = encode(msg);
    if (send(m_fd, bytes.data(), bytes.size(), MSG_NOSIGNAL | MSG_DONTWAIT) != static_cast<ssize_t>(bytes.size())) {
      callback(HelperReply{});
      fail();
      return;
    }
    if (m_pending.empty()) {
      m_deadline = std::chrono::steady_clock::now() + kRequestTimeout;
    }
    m_pending.push_back(std::move(callback));
  }

  void HelperClient::readOne() {
    std::array<std::uint8_t, kMsgSize> bytes{};
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> control{};
    iovec iov{.iov_base = bytes.data(), .iov_len = bytes.size()};
    msghdr hdr{};
    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
    hdr.msg_control = control.data();
    hdr.msg_controllen = control.size();
    // Non-blocking: a stale readiness report (the old helper's fd number reused by a new one) must not stall the loop.
    const ssize_t got = recvmsg(m_fd, &hdr, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
    if (got < 0 && (errno == EAGAIN || errno == EINTR)) {
      return;
    }
    int fd = -1;
    for (cmsghdr* c = CMSG_FIRSTHDR(&hdr); c != nullptr; c = CMSG_NXTHDR(&hdr, c)) {
      if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS && c->cmsg_len >= CMSG_LEN(sizeof(int))) {
        std::memcpy(&fd, CMSG_DATA(c), sizeof(int));
      }
    }
    if (got != static_cast<ssize_t>(kMsgSize) || (hdr.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
      if (fd >= 0) {
        ::close(fd);
      }
      fail();
      return;
    }
    HelperReply reply{.msg = decode(bytes), .data = nullptr};
    if (fd >= 0) {
      reply.data = MappedFd::map(fd);
      if (reply.data == nullptr) {
        reply.msg = HelperMsg{};
      }
    }
    if (m_pending.empty()) {
      fail();
      return;
    }
    auto callback = std::move(m_pending.front());
    m_pending.pop_front();
    m_deadline = std::chrono::steady_clock::now() + kRequestTimeout;
    callback(reply);
  }

  void HelperClient::fail() {
    if (m_fd >= 0) {
      ::close(m_fd);
      m_fd = -1;
    }
    if (m_pid > 0) {
      kill(m_pid, SIGKILL);
      waitpid(m_pid, nullptr, 0);
      m_pid = -1;
    }
    auto pending = std::move(m_pending);
    m_pending.clear();
    for (auto& callback : pending) {
      callback(HelperReply{});
    }
  }

  void HelperClient::drain() {
    while (m_fd >= 0 && !m_pending.empty()) {
      pollfd pfd{.fd = m_fd, .events = POLLIN, .revents = 0};
      const int timeout = pollTimeoutMs();
      if (timeout == 0) {
        kLog.warn("helper request timed out");
        fail();
        return;
      }
      if (poll(&pfd, 1, timeout) < 0 && errno != EINTR) {
        fail();
        return;
      }
      if ((pfd.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
        readOne();
      }
    }
  }

  void HelperClient::doAddPollFds(std::vector<pollfd>& fds) {
    if (m_fd >= 0 && !m_pending.empty()) {
      fds.push_back({.fd = m_fd, .events = POLLIN, .revents = 0});
    }
  }

  int HelperClient::pollTimeoutMs() const {
    if (m_fd < 0 || m_pending.empty()) {
      return -1;
    }
    const auto left =
        std::chrono::ceil<std::chrono::milliseconds>(m_deadline - std::chrono::steady_clock::now()).count();
    return static_cast<int>(std::max<std::int64_t>(0, left));
  }

  void HelperClient::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
    if (pollTimeoutMs() == 0) {
      kLog.warn("helper request timed out");
      fail();
      return;
    }
    if (startIdx >= fds.size() || fds[startIdx].fd != m_fd) {
      return;
    }
    if ((fds[startIdx].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      readOne();
    }
  }

} // namespace preview
