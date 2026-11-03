#pragma once

#include "ipc/pixel_stream_server.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

using GstElement = struct _GstElement;
using GstSample = struct _GstSample;

// Plays a video file in a loop in the lock helper. With `dmabuf`, VA decodes into single-plane
// RGBA dmabufs (vapostproc); without VA, or when that path fails before its first frame, the
// decode runs in software into RGBA memory. GStreamer threads only write to wakeFd(); every
// other call belongs to the helper thread.
class LockVideo {
public:
  // Throws std::runtime_error when no pipeline can start.
  LockVideo(const std::string& path, bool dmabuf);
  ~LockVideo();
  LockVideo(const LockVideo&) = delete;
  LockVideo& operator=(const LockVideo&) = delete;

  // Readable when a frame or a pipeline message waits.
  [[nodiscard]] int wakeFd() const noexcept { return m_wakeFd; }
  // Handles pipeline messages: loops at the end, falls back to software after an early VA error.
  // False once the video cannot play any more.
  bool dispatch();

  struct Frame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // Set for a VA frame; otherwise `pixels` holds top-down RGBA rows of width * 4 bytes.
    std::optional<PixelStreamServer::Dmabuf> dmabuf;
    std::span<const std::uint8_t> pixels;
  };
  // The newest frame since the last call; it stays valid until the next call.
  std::optional<Frame> takeFrame();
  // Restarts in software decode (the dmabuf cannot be imported). False when already in software.
  bool fallBack();
  void setPaused(bool paused);
  [[nodiscard]] bool usesDmabuf() const noexcept { return m_dmabuf; }

  // LockShader source that covers the target with iChannel0 (the frame), cropping the overflow like a wallpaper.
  static constexpr const char* kCoverShader = R"(
void mainImage(out vec4 fragColor, in vec2 fragCoord) {
  vec2 size = max(iChannelResolution[0].xy, vec2(1.0));
  float scale = max(iResolution.x / size.x, iResolution.y / size.y);
  vec2 uv = ((fragCoord - iResolution.xy * 0.5) / scale + size * 0.5) / size;
  fragColor = texture2D(iChannel0, vec2(uv.x, 1.0 - uv.y));
}
)";

private:
  void start();
  void stop() noexcept;
  void releaseSample() noexcept;

  std::string m_path;
  bool m_dmabuf = false;
  bool m_paused = false;
  bool m_hadFrame = false;
  int m_wakeFd = -1;
  GstElement* m_pipeline = nullptr;
  GstElement* m_sink = nullptr;
  std::uint32_t m_fourcc = 0;
  std::uint64_t m_modifier = 0;
  GstSample* m_sample = nullptr;
  std::vector<std::uint8_t> m_pixels;
};
