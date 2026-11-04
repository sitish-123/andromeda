#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

// Shell side of the pixel stream companion protocol (documented for plugin authors in
// docs/user/plugins/development/declarative-ui.mdx, "Pixel streams").
//
// The companion listens on a filesystem SOCK_SEQPACKET socket. All fields are little-endian
// uint32. The shell sends HELLO {1, version} once (again on an adopted socket, to ask for a
// resend) and REQUEST {2, 0} whenever it is ready for a frame. In version 2 a companion may
// attach GPU buffers with DMABUF {3, id, width, height, fourcc, modifier lo, modifier hi, stride,
// offset} carrying one dmabuf fd; a REQUEST then frees every buffer but the one last shown, and
// NO_DMABUF {3, 0} from the shell asks for memfd buffers instead, and PAUSE {4, 1|0} stops or
// resumes frames (decode and drawing) while nobody can see them. The companion attaches shared buffers with BUFFER {1, id, width, height} carrying one
// sealed memfd (RGBA8, tightly packed rows), and answers each REQUEST with FRAME {2, id}. The
// buffer named by a FRAME stays owned by the shell until its next REQUEST.
class PixelStreamClient {
public:
  struct Frame {
    // Null for a dmabuf frame.
    const std::uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    // A dmabuf frame: fd >= 0, still owned by the client. `serial` changes with every attach.
    int dmabufFd = -1;
    std::uint32_t fourcc = 0;
    std::uint64_t modifier = 0;
    std::uint32_t stride = 0;
    std::uint32_t offset = 0;
    std::uint64_t serial = 0;
  };
  using ChangeCallback = std::function<void()>;

  static constexpr std::uint32_t kProtocolVersion = 1;
  // Sent only on adopted sockets (the lock helper): plugin companions keep version 1.
  static constexpr std::uint32_t kDmabufProtocolVersion = 2;
  static constexpr std::size_t kMaxBuffers = 4;
  static constexpr std::uint32_t kMaxDimension = 4096;
  static constexpr std::size_t kMaxPixels = std::size_t{4096} * 1024;
  static constexpr std::chrono::milliseconds kRetryInterval{1000};

  PixelStreamClient() = default;
  ~PixelStreamClient();
  PixelStreamClient(const PixelStreamClient&) = delete;
  PixelStreamClient& operator=(const PixelStreamClient&) = delete;

  // Called after a new frame arrived or the connection dropped.
  void setChangeCallback(ChangeCallback callback) { m_onChange = std::move(callback); }

  // Returns false when the path is unchanged. A new path drops the connection; the first
  // connection attempt is due immediately. An empty path stops streaming.
  bool setPath(std::string path);
  [[nodiscard]] const std::string& path() const noexcept { return m_path; }
  // Streams over an already connected socket (one end of a socketpair) instead of a path, and
  // takes ownership of it. A drop is final: there is no path to reconnect to.
  void adopt(int fd);

  [[nodiscard]] int fd() const noexcept { return m_fd; }
  [[nodiscard]] bool connected() const noexcept { return m_fd >= 0; }
  // When the next connection attempt is due; empty while connected or without a path.
  [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> retryAt() const;
  // Attempts a connection once retryAt() has passed.
  void tick(std::chrono::steady_clock::time_point now);
  // Handles poll() results for fd().
  void dispatch(short revents);

  // The most recent frame, valid until releaseFrame(), a disconnect, or a path change.
  [[nodiscard]] std::optional<Frame> frame() const;
  // Returns the frame's buffer to the companion and requests the next frame. With a sync_file
  // fence (taken over), the REQUEST waits until the fence signals: a dmabuf REQUEST lets the
  // companion draw into buffers the consumer's GPU may still read.
  void releaseFrame(int fenceFd = -1);
  // The fence a released frame waits on, or -1; poll it for POLLIN.
  [[nodiscard]] int releaseFence() const noexcept { return m_releaseFence; }
  // Called when releaseFence() is readable: sends the deferred REQUEST.
  void dispatchReleaseFence();
  // Asks the companion for memfd buffers from now on (the dmabuf could not be imported).
  void refuseDmabuf();
  // Sends PAUSE on an adopted (version 2) socket, now and after each adopt().
  void setPaused(bool paused);
  // Drops the connection and reconnects immediately; a new connection receives the current
  // frame right away. Used when the consumer lost its copy (graphics context reset). An adopted
  // socket stays open and sends HELLO again instead, which asks the companion to resend.
  void reconnect();

private:
  struct Buffer {
    void* data = nullptr;
    std::size_t size = 0;
    int width = 0;
    int height = 0;
    int dmabufFd = -1;
    std::uint32_t fourcc = 0;
    std::uint64_t modifier = 0;
    std::uint32_t stride = 0;
    std::uint32_t offset = 0;
    std::uint64_t serial = 0;
  };

  void connect();
  void disconnect();
  void fail(std::string_view reason);
  bool send(std::uint32_t type, std::uint32_t argument);
  // Reads one message. Returns false when the socket has nothing more to read or failed.
  bool readMessage();
  void attachBuffer(std::uint32_t id, std::uint32_t width, std::uint32_t height, int fd);
  void attachDmabuf(
      std::uint32_t id, std::uint32_t width, std::uint32_t height, std::uint32_t fourcc, std::uint64_t modifier,
      std::uint32_t stride, std::uint32_t offset, int fd
  );
  void unmap(Buffer& buffer);
  void notify();
  void closeReleaseFence();

  std::string m_path;
  int m_fd = -1;
  std::chrono::steady_clock::time_point m_retryAt;
  std::array<Buffer, kMaxBuffers> m_buffers{};
  std::optional<std::uint32_t> m_frameBuffer;
  bool m_requested = false;
  int m_releaseFence = -1;
  bool m_paused = false;
  std::uint64_t m_serial = 0;
  ChangeCallback m_onChange;
};
