#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct wl_surface;
struct wp_presentation;
struct wp_presentation_feedback;

// DESK_FRAME_STATS=1: one line per animation run of a surface, from wp_presentation feedback:
// "anim <name>#<run> frames N p99_cpu_ms X missed M". A run starts at the first frame rendered while springs are
// active and ends when the first frame after them is presented. CPU = tick + prepare + render time of each frame.
class FrameStats {
public:
  [[nodiscard]] static bool enabled();

  FrameStats(wp_presentation* presentation, std::string name);
  ~FrameStats();
  FrameStats(const FrameStats&) = delete;
  FrameStats& operator=(const FrameStats&) = delete;

  void addCpu(float ms) noexcept { m_cpuMs += ms; }
  // Before the commit of a frame.
  void beginFrame(wl_surface* surface, bool animating);
  // After the frame was rendered (its CPU time is complete).
  void endFrame();

  void onPresented(wp_presentation_feedback* feedback, std::uint64_t presentedNs, std::uint32_t refreshNs);
  void onDiscarded(wp_presentation_feedback* feedback);

private:
  void finishIfDone();

  wp_presentation* m_presentation;
  std::string m_name;
  std::vector<wp_presentation_feedback*> m_pending;
  std::vector<float> m_cpu;
  float m_cpuMs = 0.0F;
  bool m_active = false;
  bool m_ending = false;
  bool m_frameInRun = false;
  std::uint64_t m_lastPresentedNs = 0;
  std::uint32_t m_missed = 0;
  std::uint32_t m_presented = 0;
  std::uint32_t m_run = 0;
};
