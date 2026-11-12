// andromeda-askpass: SUDO_ASKPASS helper. Asks the running shell for the password over its IPC socket and prints
// it to stdout. Exit 1 with empty stdout on cancel, timeout or when no shell runs.

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

  constexpr int kTimeoutMs = 120 * 1000;
  constexpr std::size_t kReplyMax = 4096;

  int noShell() {
    std::fputs("andromeda-askpass: no Andromeda shell session; run sudo in a terminal\n", stderr);
    return 1;
  }


  bool sendAll(int fd, std::string_view text) {
    while (!text.empty()) {
      const auto n = ::send(fd, text.data(), text.size(), MSG_NOSIGNAL);
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n <= 0) {
        return false;
      }
      text.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
  }

} // namespace

int main() {
  // getenv, not secure_getenv, under setgid: the environment only picks the socket, and the shell checks the peer.
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  if (runtime == nullptr || runtime[0] == '\0') {
    return noShell();
  }
  const char* display = std::getenv("WAYLAND_DISPLAY");
  if (display == nullptr || display[0] == '\0') {
    display = "wayland-0";
  }
  const std::string path = std::string(runtime) + "/noctalia-" + display + ".sock";

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    return noShell();
  }
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0 || ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
    return noShell();
  }
  // The prompt (argv[1]) is not sent: the shell reads the caller from /proc. The shell knows this helper by the
  // setgid group in SO_PEERCRED, then answers "verify".
  constexpr std::string_view kRequest = "askpass\n";
  if (!sendAll(fd, kRequest)) {
    ::close(fd);
    return noShell();
  }

  // One page, never reallocated, kept out of core dumps and wiped before exit.
  char* reply = static_cast<char*>(::mmap(nullptr, kReplyMax, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (reply == MAP_FAILED) {
    ::close(fd);
    return 1;
  }
  ::madvise(reply, kReplyMax, MADV_DONTDUMP);
  const auto finish = [reply](int code) {
    explicit_bzero(reply, kReplyMax);
    ::munmap(reply, kReplyMax);
    return code;
  };
  constexpr std::string_view kVerify = "verify\n";
  bool verified = false;
  std::size_t size = 0;
  pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
  for (;;) {
    const int ready = ::poll(&pfd, 1, kTimeoutMs);
    if (ready < 0 && errno == EINTR) {
      continue;
    }
    if (ready <= 0) {
      std::fputs("andromeda-askpass: no answer in 120 s\n", stderr);
      ::close(fd);
      return finish(1);
    }
    if (size == kReplyMax) {
      ::close(fd);
      return finish(1);
    }
    const auto n = ::read(fd, reply + size, kReplyMax - size);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      break;
    }
    size += static_cast<std::size_t>(n);
    if (!verified && std::string_view(reply, size).starts_with(kVerify)) {
      // The setgid exec already made it non-dumpable; this keeps it so when installed without the setgid bit.
      if (::prctl(PR_SET_DUMPABLE, 0) != 0 || !sendAll(fd, "ready\n")) {
        ::close(fd);
        return finish(1);
      }
      verified = true;
      std::memmove(reply, reply + kVerify.size(), size - kVerify.size());
      size -= kVerify.size();
    }
  }
  ::close(fd);

  const std::string_view text(reply, size);
  constexpr std::string_view kPassword = "password\n";
  if (!verified || !text.starts_with(kPassword) || !text.ends_with('\n')) {
    if (text.starts_with("error: ") && text.ends_with('\n')) {
      std::fputs("andromeda-askpass: ", stderr);
      std::fwrite(text.data() + 7, 1, text.size() - 7, stderr);
    }
    return finish(1);
  }
  // write(2), not stdio: no copy of the password in a stdio buffer.
  std::string_view out = text.substr(kPassword.size());
  while (!out.empty()) {
    const auto n = ::write(STDOUT_FILENO, out.data(), out.size());
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return finish(1);
    }
    out.remove_prefix(static_cast<std::size_t>(n));
  }
  return finish(0);
}
