#pragma once

// Main-loop stall watch shared by the compositor and the shell. A thread blocks on a one-shot timerfd that the main
// loop arms when work starts and disarms when it ends, so it wakes only for a slow pass: no idle cost. It samples the
// main thread's kernel state every 200 ms. With developer diagnostics on, a pass that runs 250 ms or more also writes
// <XDG_STATE_HOME>/andromeda/stalls/<component>-<UTC>-<pid>.txt: the samples, the main thread's user stack, the kernel
// stacks of every GPU/DRM task (System1 Diagnostics.KernelStacks) when the wait is in a GPU path, and the last log
// lines. All capture and file work runs on the watch thread, never on the stalled one.

#include "crash_capture.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <ctime>
#include <execinfo.h>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <semaphore.h>
#include <string>
#include <string_view>
#include <sys/timerfd.h>
#include <systemd/sd-bus.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace andromeda::stall {

  inline std::uint64_t monotonicNs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000 + static_cast<std::uint64_t>(now.tv_nsec);
  }

  namespace detail {

    constexpr int kMaxFrames = 64;
    inline void* gFrames[kMaxFrames];
    inline std::atomic<int> gFrameCount{0};
    inline sem_t gStackDone;

    inline int stackSignal() { return SIGRTMIN + 5; }

    inline void onStackSignal(int /*sig*/) {
      const int savedErrno = errno;
      gFrameCount.store(::backtrace(gFrames, kMaxFrames), std::memory_order_release);
      ::sem_post(&gStackDone);
      errno = savedErrno;
    }

    inline std::string readSmallFile(const std::string& path) {
      const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
      if (fd < 0) {
        return {};
      }
      char buf[512];
      const ssize_t n = ::read(fd, buf, sizeof(buf));
      ::close(fd);
      std::string text(buf, n > 0 ? static_cast<std::size_t>(n) : 0);
      while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) {
        text.pop_back();
      }
      return text;
    }

    inline bool isGpuWait(std::string_view wchan) {
      for (const std::string_view key : {"drm", "xe_", "i915", "ttm", "dma_fence", "dma_resv", "ww_mutex"}) {
        if (wchan.find(key) != std::string_view::npos) {
          return true;
        }
      }
      return false;
    }

    inline std::string kernelStacks() {
      sd_bus* bus = nullptr;
      if (sd_bus_open_system(&bus) < 0) {
        return "unavailable: no system bus\n";
      }
      sd_bus_set_method_call_timeout(bus, 2'000'000);
      sd_bus_error error = SD_BUS_ERROR_NULL;
      sd_bus_message* reply = nullptr;
      std::string out;
      if (sd_bus_call_method(bus, "org.andromeda.System1", "/org/andromeda/System1/Diagnostics",
                             "org.andromeda.System1.Diagnostics", "KernelStacks", &error, &reply, "u",
                             static_cast<std::uint32_t>(::getpid()))
          < 0) {
        out = std::format("unavailable: {}\n", error.message != nullptr ? error.message : "call failed");
      } else if (const char* text = nullptr; sd_bus_message_read(reply, "s", &text) >= 0 && text != nullptr) {
        out = text;
      }
      sd_bus_error_free(&error);
      sd_bus_message_unref(reply);
      sd_bus_flush_close_unref(bus);
      return out;
    }

    inline std::string recentLog() {
      using namespace crash::detail;
      std::string out;
      const std::uint64_t next = gNext.load(std::memory_order_acquire);
      for (std::uint64_t index = next > crash::kRingLines ? next - crash::kRingLines : 0; index < next; ++index) {
        const Slot& slot = gRing[index % crash::kRingLines];
        if (slot.seq.load(std::memory_order_acquire) != index + 1) {
          continue;
        }
        const std::string line(slot.text, slot.length < crash::kLineBytes ? slot.length : crash::kLineBytes);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (slot.seq.load(std::memory_order_relaxed) == index + 1) {
          out += line;
          out += '\n';
        }
      }
      return out;
    }

  } // namespace detail

  // <state>/andromeda/stalls next to the crash directory; empty when none is writable.
  inline std::string stallDirectory() {
    const std::string crashes = crash::crashDirectory();
    if (crashes.empty()) {
      return {};
    }
    const std::string dir = std::filesystem::path(crashes).parent_path() / "stalls";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return ec ? std::string() : dir;
  }

  class Watch {
  public:
    static constexpr std::uint64_t kPeriodNs = 200'000'000;
    static constexpr int kMaxSamples = 10;
    static constexpr std::uint64_t kReportNs = 250'000'000;
    static constexpr int kReportsPerHour = 6;
    static constexpr std::size_t kKeptReports = 50;

    // Construct on the thread to watch.
    explicit Watch(std::string component)
        : m_component(std::move(component)), m_tid(::gettid()), m_task(std::format("/proc/self/task/{}/", m_tid)),
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

    ~Watch() {
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

    Watch(const Watch&) = delete;
    Watch& operator=(const Watch&) = delete;

    // Developer diagnostics: reports with stacks. Call on the watched thread.
    void setDeveloper(bool on) {
      if (on && !m_handlerInstalled) {
        m_handlerInstalled = true;
        sem_init(&detail::gStackDone, 0, 0);
        // backtrace() loads libgcc_s on its first call, which allocates: never let that first call be the handler's.
        void* warm[2];
        (void)::backtrace(warm, 2);
        struct sigaction action{};
        action.sa_handler = &detail::onStackSignal;
        action.sa_flags = SA_RESTART;
        sigemptyset(&action.sa_mask);
        sigaction(detail::stackSignal(), &action, nullptr);
      }
      m_developer.store(on);
    }

    [[nodiscard]] bool developer() const { return m_developer.load(std::memory_order_relaxed); }

    void arm(std::uint64_t startNs) {
      m_startNs.store(startNs);
      setTimer(startNs + kPeriodNs);
    }

    void disarm() {
      const std::uint64_t start = m_startNs.exchange(0);
      m_endNs.store(monotonicNs());
      m_endedStartNs.store(start);
      // Wake the watch thread to finish a report only after a pass it captured.
      if (start != 0 && m_reportStartNs.load() == start) {
        setTimer(1);
      } else {
        setTimer(0);
      }
    }

    // The samples taken during the pass that started at `startNs`.
    std::string take(std::uint64_t startNs) {
      const std::lock_guard lock(m_mutex);
      std::string samples = m_samplesStartNs == startNs ? m_samples : std::string();
      return samples;
    }

  private:
    struct Pending {
      std::string userStack;
      std::string kernelStacks;
    };

    void setTimer(std::uint64_t deadlineNs) const {
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
        std::uint64_t expirations = 0;
        if (read(m_timer, &expirations, sizeof(expirations)) < 0 && errno != EINTR) {
          return;
        }
        if (m_stop.load()) {
          return;
        }
        const std::uint64_t start = m_startNs.load();
        if (start == 0) {
          finishReport();
          continue;
        }
        const std::uint64_t now = monotonicNs();
        // A late re-arm for an earlier pass can replace this pass's deadline: put it back.
        if (now < start + kPeriodNs) {
          setTimer(start + kPeriodNs);
          continue;
        }
        const std::string wchan = detail::readSmallFile(m_task + "wchan");
        const std::string sample = this->sample((now - start) / 1'000'000, wchan);
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
        if (count == 1 && developer()) {
          capture(start, wchan);
        }
        if (count < kMaxSamples && m_startNs.load() == start) {
          setTimer(start + kPeriodNs * static_cast<std::uint64_t>(count + 1));
        }
      }
    }

    void capture(std::uint64_t start, const std::string& wchan) {
      m_pending = std::make_unique<Pending>();
      while (sem_trywait(&detail::gStackDone) == 0) {
      }
      detail::gFrameCount.store(0);
      if (::tgkill(::getpid(), m_tid, detail::stackSignal()) == 0) {
        timespec deadline{};
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 100'000'000;
        deadline.tv_sec += deadline.tv_nsec / 1'000'000'000;
        deadline.tv_nsec %= 1'000'000'000;
        if (sem_timedwait(&detail::gStackDone, &deadline) == 0) {
          m_pending->userStack = symbolize();
        } else {
          m_pending->userStack = "unavailable: the thread did not take the signal within 100 ms\n";
        }
      }
      if (detail::isGpuWait(wchan)) {
        m_pending->kernelStacks = detail::kernelStacks();
      }
      m_reportStartNs.store(start);
      // The pass may have ended during the capture, before disarm() could see the report start.
      if (m_startNs.load() != start) {
        finishReport();
      }
    }

    static std::string symbolize() {
      const int count = detail::gFrameCount.load(std::memory_order_acquire);
      crash::detail::resolveModules(detail::gFrames, count);
      char** names = ::backtrace_symbols(detail::gFrames, count);
      std::string out;
      // Frame 0 and 1 are this handler and the kernel's signal frame.
      for (int i = 2; i < count; ++i) {
        // backtrace_symbols gives "module(name+0x1f) [addr]"; keep the name, if any.
        std::string_view name = names != nullptr ? std::string_view(names[i]) : std::string_view();
        const std::size_t open = name.find('(');
        name = open == std::string_view::npos ? std::string_view() : name.substr(open + 1);
        name = name.substr(0, name.find_first_of("+)"));
        out += std::format(
            "#{} {}+{:#x}{}{}\n", i - 2, crash::detail::gModules[i][0] != '\0' ? crash::detail::gModules[i] : "?",
            crash::detail::gOffsets[i], name.empty() ? "" : " ", name
        );
      }
      free(names);
      return out;
    }

    void finishReport() {
      const std::uint64_t start = m_reportStartNs.exchange(0);
      std::unique_ptr<Pending> pending = std::move(m_pending);
      if (start == 0 || pending == nullptr) {
        return;
      }
      if (m_endedStartNs.load() != start) {
        return;
      }
      const std::uint64_t end = m_endNs.load();
      if (end < start + kReportNs || !allowReport(end)) {
        return;
      }
      const std::string dir = stallDirectory();
      if (dir.empty()) {
        return;
      }
      const std::time_t wall = std::time(nullptr);
      std::tm utc{};
      gmtime_r(&wall, &utc);
      char stamp[32];
      std::strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", &utc);
      const std::string path = std::format("{}/{}-{}-{}.txt", dir, m_component, stamp, ::getpid());
      const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
      if (fd < 0) {
        return;
      }
      const std::string report = std::format(
          "component: {}\npid: {}\nduration_ms: {}\nsamples: {}\nuser_stack: (addr2line -Cfie <module> <offset>)\n{}kernel_stacks:\n{}log:\n{}",
          m_component, ::getpid(), (end - start) / 1'000'000, take(start), pending->userStack,
          pending->kernelStacks.empty() ? std::string("not a GPU wait\n") : pending->kernelStacks, detail::recentLog()
      );
      for (std::size_t done = 0; done < report.size();) {
        const ssize_t n = ::write(fd, report.data() + done, report.size() - done);
        if (n <= 0) {
          break;
        }
        done += static_cast<std::size_t>(n);
      }
      ::close(fd);
      const std::string line = std::format("{}: stall {} ms, report: {}\n", m_component, (end - start) / 1'000'000, path);
      (void)!::write(STDERR_FILENO, line.data(), line.size());
      prune(dir);
    }

    bool allowReport(std::uint64_t nowNs) {
      constexpr std::uint64_t kHourNs = 3600ULL * 1'000'000'000;
      if (nowNs - m_hourStartNs >= kHourNs) {
        m_hourStartNs = nowNs;
        m_reportsThisHour = 0;
      }
      return m_reportsThisHour++ < kReportsPerHour;
    }

    static void prune(const std::string& dir) {
      std::error_code ec;
      std::vector<std::filesystem::path> files;
      for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        files.push_back(entry.path());
      }
      if (files.size() <= kKeptReports) {
        return;
      }
      std::ranges::sort(files, [](const auto& a, const auto& b) {
        std::error_code e;
        return std::filesystem::last_write_time(a, e) < std::filesystem::last_write_time(b, e);
      });
      for (std::size_t i = 0; i + kKeptReports < files.size(); ++i) {
        std::filesystem::remove(files[i], ec);
      }
    }

    std::string sample(std::uint64_t atMs, const std::string& wchan) const {
      const std::string stat = detail::readSmallFile(m_task + "stat");
      const std::size_t paren = stat.rfind(')');
      const char state = paren != std::string::npos && paren + 2 < stat.size() ? stat[paren + 2] : '?';
      const std::string syscall = detail::readSmallFile(m_task + "syscall");
      return std::format(
          "{}ms:state={},wchan={},syscall={}", atMs, state, wchan.empty() ? "?" : wchan,
          syscall.substr(0, syscall.find(' '))
      );
    }

    const std::string m_component;
    const pid_t m_tid;
    const std::string m_task;
    const int m_timer;
    bool m_handlerInstalled = false;
    std::atomic<bool> m_developer{false};
    std::atomic<std::uint64_t> m_startNs{0};
    // The start of the pass a report is pending for, 0 when none; m_pending is touched only by the watch thread.
    std::atomic<std::uint64_t> m_reportStartNs{0};
    // The end of the last pass and its start, written together by disarm().
    std::atomic<std::uint64_t> m_endNs{0};
    std::atomic<std::uint64_t> m_endedStartNs{0};
    std::unique_ptr<Pending> m_pending;
    std::uint64_t m_hourStartNs = 0;
    int m_reportsThisHour = 0;
    std::atomic<bool> m_stop{false};
    std::mutex m_mutex;
    std::uint64_t m_samplesStartNs = 0;
    int m_sampleCount = 0;
    std::string m_samples;
    std::thread m_thread;
  };

} // namespace andromeda::stall
