#include "ipc/pixel_stream_server.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

  enum : std::uint32_t {
    kHello = 1,
    kRequest = 2,
  };

  enum : std::uint32_t {
    kBuffer = 1,
    kFrame = 2,
  };

  void writeLe32(std::uint8_t* out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
      out[i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
  }

  std::uint32_t readLe32(const std::uint8_t* in) {
    return std::uint32_t{in[0]} | std::uint32_t{in[1]} << 8 | std::uint32_t{in[2]} << 16 | std::uint32_t{in[3]} << 24;
  }

} // namespace

PixelStreamServer::PixelStreamServer(int fd) : m_fd(fd) {}

PixelStreamServer::~PixelStreamServer() {
  releaseBuffer();
  if (m_fd >= 0) {
    ::close(m_fd);
  }
}

bool PixelStreamServer::dispatch() {
  while (true) {
    std::array<std::uint8_t, 9> message{};
    const ssize_t count = ::recv(m_fd, message.data(), message.size(), MSG_DONTWAIT);
    if (count < 0) {
      return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
    }
    if (count != 8) {
      return false;
    }
    const std::uint32_t type = readLe32(message.data());
    if (type == kRequest) {
      m_requested = true;
    } else if (type == kHello) {
      m_resend = std::exchange(m_hello, true);
    } else {
      return false;
    }
  }
}

std::span<std::uint8_t> PixelStreamServer::frameBuffer(std::uint32_t width, std::uint32_t height) {
  if (width == m_width && height == m_height && m_data != nullptr) {
    return {m_data, m_size};
  }
  releaseBuffer();
  const std::size_t size = std::size_t{width} * height * 4;
  m_memfd = ::memfd_create("lock-helper-frame", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (m_memfd < 0 || size == 0 || ::ftruncate(m_memfd, static_cast<off_t>(size)) != 0
      || ::fcntl(m_memfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0) {
    releaseBuffer();
    return {};
  }
  void* data = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, m_memfd, 0);
  if (data == MAP_FAILED) {
    releaseBuffer();
    return {};
  }
  m_data = static_cast<std::uint8_t*>(data);
  m_size = size;
  m_width = width;
  m_height = height;
  return {m_data, m_size};
}

bool PixelStreamServer::sendFrame() {
  if (!m_requested || m_data == nullptr) {
    return false;
  }
  if (!m_attached) {
    std::array<std::uint8_t, 16> buffer{};
    writeLe32(buffer.data(), kBuffer);
    writeLe32(buffer.data() + 4, 0);
    writeLe32(buffer.data() + 8, m_width);
    writeLe32(buffer.data() + 12, m_height);
    if (!sendMessage(buffer, m_memfd)) {
      return false;
    }
    m_attached = true;
  }
  std::array<std::uint8_t, 8> frame{};
  writeLe32(frame.data(), kFrame);
  writeLe32(frame.data() + 4, 0);
  if (!sendMessage(frame, -1)) {
    return false;
  }
  m_requested = false;
  return true;
}

void PixelStreamServer::releaseBuffer() {
  if (m_data != nullptr) {
    ::munmap(m_data, m_size);
  }
  if (m_memfd >= 0) {
    ::close(m_memfd);
  }
  m_memfd = -1;
  m_data = nullptr;
  m_size = 0;
  m_width = 0;
  m_height = 0;
  m_attached = false;
}

bool PixelStreamServer::sendMessage(std::span<const std::uint8_t> payload, int passFd) {
  iovec vector{.iov_base = const_cast<std::uint8_t*>(payload.data()), .iov_len = payload.size()};
  msghdr message{};
  message.msg_iov = &vector;
  message.msg_iovlen = 1;
  alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> control{};
  if (passFd >= 0) {
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    cmsghdr* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(header), &passFd, sizeof(int));
  }
  return ::sendmsg(m_fd, &message, MSG_NOSIGNAL) == static_cast<ssize_t>(payload.size());
}
