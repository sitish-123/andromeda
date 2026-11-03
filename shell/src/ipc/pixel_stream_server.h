#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

// Companion side of the pixel stream protocol (see PixelStreamClient) on one connected socket.
// Keeps one sealed memfd buffer: each REQUEST releases the last frame, so the buffer is free
// whenever a frame is due.
class PixelStreamServer {
public:
  // Takes ownership of `fd`, a connected SOCK_SEQPACKET socket.
  explicit PixelStreamServer(int fd);
  ~PixelStreamServer();
  PixelStreamServer(const PixelStreamServer&) = delete;
  PixelStreamServer& operator=(const PixelStreamServer&) = delete;

  [[nodiscard]] int fd() const noexcept { return m_fd; }
  // Reads pending messages. False once the shell closed the stream or broke the protocol.
  bool dispatch();
  // True while the shell waits for a frame.
  [[nodiscard]] bool requested() const noexcept { return m_requested; }
  // True once after the shell lost its copy (a repeated HELLO): send the current frame again.
  [[nodiscard]] bool takeResend() noexcept { return std::exchange(m_resend, false); }
  // Writable RGBA8 pixels (width * height * 4, top-down) for the next frame; empty on failure.
  [[nodiscard]] std::span<std::uint8_t> frameBuffer(std::uint32_t width, std::uint32_t height);
  // Hands the frame buffer to the shell. Needs requested().
  bool sendFrame();

  // The shell takes dmabuf buffers: it said version 2 and did not refuse them.
  [[nodiscard]] bool dmabufAllowed() const noexcept { return m_version >= 2 && !m_noDmabuf; }
  struct Dmabuf {
    int fd = -1;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t fourcc = 0;
    std::uint64_t modifier = 0;
    std::uint32_t stride = 0;
    std::uint32_t offset = 0;
  };
  // Attaches a dmabuf as buffer `id` (1..3; 0 is the memfd buffer). The fd stays the caller's.
  bool attachDmabuf(std::uint32_t id, const Dmabuf& buffer);
  // Shows dmabuf buffer `id`. Needs requested(); the shell owns it until a later frame is shown.
  bool sendDmabufFrame(std::uint32_t id);

private:
  void releaseBuffer();
  bool sendMessage(std::span<const std::uint8_t> payload, int passFd);

  int m_fd = -1;
  int m_memfd = -1;
  std::uint8_t* m_data = nullptr;
  std::size_t m_size = 0;
  std::uint32_t m_width = 0;
  std::uint32_t m_height = 0;
  bool m_attached = false;
  bool m_requested = false;
  bool m_hello = false;
  bool m_resend = false;
  std::uint32_t m_version = 0;
  bool m_noDmabuf = false;
};
