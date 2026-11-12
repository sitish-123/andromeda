#include "shell/polkit/askpass_broker.h"

#include "core/log.h"
#include "util/secure_wipe.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

#ifndef SO_PEERPIDFD
#define SO_PEERPIDFD 77
#endif

namespace {

  constexpr Logger kLog("askpass");
  constexpr std::size_t kMaxHandshakeBytes = 16;
  constexpr auto kReadyTimeout = std::chrono::seconds(2);

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

  // A pidfd polls readable once its process has exited.
  bool alive(int pidfd) {
    if (pidfd < 0) {
      return false;
    }
    pollfd pfd{.fd = pidfd, .events = POLLIN, .revents = 0};
    return ::poll(&pfd, 1, 0) == 0;
  }

  int peerPidfd(int fd, pid_t peerPid) {
    int pidfd = -1;
    socklen_t size = sizeof(pidfd);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERPIDFD, &pidfd, &size) == 0 && pidfd >= 0) {
      return pidfd;
    }
    return static_cast<int>(::syscall(SYS_pidfd_open, peerPid, 0));
  }

  void closeFd(int fd) {
    if (fd >= 0) {
      ::close(fd);
    }
  }

} // namespace

AskpassBroker::AskpassBroker(AskpassProc proc, PidfdOpener openPidfd)
    : m_proc(std::move(proc)), m_openPidfd(std::move(openPidfd)) {
  if (!m_openPidfd) {
    m_openPidfd = [](pid_t pid) { return static_cast<int>(::syscall(SYS_pidfd_open, pid, 0)); };
  }
}

AskpassBroker::~AskpassBroker() {
  if (m_pending) {
    ::close(m_pending->fd);
    closeFd(m_pending->peerPidfd);
    closeFd(m_pending->parentPidfd);
  }
}

void AskpassBroker::setStateCallback(StateCallback callback) { m_stateCallback = std::move(callback); }

void AskpassBroker::refuse(int fd, std::string_view reason) { sendAll(fd, std::string("error: ").append(reason) + '\n'); }

bool AskpassBroker::begin(int fd, pid_t peerPid) {
  if (m_pending) {
    refuse(fd, "another password request is open");
    return false;
  }
  // The pidfd pins the peer first, so the /proc reads below cannot land on a reused pid unnoticed.
  const int peer = peerPidfd(fd, peerPid);
  // The credentials at connect(): a process that inherits the fd later cannot change them.
  ucred cred{};
  socklen_t credSize = sizeof(cred);
  auto caller = ::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &credSize) == 0
      ? verifyAskpassCaller(m_proc, peerPid, cred.gid)
      : std::unexpected(std::string("internal error"));
  const int parent = caller ? m_openPidfd(caller->parentPid) : -1;
  std::string error;
  if (!caller) {
    error = caller.error();
  } else if (!alive(peer) || !alive(parent)) {
    error = "the caller exited";
  } else if (auto changed = recheckAskpassCaller(m_proc, peerPid, caller->parentPid)) {
    error = std::move(*changed);
  }
  const int flags = ::fcntl(fd, F_GETFL);
  if (error.empty() && (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)) {
    error = "internal error";
  }
  if (!error.empty()) {
    kLog.warn("askpass: refused pid {}: {}", peerPid, error);
    refuse(fd, error);
    closeFd(peer);
    closeFd(parent);
    return false;
  }
  m_pending = Pending{
      .fd = fd,
      .peerPidfd = peer,
      .parentPidfd = parent,
      .peerPid = peerPid,
      .caller = std::move(*caller),
      .readyDeadline = std::chrono::steady_clock::now() + kReadyTimeout,
  };
  sendAll(fd, "verify\n");
  return true;
}

void AskpassBroker::readReady() {
  char buf[kMaxHandshakeBytes];
  const auto n = ::recv(m_pending->fd, buf, sizeof(buf), MSG_DONTWAIT);
  if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
    return;
  }
  if (n <= 0) {
    finish({});
    return;
  }
  m_pending->input.append(buf, static_cast<std::size_t>(n));
  if (m_pending->input.find('\n') == std::string::npos) {
    if (m_pending->input.size() >= kMaxHandshakeBytes) {
      finish("error: bad handshake\n");
    }
    return;
  }
  if (m_pending->input != "ready\n") {
    finish("error: bad handshake\n");
    return;
  }
  std::string error;
  if (auto changed = recheckAskpassCaller(m_proc, m_pending->peerPid, m_pending->caller.parentPid)) {
    error = std::move(*changed);
  } else if (!alive(m_pending->peerPidfd) || !alive(m_pending->parentPidfd)) {
    error = "the caller exited";
  }
  if (!error.empty()) {
    kLog.warn("askpass: refused pid {}: {}", m_pending->peerPid, error);
    finish("error: " + error + "\n");
    return;
  }
  kLog.info("askpass: request from {} (pid {})", m_pending->caller.request.requester,
            m_pending->caller.request.requesterPid);
  m_pending->phase = Phase::Shown;
  if (m_stateCallback) {
    m_stateCallback();
  }
}

void AskpassBroker::submitResponse(std::string_view password) {
  if (!hasPendingRequest()) {
    return;
  }
  // Reserved up front: a reallocation would leave an unwiped copy of the password.
  std::string reply;
  reply.reserve(password.size() + 16);
  reply += "password\n";
  reply += password;
  reply += '\n';
  finish(reply);
  util::secureWipe(reply);
}

void AskpassBroker::cancelRequest() {
  if (hasPendingRequest()) {
    finish("cancel\n");
  }
}

void AskpassBroker::finish(std::string_view reply) {
  if (!m_pending) {
    return;
  }
  const bool shown = m_pending->phase == Phase::Shown;
  if (!reply.empty()) {
    sendAll(m_pending->fd, reply);
  }
  ::close(m_pending->fd);
  closeFd(m_pending->peerPidfd);
  closeFd(m_pending->parentPidfd);
  m_pending.reset();
  if (shown && m_stateCallback) {
    m_stateCallback();
  }
}

void AskpassBroker::doAddPollFds(std::vector<pollfd>& fds) {
  m_polled = m_pending.has_value();
  if (!m_polled) {
    return;
  }
  // Once shown, no events are asked: only POLLHUP or POLLERR, the client leaving, matter.
  const short events = m_pending->phase == Phase::AwaitReady ? POLLIN : 0;
  fds.push_back({.fd = m_pending->fd, .events = events, .revents = 0});
}

int AskpassBroker::pollTimeoutMs() const {
  if (!m_pending || m_pending->phase != Phase::AwaitReady) {
    return -1;
  }
  const auto left = std::chrono::ceil<std::chrono::milliseconds>(m_pending->readyDeadline - std::chrono::steady_clock::now());
  return static_cast<int>(std::max<std::chrono::milliseconds::rep>(left.count(), 0));
}

void AskpassBroker::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  if (!m_polled || startIdx >= fds.size() || !m_pending || fds[startIdx].fd != m_pending->fd) {
    return;
  }
  const short revents = fds[startIdx].revents;
  if (m_pending->phase == Phase::AwaitReady && std::chrono::steady_clock::now() >= m_pending->readyDeadline) {
    kLog.warn("askpass: refused pid {}: no ready in time", m_pending->peerPid);
    finish("error: no ready in time\n");
    return;
  }
  if (m_pending->phase == Phase::AwaitReady && (revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
    readReady();
    return;
  }
  if ((revents & (POLLHUP | POLLERR)) != 0) {
    kLog.info("askpass: the client left");
    finish({});
  }
}
