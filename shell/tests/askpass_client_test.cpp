#include "tests/test_check.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

  namespace fs = std::filesystem;

  struct Result {
    int exitCode = -1;
    std::string out;
    std::string err;
  };

  std::string readAll(int fd) {
    std::string text;
    char buf[256];
    ssize_t n = 0;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0) {
      text.append(buf, static_cast<std::size_t>(n));
    }
    return text;
  }

  std::string readLine(int fd) {
    std::string line;
    char c = 0;
    while (::read(fd, &c, 1) == 1) {
      line += c;
      if (c == '\n') {
        break;
      }
    }
    return line;
  }

  // Runs the askpass binary; when `listenFd` is open, answers its request with `reply`, after the handshake when
  // `handshake` is set.
  Result run(const char* exe, int listenFd, std::string_view reply, bool handshake = true) {
    int out[2];
    int err[2];
    TEST_CHECK(::pipe(out) == 0 && ::pipe(err) == 0);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, err[1], STDERR_FILENO);
    char prompt[] = "[sudo] password for user: ";
    char* argv[] = {const_cast<char*>(exe), prompt, nullptr};
    pid_t pid = 0;
    TEST_CHECK(::posix_spawn(&pid, exe, &actions, nullptr, argv, environ) == 0);
    posix_spawn_file_actions_destroy(&actions);
    ::close(out[1]);
    ::close(err[1]);

    if (listenFd >= 0) {
      pollfd pfd{.fd = listenFd, .events = POLLIN, .revents = 0};
      TEST_CHECK(::poll(&pfd, 1, 5000) == 1);
      const int conn = ::accept(listenFd, nullptr, nullptr);
      TEST_CHECK(conn >= 0);
      TEST_CHECK(readLine(conn) == "askpass\n");
      if (handshake) {
        TEST_CHECK(::write(conn, "verify\n", 7) == 7);
        TEST_CHECK(readLine(conn) == "ready\n");
      }
      TEST_CHECK(::write(conn, reply.data(), reply.size()) == static_cast<ssize_t>(reply.size()));
      ::close(conn);
    }

    Result result;
    result.out = readAll(out[0]);
    result.err = readAll(err[0]);
    ::close(out[0]);
    ::close(err[0]);
    int status = 0;
    TEST_CHECK(::waitpid(pid, &status, 0) == pid);
    TEST_CHECK(WIFEXITED(status));
    result.exitCode = WEXITSTATUS(status);
    return result;
  }

} // namespace

int main(int argc, char** argv) {
  TEST_CHECK(argc == 2);
  const char* exe = argv[1];
  std::string pattern = (fs::temp_directory_path() / "andromeda-askpass-client-XXXXXX").string();
  const fs::path dir(::mkdtemp(pattern.data()));
  TEST_CHECK(::setenv("XDG_RUNTIME_DIR", dir.c_str(), 1) == 0);
  TEST_CHECK(::setenv("WAYLAND_DISPLAY", "askpass-test", 1) == 0);

  const std::string path = (dir / "noctalia-askpass-test.sock").string();
  const int listenFd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  TEST_CHECK(::bind(listenFd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
  TEST_CHECK(::listen(listenFd, 4) == 0);

  const Result answered = run(exe, listenFd, "password\nhunter 2\n");
  TEST_CHECK(answered.exitCode == 0);
  TEST_CHECK(answered.out == "hunter 2\n");

  const Result cancelled = run(exe, listenFd, "cancel\n");
  TEST_CHECK(cancelled.exitCode == 1);
  TEST_CHECK(cancelled.out.empty());

  const Result busy = run(exe, listenFd, "error: another password request is open\n", false);
  TEST_CHECK(busy.exitCode == 1);
  TEST_CHECK(busy.out.empty());
  TEST_CHECK(busy.err.find("another password request is open") != std::string::npos);

  ::close(listenFd);
  ::unlink(path.c_str());
  const Result noShell = run(exe, -1, {});
  TEST_CHECK(noShell.exitCode == 1);
  TEST_CHECK(noShell.out.empty());
  TEST_CHECK(noShell.err.find("no Andromeda shell session; run sudo in a terminal") != std::string::npos);

  fs::remove_all(dir);
  return 0;
}
