#include "output/stall_probe.h"

#include <andromedafx/render/frame_stats.h>

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

    std::string firstToken(const std::string& text) {
      return text.substr(0, text.find_first_of(" \n"));
    }

    // Samples the main thread's kernel state while a frame runs past 200 ms, every 200 ms, at most 10 times. The thread
    // blocks on a one-shot timerfd that each frame arms and disarms, so it wakes only for a slow frame.
    class Watchdog {
    public:
      static constexpr uint64_t kPeriodNs = 200'000'000;
      static constexpr int kMaxSamples = 10;

      Watchdog()
          : m_task(std::format("/proc/self/task/{}/", gettid())),
            m_timer(timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC)) {
        if (m_timer < 0) {
          return;
        }
        sigset_t all;
        sigset_t previous;
        sigfillset(&all);
        pthread_sigmask(SIG_BLOCK, &all, &previous);
        m_thread = std::thread([this] { loop(); });
        pthread_sigmask(SIG_SETMASK, &previous, nullptr);
      }

      ~Watchdog() {
        if (m_thread.joinable()) {
          m_stop.store(true);
          // An absolute deadline in the past fires at once.
          setTimer(1);
          m_thread.join();
        }
        if (m_timer >= 0) {
          close(m_timer);
        }
      }

      void arm(uint64_t startNs) {
        m_frameStartNs.store(startNs);
        setTimer(startNs + kPeriodNs);
      }

      void disarm() {
        m_frameStartNs.store(0);
        setTimer(0);
      }

      // The samples taken during the frame that started at `startNs`.
      std::string take(uint64_t startNs) {
        const std::lock_guard lock(m_mutex);
        std::string samples = m_samplesStartNs == startNs ? std::move(m_samples) : std::string();
        m_samples.clear();
        return samples;
      }

    private:
      // An absolute CLOCK_MONOTONIC deadline; 0 disarms.
      void setTimer(uint64_t deadlineNs) const {
        if (m_timer < 0) {
          return;
        }
        itimerspec spec{};
        spec.it_value.tv_sec = static_cast<time_t>(deadlineNs / 1'000'000'000);
        spec.it_value.tv_nsec = static_cast<long>(deadlineNs % 1'000'000'000);
        timerfd_settime(m_timer, TFD_TIMER_ABSTIME, &spec, nullptr);
      }

      void loop() {
        pthread_setname_np(pthread_self(), "andromeda-stall");
        for (;;) {
          uint64_t expirations = 0;
          if (read(m_timer, &expirations, sizeof(expirations)) < 0 && errno != EINTR) {
            return;
          }
          if (m_stop.load()) {
            return;
          }
          const uint64_t start = m_frameStartNs.load();
          if (start == 0) {
            continue;
          }
          const uint64_t now = monotonicNs();
          // A late re-arm for an earlier frame can replace this frame's deadline: put it back.
          if (now < start + kPeriodNs) {
            setTimer(start + kPeriodNs);
            continue;
          }
          const std::string sample = this->sample((now - start) / 1'000'000);
          int count = 0;
          {
            const std::lock_guard lock(m_mutex);
            if (m_samplesStartNs != start) {
              m_samplesStartNs = start;
              m_sampleCount = 0;
              m_samples.clear();
            }
            m_samples += m_samples.empty() ? sample : ";" + sample;
            count = ++m_sampleCount;
          }
          if (count < kMaxSamples && m_frameStartNs.load() == start) {
            setTimer(start + kPeriodNs * static_cast<uint64_t>(count + 1));
          }
        }
      }

      std::string sample(uint64_t atMs) const {
        const std::string stat = readSmallFile((m_task + "stat").c_str());
        const size_t paren = stat.rfind(')');
        const char state = paren != std::string::npos && paren + 2 < stat.size() ? stat[paren + 2] : '?';
        const std::string wchan = readSmallFile((m_task + "wchan").c_str());
        return std::format(
            "{}ms:state={},wchan={},syscall={}", atMs, state, wchan.empty() ? "?" : wchan,
            firstToken(readSmallFile((m_task + "syscall").c_str()))
        );
      }

      const std::string m_task;
      const int m_timer;
      std::atomic<uint64_t> m_frameStartNs{0};
      std::atomic<bool> m_stop{false};
      std::mutex m_mutex;
      uint64_t m_samplesStartNs = 0;
      int m_sampleCount = 0;
      std::string m_samples;
      std::thread m_thread;
    };

    Watchdog& watchdog() {
      static Watchdog instance;
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
  }

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
