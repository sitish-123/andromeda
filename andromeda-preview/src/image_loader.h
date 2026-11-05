#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>

namespace preview {

  // Decoded pixels: straight-alpha RGBA8 for images (libdesk-ui blends straight alpha).
  struct Pixels {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    std::shared_ptr<const void> owner;
    std::span<const std::uint8_t> bytes;
  };

  // Decodes the first frame of an image through glycin, whose loaders run in their own sandbox, on one worker
  // thread. Only the newest request runs: a request made while another decodes replaces any queued one, so
  // holding an arrow key through a folder never piles up decodes. `done` runs on the main loop, with empty
  // pixels on failure. A frame larger than the box is scaled down by the loader.
  class ImageDecoder {
  public:
    ImageDecoder();
    // Waits for the decode in progress (glycin's sync API cannot be cancelled).
    ~ImageDecoder();
    ImageDecoder(const ImageDecoder&) = delete;
    ImageDecoder& operator=(const ImageDecoder&) = delete;

    void request(std::string path, std::uint32_t maxWidth, std::uint32_t maxHeight, std::function<void(Pixels)> done);
    // Drops a queued request; a decode in progress still finishes, and its result is dropped.
    void cancel();

  private:
    struct Job {
      std::string path;
      std::uint32_t maxWidth = 0;
      std::uint32_t maxHeight = 0;
      std::function<void(Pixels)> done;
      std::uint64_t serial = 0;
    };
    void run();

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::optional<Job> m_queued;
    std::shared_ptr<std::atomic<std::uint64_t>> m_latest = std::make_shared<std::atomic<std::uint64_t>>(0);
    bool m_stop = false;
    std::thread m_thread;
  };

} // namespace preview
