#include "wayland/pipe_io.h"

#include <cerrno>
#include <poll.h>
#include <unistd.h>

namespace pipe_io {
  namespace {
    constexpr int kReadTimeoutMs = 5000;
  } // namespace

  // Past `maxBytes` the read gives nothing: a partial clipboard is worse than none.
  std::string readPipe(int fd, std::size_t maxBytes) {
    std::string bytes;
    char buf[4096];
    while (true) {
      pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
      const int ready = ::poll(&pfd, 1, kReadTimeoutMs);
      if (ready < 0 && errno == EINTR) {
        continue;
      }
      if (ready <= 0) {
        bytes.clear();
        break;
      }
      const ssize_t n = ::read(fd, buf, sizeof(buf));
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n <= 0) {
        break;
      }
      bytes.append(buf, static_cast<std::size_t>(n));
      if (bytes.size() > maxBytes) {
        bytes.clear();
        break;
      }
    }
    ::close(fd);
    return bytes;
  }

  void writeAll(int fd, const std::string& bytes) {
    std::size_t done = 0;
    while (done < bytes.size()) {
      const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n <= 0) {
        break;
      }
      done += static_cast<std::size_t>(n);
    }
  }
} // namespace pipe_io
