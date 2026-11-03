#include "ipc/pixel_stream_client.h"
#include "tests/test_check.h"

#include <chrono>
#include <fcntl.h>
#include <cstdint>
#include <format>
#include <poll.h>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

// Spawns `noctalia lock-helper` with one plugin slot and reads its first frame.
// Args: <noctalia binary> <plugin source root>.
int main(int argc, char* argv[]) {
  TEST_CHECK(argc == 3);
  int slot[2] = {-1, -1};
  TEST_CHECK(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, slot) == 0);
  int alive[2] = {-1, -1};
  TEST_CHECK(::pipe2(alive, O_CLOEXEC) == 0);

  const std::string start = std::format(
      "dark = true\nplugin_sources = [\"{}\"]\n[[slot]]\nfd = 4\ntype = \"me/lockbox:box\"\n"
      "width = 40\nheight = 40\nscale = 1.0\n",
      argv[2]
  );
  const int startFd = ::memfd_create("start", 0);
  TEST_CHECK(::write(startFd, start.data(), start.size()) == static_cast<ssize_t>(start.size()));
  TEST_CHECK(::lseek(startFd, 0, SEEK_SET) == 0);

  const pid_t pid = ::fork();
  TEST_CHECK(pid >= 0);
  if (pid == 0) {
    // dup2 onto the same number keeps FD_CLOEXEC, so move both fds out of 3-4 first.
    const int startCopy = ::fcntl(startFd, F_DUPFD_CLOEXEC, 10);
    const int slotCopy = ::fcntl(slot[1], F_DUPFD_CLOEXEC, 10);
    ::dup2(alive[0], STDIN_FILENO);
    ::dup2(startCopy, 3);
    ::dup2(slotCopy, 4);
    ::execl(argv[1], argv[1], "lock-helper", "--start-fd", "3", nullptr);
    ::_exit(127);
  }
  ::close(slot[1]);
  ::close(alive[0]);
  ::close(startFd);

  PixelStreamClient client;
  client.adopt(slot[0]);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  bool painted = false;
  while (!painted && client.connected() && std::chrono::steady_clock::now() < deadline) {
    pollfd fd{.fd = client.fd(), .events = POLLIN, .revents = 0};
    if (::poll(&fd, 1, 500) > 0) {
      client.dispatch(fd.revents);
    }
    if (const auto frame = client.frame(); frame.has_value()) {
      TEST_CHECK(frame->width == 40 && frame->height == 40);
      const std::uint8_t* centre = frame->pixels + (20 * 40 + 20) * 4;
      painted = centre[0] > 200 && centre[3] > 200;
      client.releaseFrame();
    }
  }
  TEST_CHECK(painted);

  // The box never changes, so only a resend after reconnect() brings another frame.
  client.reconnect();
  bool resent = false;
  while (!resent && client.connected() && std::chrono::steady_clock::now() < deadline) {
    pollfd fd{.fd = client.fd(), .events = POLLIN, .revents = 0};
    if (::poll(&fd, 1, 500) > 0) {
      client.dispatch(fd.revents);
    }
    resent = client.frame().has_value();
  }
  TEST_CHECK(resent);

  // Stdin EOF ends the helper.
  ::close(alive[1]);
  int status = 0;
  TEST_CHECK(::waitpid(pid, &status, 0) == pid);
  TEST_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  return 0;
}
