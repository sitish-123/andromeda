#include "output/stall_probe.h"

#include <andromedafx/render/frame_stats.h>
#include "protocols/stall_watch.h"

#include <atomic>
#include <charconv>
#include <cerrno>
#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <format>
#include <mutex>
#include <pthread.h>
#include <string_view>
#include <sys/timerfd.h>
#include <thread>
#include <utility>
#include <unistd.h>

namespace andromeda {

  namespace {

    constexpr uint64_t kSampleIntervalNs = 1'000'000'000;

    uint64_t monotonicNs() {
      timespec now{};
      clock_gettime(CLOCK_MONOTONIC, &now);
      return static_cast<uint64_t>(now.tv_sec) * 1'000'000'000 + static_cast<uint64_t>(now.tv_nsec);
    }

    std::string readSmallFile(const char* path) {
      const int fd = open(path, O_RDONLY | O_CLOEXEC);
      if (fd < 0) {
        return {};
      }
      std::string text(1024, '\0');
      const ssize_t n = read(fd, text.data(), text.size());
      close(fd);
      text.resize(n > 0 ? static_cast<size_t>(n) : 0);
      return text;
    }

    int64_t parseInt(std::string_view text) {
      int64_t value = -1;
      std::from_chars(text.data(), text.data() + text.size(), value);
      return value;
    }

    // The `total=` value of the `full` line of a PSI file, in µs.
    int64_t fullTotalUs(const std::string& path) {
      const std::string text = readSmallFile(path.c_str());
      const size_t line = text.find("full ");
      if (line == std::string::npos) {
        return -1;
      }
      const size_t total = text.find("total=", line);
      return total == std::string::npos ? -1 : parseInt(std::string_view(text).substr(total + 6));
    }

    // Field 42 of /proc/thread-self/stat, delayacct_blkio_ticks; it stays 0 unless kernel.task_delayacct=1.
    int64_t blkioTicks() {
      const std::string text = readSmallFile("/proc/thread-self/stat");
      size_t pos = text.rfind(')');
      if (pos == std::string::npos) {
        return -1;
      }
      // Field 3 starts after ") ".
      for (int field = 3; field <= 42; ++field) {
        pos = text.find(' ', pos + 1);
        if (pos == std::string::npos) {
          return -1;
        }
      }
      return parseInt(std::string_view(text).substr(pos + 1));
    }

    std::string cgroupDir() {
      const std::string text = readSmallFile("/proc/self/cgroup");
      const size_t start = text.find("0::");
      if (start == std::string::npos) {
        return {};
      }
      const size_t end = text.find('\n', start);
      return "/sys/fs/cgroup" + text.substr(start + 3, end == std::string::npos ? end : end - start - 3);
    }

    std::string delta(int64_t before, int64_t after, int64_t divisor, std::string_view unit) {
      if (before < 0 || after < 0) {
        return "?";
      }
      return std::format("+{}{}", (after - before) / divisor, unit);
    }

#ifdef ANDROMEDA_TEST_IPC
    int gTestStallMs = 0;
#endif

    stall::Watch& watchdog() {
      static stall::Watch instance("andromeda");
      return instance;
    }

  } // namespace

  StallProbe::Totals StallProbe::readTotals() {
    if (!m_cgroupDir) {
      m_cgroupDir = cgroupDir();
    }
    return {
        .atNs = monotonicNs(),
        .blkioTicks = blkioTicks(),
        .cgroupIoFullUs = m_cgroupDir->empty() ? -1 : fullTotalUs(*m_cgroupDir + "/io.pressure"),
        .cgroupMemFullUs = m_cgroupDir->empty() ? -1 : fullTotalUs(*m_cgroupDir + "/memory.pressure"),
        .systemIoFullUs = fullTotalUs("/proc/pressure/io"),
    };
  }

