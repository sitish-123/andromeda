#pragma once

#include "core/poll_source.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <sys/types.h>

namespace preview {

  // Wire format of andromeda-preview-helper (andromeda-preview/helper/src/proto.rs).
  struct HelperMsg {
    std::uint32_t op = 0;
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t c = 0;
    std::uint32_t d = 0;
    std::uint32_t e = 0;
  };

  namespace op {
    inline constexpr std::uint32_t Info = 1;
    inline constexpr std::uint32_t Render = 2;
    inline constexpr std::uint32_t Text = 3;
    inline constexpr std::uint32_t Frame = 4;
    // a = frame id. No reply.
    inline constexpr std::uint32_t Release = 5;
    inline constexpr std::uint32_t InfoReply = 0x81;
    inline constexpr std::uint32_t Page = 0x82;
    inline constexpr std::uint32_t TextReply = 0x83;
    // a = width, b = height, c = DRM fourcc, d = planes, e = frame id; payload: u64 modifier, u64 pts ns, then per
    // plane u32 fd index, offset, stride; fds = the plane buffers.
    inline constexpr std::uint32_t FrameReply = 0x84;
    inline constexpr std::uint32_t End = 0x85;
    inline constexpr std::uint32_t Error = 0xff;
  } // namespace op

  enum class HelperKind : std::uint8_t { Text, Pdf, Media, MediaSoftware };

  // Closes the fd when the last copy goes away.
  class SharedFd {
  public:
    explicit SharedFd(int fd);
    ~SharedFd();
    SharedFd(const SharedFd&) = delete;
    SharedFd& operator=(const SharedFd&) = delete;
    [[nodiscard]] int get() const noexcept { return m_fd; }

  private:
    int m_fd;
  };

  // A read-only mapping of a memfd the helper sent.
  class MappedFd {
  public:
    static std::shared_ptr<MappedFd> map(int fd);
    ~MappedFd();
    MappedFd(const MappedFd&) = delete;
    MappedFd& operator=(const MappedFd&) = delete;

    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept { return {m_data, m_size}; }

  private:
    MappedFd(const std::uint8_t* data, std::size_t size) : m_data(data), m_size(size) {}
    const std::uint8_t* m_data;
    std::size_t m_size;
  };

  struct HelperReply {
    HelperMsg msg;
    // The one fd of a page or text reply, mapped.
    std::shared_ptr<MappedFd> data;
    // Frame replies: the payload after the header and the plane buffers, not mapped.
    std::vector<std::uint8_t> payload;
    std::vector<std::shared_ptr<SharedFd>> fds;
    [[nodiscard]] bool ok() const noexcept { return msg.op != 0 && msg.op != op::Error; }
  };

  // One sandboxed decoder process for one file. Requests are answered in order; a reply with op 0 means the
  // helper died or broke the protocol.
  class HelperClient final : public PollSource {
  public:
    using ReplyCallback = std::function<void(const HelperReply&)>;

    ~HelperClient() override;
    HelperClient(const HelperClient&) = delete;
    HelperClient& operator=(const HelperClient&) = delete;

    // Starts the helper on `path`; nullptr when it cannot start.
    static std::unique_ptr<HelperClient> start(HelperKind kind, const std::string& path);

    void request(const HelperMsg& msg, ReplyCallback callback);
    // A message the helper answers with nothing (Release).
    void notify(const HelperMsg& msg);
    [[nodiscard]] bool alive() const noexcept { return m_fd >= 0; }

    // Blocks until every queued request is answered. For tests and the first frame of a cold start.
    void drain();

    [[nodiscard]] int pollTimeoutMs() const override;
    void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

    // A request that takes longer kills the helper (a decoder loop on a hostile file).
    static constexpr std::chrono::seconds kRequestTimeout{10};

  protected:
    void doAddPollFds(std::vector<pollfd>& fds) override;

  private:
    HelperClient(int fd, pid_t pid) : m_fd(fd), m_pid(pid) {}
    void readOne();
    void fail();

    int m_fd;
    pid_t m_pid;
    std::deque<ReplyCallback> m_pending;
    std::chrono::steady_clock::time_point m_deadline{};
  };

  // The helper binary: $ANDROMEDA_PREVIEW_HELPER, else the installed libexec path.
  [[nodiscard]] std::string helperPath();

} // namespace preview
