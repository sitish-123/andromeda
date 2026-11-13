#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <sys/resource.h>

namespace andromeda {

  // Tells why the main thread slept during a slow frame: phase times, thread rusage, blkio delay and pressure deltas.
  // Per frame it costs clock reads, one getrusage and two timerfd_settime; it reads files at most once per second, and
  // only on frames.
  class StallProbe {
  public:
    enum class Mark : uint8_t { BuildStart, BuildEnd, CommitEnd, Count };

    // Developer diagnostics: slow frames also write a stall report with stacks (protocols/stall_watch.h).
    static void setDeveloper(bool on);
#ifdef ANDROMEDA_TEST_IPC
    // Harness: the next frame sleeps `ms` on the main thread after the probe arms.
    static void stallNextFrame(int ms);
#endif
    void frameStart();
    void mark(Mark mark);
    // The log fields of a frame that took at least `thresholdMs`, or nothing for a fast frame.
    [[nodiscard]] std::optional<std::string> frameEnd(double thresholdMs);

  private:
    // Kernel totals read from files: -1 where a file is missing or unreadable.
    struct Totals {
      uint64_t atNs = 0;
      int64_t blkioTicks = -1;
      int64_t cgroupIoFullUs = -1;
      int64_t cgroupMemFullUs = -1;
      int64_t systemIoFullUs = -1;
    };

    Totals readTotals();

    uint64_t m_startNs = 0;
    uint64_t m_fenceStartNs = 0;
    rusage m_usage{};
    std::array<uint64_t, static_cast<size_t>(Mark::Count)> m_marks{};
    Totals m_baseline;
    std::optional<std::string> m_cgroupDir;
  };

} // namespace andromeda