  void StallProbe::frameStart() {
    m_marks = {};
    m_startNs = monotonicNs();
    m_fenceStartNs = fx_fence_wait_ns();
    getrusage(RUSAGE_THREAD, &m_usage);
    if (m_startNs - m_baseline.atNs >= kSampleIntervalNs) {
      m_baseline = readTotals();
    }
    watchdog().arm(m_startNs);
#ifdef ANDROMEDA_TEST_IPC
    // The stack signal of developer diagnostics ends a sleep early: sleep to the deadline.
    for (const uint64_t until = m_startNs + static_cast<uint64_t>(std::exchange(gTestStallMs, 0)) * 1'000'000;
         monotonicNs() < until;) {
      usleep(static_cast<useconds_t>((until - monotonicNs()) / 1000));
    }
#endif
  }

  void StallProbe::setDeveloper(bool on) {
    watchdog().setDeveloper(on);
  }

#ifdef ANDROMEDA_TEST_IPC
  void StallProbe::stallNextFrame(int ms) {
    gTestStallMs = ms;
  }
#endif

  void StallProbe::mark(Mark mark) {
    m_marks[static_cast<size_t>(mark)] = monotonicNs();
  }

  std::optional<std::string> StallProbe::frameEnd(double thresholdMs) {
    const uint64_t endNs = monotonicNs();
    watchdog().disarm();
    const double ms = static_cast<double>(endNs - m_startNs) / 1e6;
    if (ms < thresholdMs) {
      return std::nullopt;
    }
    const std::string samples = watchdog().take(m_startNs);
    rusage usage{};
    getrusage(RUSAGE_THREAD, &usage);
    const Totals now = readTotals();
    const uint64_t fenceNs = fx_fence_wait_ns() - m_fenceStartNs;
    const uint64_t buildStart = m_marks[static_cast<size_t>(Mark::BuildStart)];
    const uint64_t buildEnd = m_marks[static_cast<size_t>(Mark::BuildEnd)];
    const uint64_t commitEnd = m_marks[static_cast<size_t>(Mark::CommitEnd)];
    const auto msOf = [](uint64_t from, uint64_t to) { return to > from ? (to - from) / 1'000'000 : 0; };
    std::string phases = std::format("prep:{}", msOf(m_startNs, buildStart != 0 ? buildStart : endNs));
    uint64_t last = m_startNs;
    if (buildStart != 0 && buildEnd != 0) {
      const uint64_t build = buildEnd - buildStart;
      phases += std::format(",render:{}", (build > fenceNs ? build - fenceNs : 0) / 1'000'000);
      last = buildEnd;
    }
    phases += std::format(",fence:{}", fenceNs / 1'000'000);
    if (buildEnd != 0 && commitEnd != 0) {
      phases += std::format(",commit:{}", msOf(buildEnd, commitEnd));
      last = commitEnd;
    }
    if (last != m_startNs) {
      phases += std::format(",post:{}", msOf(last, endNs));
    }
    return std::format(
        "frame took {:.1f} ms (main loop blocked) phases={} majflt={} minflt={} nvcsw={} nivcsw={} blkio={} "
        "io_full={} mem_full={} sys_io_full={} window={}ms samples={}",
        ms, phases, usage.ru_majflt - m_usage.ru_majflt, usage.ru_minflt - m_usage.ru_minflt,
        usage.ru_nvcsw - m_usage.ru_nvcsw, usage.ru_nivcsw - m_usage.ru_nivcsw,
        delta(m_baseline.blkioTicks, now.blkioTicks, 1, ""),
        delta(m_baseline.cgroupIoFullUs, now.cgroupIoFullUs, 1000, "ms"),
        delta(m_baseline.cgroupMemFullUs, now.cgroupMemFullUs, 1000, "ms"),
        delta(m_baseline.systemIoFullUs, now.systemIoFullUs, 1000, "ms"), (now.atNs - m_baseline.atNs) / 1'000'000,
        samples.empty() ? "none" : samples
    );
  }

} // namespace andromeda
