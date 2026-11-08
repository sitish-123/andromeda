#pragma once

#include "core/timer_manager.h"

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

class AppLoop;
class WaylandConnection;
struct wl_surface;

namespace preview {
  class AudioPlayer;
  class HelperClient;
  class VideoView;
} // namespace preview

// Plays a video in place in the Gallery (mockup section 15 middle): frames from the sandboxed andromeda-preview
// helper on a subsurface of the window, sound through the lazily loaded audio module. The helper cannot seek, so the
// scrubber shows progress only.
class GalleryVideo {
public:
  struct Callbacks {
    std::function<wl_surface*()> parent;
    std::function<void()> redraw; // the window must commit (a new subsurface position)
    // About 4 times a second while playing; `progress` 0..1, `time` "0:17 / 0:42".
    std::function<void(bool playing, float progress, const std::string& time)> progress;
    std::function<void()> ended;
  };

  GalleryVideo(AppLoop& loop, WaylandConnection& wayland, Callbacks callbacks);
  ~GalleryVideo();
  GalleryVideo(const GalleryVideo&) = delete;
  GalleryVideo& operator=(const GalleryVideo&) = delete;

  // `rect` = x, y, width, height in window-local logical px.
  void play(const std::filesystem::path& video, bool sound, std::array<float, 4> rect);
  void place(std::array<float, 4> rect);
  void toggle();
  void stop();
  [[nodiscard]] bool active() const noexcept { return m_helper != nullptr; }

private:
  // `software`: wl_shm frames, for a compositor that refuses the helper's dmabufs.
  void start(const std::filesystem::path& video, bool sound, bool software);
  void tick();

  AppLoop& m_loop;
  WaylandConnection& m_wayland;
  Callbacks m_callbacks;
  std::unique_ptr<preview::HelperClient> m_helper;
  std::unique_ptr<preview::VideoView> m_video;
  std::unique_ptr<preview::AudioPlayer> m_audio;
  std::array<float, 4> m_rect{};
  std::uint32_t m_durationMs = 0;
  std::uint64_t m_generation = 0;
  Timer m_tick;
};
