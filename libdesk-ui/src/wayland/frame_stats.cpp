#include "wayland/frame_stats.h"

#include "core/log.h"
#include "presentation-time-client-protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace {
  constexpr Logger kLog("frame-stats");

  void handlePresented(
      void* data, struct wp_presentation_feedback* feedback, std::uint32_t secHi, std::uint32_t secLo, std::uint32_t nsec,
      std::uint32_t refresh, std::uint32_t /*seqHi*/, std::uint32_t /*seqLo*/, std::uint32_t /*flags*/
  ) {
    const std::uint64_t sec = (static_cast<std::uint64_t>(secHi) << 32U) | secLo;
    static_cast<FrameStats*>(data)->onPresented(feedback, sec * 1'000'000'000ULL + nsec, refresh);
  }

  void handleDiscarded(void* data, struct wp_presentation_feedback* feedback) {
    static_cast<FrameStats*>(data)->onDiscarded(feedback);
  }

  void handleSyncOutput(void* /*data*/, struct wp_presentation_feedback* /*feedback*/, wl_output* /*output*/) {}

  const wp_presentation_feedback_listener kListener = {
      .sync_output = handleSyncOutput,
      .presented = handlePresented,
      .discarded = handleDiscarded,
  };
} // namespace

bool FrameStats::enabled() {
  static const bool on = [] {
    const char* env = std::getenv("DESK_FRAME_STATS");
    return env != nullptr && env[0] != '\0' && env[0] != '0';
  }();
  return on;
}

FrameStats::FrameStats(wp_presentation* presentation, std::string name)
    : m_presentation(presentation), m_name(std::move(name)) {}

FrameStats::~FrameStats() {
  for (struct wp_presentation_feedback* feedback : m_pending) {
    wp_presentation_feedback_destroy(feedback);
  }
}

void FrameStats::beginFrame(wl_surface* surface, bool animating) {
  if (!m_active && !animating) {
    m_frameInRun = false;
    return;
  }
  if (!m_active) {
    m_active = true;
    m_ending = false;
    m_cpu.clear();
    m_missed = 0;
    m_presented = 0;
    m_lastPresentedNs = 0;
    ++m_run;
  }
  m_ending = m_ending || !animating;
  m_frameInRun = true;
  struct wp_presentation_feedback* feedback = wp_presentation_feedback(m_presentation, surface);
  wp_presentation_feedback_add_listener(feedback, &kListener, this);
  m_pending.push_back(feedback);
}

void FrameStats::endFrame() {
  if (m_frameInRun) {
    m_cpu.push_back(m_cpuMs);
  }
  m_cpuMs = 0.0F;
  m_frameInRun = false;
}

void FrameStats::onPresented(struct wp_presentation_feedback* feedback, std::uint64_t presentedNs, std::uint32_t refreshNs) {
  std::erase(m_pending, feedback);
  wp_presentation_feedback_destroy(feedback);
  if (m_lastPresentedNs != 0 && refreshNs != 0 && presentedNs > m_lastPresentedNs) {
    const double intervals = static_cast<double>(presentedNs - m_lastPresentedNs) / refreshNs;
    m_missed += static_cast<std::uint32_t>(std::max(0.0, std::round(intervals) - 1.0));
  }
  m_lastPresentedNs = presentedNs;
  ++m_presented;
  finishIfDone();
}

void FrameStats::onDiscarded(struct wp_presentation_feedback* feedback) {
  std::erase(m_pending, feedback);
  wp_presentation_feedback_destroy(feedback);
  ++m_missed;
  finishIfDone();
}

void FrameStats::finishIfDone() {
  if (!m_active || !m_ending || !m_pending.empty()) {
    return;
  }
  std::vector<float> cpu = m_cpu;
  float p99 = 0.0F;
  if (!cpu.empty()) {
    const auto rank = static_cast<std::size_t>(std::ceil(0.99 * static_cast<double>(cpu.size()))) - 1;
    std::ranges::nth_element(cpu, cpu.begin() + static_cast<std::ptrdiff_t>(rank));
    p99 = cpu[rank];
  }
  kLog.info("anim {}#{} frames {} p99_cpu_ms {:.2f} missed {}", m_name, m_run, m_presented, p99, m_missed);
  m_active = false;
  m_ending = false;
}
