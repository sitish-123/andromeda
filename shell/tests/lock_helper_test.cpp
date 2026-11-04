#include "ipc/pixel_stream_client.h"
#include "tests/test_check.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <format>
#include <poll.h>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

  constexpr int kSlots = 4;
  using Clock = std::chrono::steady_clock;

  std::string writeTemp(const std::string& text) {
    char path[] = "/tmp/lock-helper-test-XXXXXX";
    const int fd = ::mkstemp(path);
    TEST_CHECK(fd >= 0);
    TEST_CHECK(::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()));
    ::close(fd);
    return path;
  }

  // Polls until `done` holds or the deadline passes.
  template <typename Done> void pump(std::array<PixelStreamClient, kSlots>& clients, Clock::time_point deadline, Done done) {
    while (!done() && Clock::now() < deadline) {
      std::array<pollfd, kSlots> fds{};
      for (int i = 0; i < kSlots; ++i) {
        fds[i] = {.fd = clients[i].fd(), .events = POLLIN, .revents = 0};
      }
      if (::poll(fds.data(), fds.size(), 200) > 0) {
        for (int i = 0; i < kSlots; ++i) {
          clients[i].dispatch(fds[i].revents);
        }
      }
    }
  }

  // The pixel at the centre of slot `i`'s current frame, or empty.
  const std::uint8_t* centre(const PixelStreamClient& client) {
    const auto frame = client.frame();
    return frame.has_value() && frame->pixels != nullptr ? frame->pixels + ((frame->height / 2) * frame->width + frame->width / 2) * 4 : nullptr;
  }

  // Pixel (x, y) of slot `i`'s current memfd frame, or empty.
  const std::uint8_t* pixel(const PixelStreamClient& client, std::uint32_t x, std::uint32_t y) {
    const auto frame = client.frame();
    return frame.has_value() && frame->pixels != nullptr ? frame->pixels + (y * frame->width + x) * 4 : nullptr;
  }

} // namespace

// Spawns `noctalia lock-helper` with a plugin slot, a shader slot, a shader that does not compile and a
// video slot (tests/lock_helper/bluered.webm: blue top half, red bottom half).
// Args: <noctalia binary> <plugin source root>.
int main(int argc, char* argv[]) {
  TEST_CHECK(argc == 3);
  const std::string green = writeTemp(
      "void mainImage(out vec4 fragColor, in vec2 fragCoord) { fragColor = vec4(0.0, 1.0, 0.0, 1.0); }\n"
  );
  const std::string broken = writeTemp("void mainImage(out vec4 fragColor, in vec2 fragCoord) { nope }\n");

  std::array<std::array<int, 2>, kSlots> pairs{};
  for (auto& pair : pairs) {
    TEST_CHECK(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair.data()) == 0);
  }
  int alive[2] = {-1, -1};
  TEST_CHECK(::pipe2(alive, O_CLOEXEC) == 0);

  const std::string start = std::format(
      "dark = true\nplugin_sources = [\"{}\"]\n"
      "[[slot]]\nfd = 4\ntype = \"me/lockbox:box\"\nwidth = 40\nheight = 40\nscale = 1.0\n"
      "[[slot]]\nfd = 5\ntype = \"shader\"\nwidth = 32\nheight = 16\nscale = 2.0\n[slot.settings]\npath = \"{}\"\n"
      "[[slot]]\nfd = 6\ntype = \"shader\"\nwidth = 16\nheight = 16\nscale = 1.0\n[slot.settings]\npath = \"{}\"\n"
      "[[slot]]\nfd = 7\ntype = \"video\"\nwidth = 32\nheight = 16\nscale = 1.0\n[slot.settings]\n"
      "path = \"{}/bluered.webm\"\n",
      argv[2], green, broken, argv[2]
  );
  const int startFd = ::memfd_create("start", 0);
  TEST_CHECK(::write(startFd, start.data(), start.size()) == static_cast<ssize_t>(start.size()));
  TEST_CHECK(::lseek(startFd, 0, SEEK_SET) == 0);

  const pid_t pid = ::fork();
  TEST_CHECK(pid >= 0);
  if (pid == 0) {
    // dup2 onto the same number keeps FD_CLOEXEC, so move every fd above the targets first.
    const int startCopy = ::fcntl(startFd, F_DUPFD_CLOEXEC, 16);
    std::array<int, kSlots> copies{};
    for (int i = 0; i < kSlots; ++i) {
      copies[i] = ::fcntl(pairs[i][1], F_DUPFD_CLOEXEC, 16);
    }
    ::dup2(alive[0], STDIN_FILENO);
    ::dup2(startCopy, 3);
    for (int i = 0; i < kSlots; ++i) {
      ::dup2(copies[i], 4 + i);
    }
    ::execl(argv[1], argv[1], "lock-helper", "--start-fd", "3", nullptr);
    ::_exit(127);
  }
  for (auto& pair : pairs) {
    ::close(pair[1]);
  }
  ::close(alive[0]);
  ::close(startFd);

  std::array<PixelStreamClient, kSlots> clients;
  for (int i = 0; i < kSlots; ++i) {
    clients[i].adopt(pairs[i][0]);
  }
  const auto deadline = Clock::now() + std::chrono::seconds(20);
  pump(clients, deadline, [&] { return centre(clients[0]) != nullptr && clients[1].frame().has_value(); });

  const std::uint8_t* box = centre(clients[0]);
  TEST_CHECK(box != nullptr && box[0] > 200 && box[3] > 200);
  // The shader slot renders at its buffer size (logical size x scale), as a dmabuf where the driver exports one.
  const auto shaderFrame = clients[1].frame();
  TEST_CHECK(shaderFrame.has_value() && shaderFrame->width == 64 && shaderFrame->height == 32);
  if (shaderFrame->dmabufFd >= 0) {
    TEST_CHECK(shaderFrame->stride >= 64 * 4);
    // A shell that cannot import it gets memfd frames from then on.
    clients[1].refuseDmabuf();
    clients[1].releaseFrame();
    pump(clients, deadline, [&] { return centre(clients[1]) != nullptr; });
  }
  const std::uint8_t* shader = centre(clients[1]);
  TEST_CHECK(shader != nullptr);
  TEST_CHECK(shader[0] < 30 && shader[1] > 220 && shader[3] == 255);
  // A shader that does not compile keeps its stream open and sends nothing.
  TEST_CHECK(clients[2].connected() && !clients[2].frame().has_value());

  // The box never changes, so only a resend after reconnect() brings another frame.
  clients[0].releaseFrame();
  clients[0].reconnect();
  pump(clients, deadline, [&] { return centre(clients[0]) != nullptr; });
  TEST_CHECK(centre(clients[0]) != nullptr);

  // The shader animates: a released frame brings the next one.
  clients[1].releaseFrame();
  pump(clients, deadline, [&] { return centre(clients[1]) != nullptr; });
  TEST_CHECK(centre(clients[1]) != nullptr);

  // The video slot shows decoded frames upright, scaled to the slot.
  pump(clients, deadline, [&] { return clients[3].frame().has_value(); });
  if (const auto video = clients[3].frame(); video.has_value() && video->dmabufFd >= 0) {
    clients[3].refuseDmabuf();
    clients[3].releaseFrame();
    pump(clients, deadline, [&] { return pixel(clients[3], 0, 0) != nullptr; });
  }
  const std::uint8_t* top = pixel(clients[3], 16, 3);
  const std::uint8_t* bottom = pixel(clients[3], 16, 12);
  TEST_CHECK(top != nullptr && bottom != nullptr);
  TEST_CHECK(top[2] > 180 && top[0] < 80);
  TEST_CHECK(bottom[0] > 180 && bottom[2] < 80);

  // A paused slot sends nothing, even when asked; resuming brings the next frame.
  clients[1].setPaused(true);
  clients[1].releaseFrame();
  pump(clients, Clock::now() + std::chrono::milliseconds(300), [&] { return clients[1].frame().has_value(); });
  TEST_CHECK(!clients[1].frame().has_value());
  clients[1].setPaused(false);
  pump(clients, deadline, [&] { return clients[1].frame().has_value(); });
  TEST_CHECK(clients[1].frame().has_value());

  // Stdin EOF ends the helper.
  ::close(alive[1]);
  int status = 0;
  TEST_CHECK(::waitpid(pid, &status, 0) == pid);
  TEST_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  ::unlink(green.c_str());
  ::unlink(broken.c_str());
  return 0;
}
