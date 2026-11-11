#pragma once

// Crash capture shared by the compositor and the shell (through libdesk-ui's logger): a fixed ring of the last log
// lines and a fatal-signal handler. The handler writes <XDG_STATE_HOME>/andromeda/crashes/<component>-<UTC time>-<pid>.txt
// (format read by `andromeda debug crashes`), prints its path to stderr, and re-raises so systemd-coredump still gets
// the core. Header-only: each binary or dlopen module that includes it owns one ring and one handler.
// Frames are raw `module+offset`; symbolization happens later, never in the handler.

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <execinfo.h>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

namespace andromeda::crash {

  constexpr std::size_t kRingLines = 200;
  constexpr std::size_t kLineBytes = 240;
  constexpr int kMaxFrames = 64;
  constexpr std::size_t kModulePathBytes = 256;

  namespace detail {

    // `seq` is the line's index + 1 once complete, 0 while a writer fills it; the handler skips incomplete slots.
    struct Slot {
      std::atomic<std::uint64_t> seq{0};
      std::uint32_t length = 0;
      char text[kLineBytes];
    };

    inline Slot gRing[kRingLines];
    inline std::atomic<std::uint64_t> gNext{0};
    inline char gComponent[64];
    inline char gVersion[160];
    // Empty when no crash directory is writable: the handler still runs and re-raises, without a file.
    inline char gDir[512];
    alignas(16) inline char gAltStack[64 * 1024];
    constexpr std::size_t kThreadStackBytes = 64 * 1024;
    // The thread id of the one thread writing the report, 0 before a fatal signal.
    inline std::atomic<pid_t> gClaimedBy{0};
    // Handler scratch: static, not on the alternate stack.
    inline char gModules[kMaxFrames][kModulePathBytes];
    inline std::uintptr_t gOffsets[kMaxFrames];
    inline constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};

    inline void copyString(char* dst, std::size_t size, std::string_view src) noexcept {
      const std::size_t n = src.size() < size - 1 ? src.size() : size - 1;
      std::memcpy(dst, src.data(), n);
      dst[n] = '\0';
    }

    // Async-signal-safe buffered writer: write(2) only.
    struct Out {
      int fd;
      char buf[1024];
      std::size_t used = 0;

      void flush() noexcept {
        std::size_t done = 0;
        while (done < used) {
          const ssize_t n = ::write(fd, buf + done, used - done);
          if (n < 0 && errno == EINTR) {
            continue;
          }
          if (n <= 0) {
            break;
          }
          done += static_cast<std::size_t>(n);
        }
        used = 0;
      }
      void put(const char* s, std::size_t n) noexcept {
        while (n > 0) {
          if (used == sizeof(buf)) {
            flush();
          }
          const std::size_t take = n < sizeof(buf) - used ? n : sizeof(buf) - used;
          std::memcpy(buf + used, s, take);
          used += take;
          s += take;
          n -= take;
        }
      }
      void str(const char* s) noexcept { put(s, std::strlen(s)); }
      void dec(std::uint64_t v, int width = 0) noexcept {
        char tmp[24];
        int i = 0;
        do {
          tmp[i++] = static_cast<char>('0' + v % 10);
          v /= 10;
        } while (v != 0);
        while (i < width) {
          tmp[i++] = '0';
        }
        while (i > 0) {
          put(&tmp[--i], 1);
        }
      }
      void hex(std::uint64_t v) noexcept {
        char tmp[18];
        int i = 0;
        do {
          tmp[i++] = "0123456789abcdef"[v & 0xF];
          v >>= 4;
        } while (v != 0);
        put("0x", 2);
        while (i > 0) {
          put(&tmp[--i], 1);
        }
      }
    };

    // Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm); gmtime_r is not async-signal-safe.
    inline void civilFromDays(std::int64_t z, int& y, unsigned& m, unsigned& d) noexcept {
      z += 719468;
      const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
      const auto doe = static_cast<unsigned>(z - era * 146097);
      const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
      const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
      const unsigned mp = (5 * doy + 2) / 153;
      d = doy - (153 * mp + 2) / 5 + 1;
      m = mp < 10 ? mp + 3 : mp - 9;
      y = static_cast<int>(yoe + era * 400) + (m <= 2 ? 1 : 0);
    }

    // `basic`: 20261112T101500Z (file names); else 2026-11-12T10:15:00Z.
    inline void putTime(Out& out, std::time_t t, bool basic) noexcept {
      int y = 0;
      unsigned m = 0;
      unsigned d = 0;
      civilFromDays(t / 86400, y, m, d);
      const auto secs = static_cast<std::uint64_t>(t % 86400);
      out.dec(static_cast<std::uint64_t>(y), 4);
      if (!basic) {
        out.put("-", 1);
      }
      out.dec(m, 2);
      if (!basic) {
        out.put("-", 1);
      }
      out.dec(d, 2);
      out.put("T", 1);
      out.dec(secs / 3600, 2);
      if (!basic) {
        out.put(":", 1);
      }
      out.dec(secs / 60 % 60, 2);
      if (!basic) {
        out.put(":", 1);
      }
      out.dec(secs % 60, 2);
      out.put("Z", 1);
    }

    inline const char* signalName(int sig) noexcept {
      switch (sig) {
      case SIGSEGV:
        return "SIGSEGV";
      case SIGBUS:
        return "SIGBUS";
      case SIGILL:
        return "SIGILL";
      case SIGFPE:
        return "SIGFPE";
      case SIGABRT:
        return "SIGABRT";
      default:
        return "SIG?";
      }
    }

    inline std::uintptr_t contextPc(void* context) noexcept {
      if (context == nullptr) {
        return 0;
      }
      [[maybe_unused]] const auto* uc = static_cast<const ucontext_t*>(context);
#if defined(__x86_64__)
      return static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
#elif defined(__aarch64__)
      return static_cast<std::uintptr_t>(uc->uc_mcontext.pc);
#else
      return 0;
#endif
    }

    inline bool parseHex(const char*& p, const char* end, std::uintptr_t& v) noexcept {
      v = 0;
      const char* start = p;
      while (p < end) {
        const char c = *p;
        unsigned digit = 0;
        if (c >= '0' && c <= '9') {
          digit = static_cast<unsigned>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
          digit = static_cast<unsigned>(c - 'a' + 10);
        } else {
          break;
        }
        v = v << 4 | digit;
        ++p;
      }
      return p != start;
    }

    // One /proc/self/maps line: "start-end perms offset dev inode   path".
    inline void mapLine(const char* line, std::size_t length, void* const* frames, int count, std::uintptr_t& base,
                        const char*& basePath, char* basePathBuf) noexcept {
      const char* p = line;
      const char* end = line + length;
      std::uintptr_t start = 0;
      std::uintptr_t stop = 0;
      std::uintptr_t offset = 0;
      if (!parseHex(p, end, start) || p >= end || *p++ != '-' || !parseHex(p, end, stop)) {
        return;
      }
      for (int field = 0; field < 4 && p < end; ++field) {
        while (p < end && *p == ' ') {
          ++p;
        }
        if (field == 1) {
          parseHex(p, end, offset);
        }
        while (p < end && *p != ' ') {
          ++p;
        }
      }
      while (p < end && *p == ' ') {
        ++p;
      }
      const std::size_t pathLength = static_cast<std::size_t>(end - p);
      if (pathLength == 0) {
        return;
      }
      if (offset == 0) {
        copyString(basePathBuf, kModulePathBytes, std::string_view(p, pathLength));
        basePath = basePathBuf;
        base = start;
      }
      const bool sameModule = basePath != nullptr && std::strlen(basePath) == pathLength
          && std::memcmp(basePath, p, pathLength) == 0;
      for (int i = 0; i < count; ++i) {
        const auto addr = reinterpret_cast<std::uintptr_t>(frames[i]);
        if (addr >= start && addr < stop && gModules[i][0] == '\0') {
          copyString(gModules[i], kModulePathBytes, std::string_view(p, pathLength));
          gOffsets[i] = sameModule ? addr - base : addr - start + offset;
        }
      }
    }

    inline void resolveModules(void* const* frames, int count) noexcept {
      for (int i = 0; i < count; ++i) {
        gModules[i][0] = '\0';
        gOffsets[i] = 0;
      }
      const int fd = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
      if (fd < 0) {
        return;
      }
      static char chunk[4096];
      static char line[1024];
      static char basePathBuf[kModulePathBytes];
      std::size_t lineLength = 0;
      std::uintptr_t base = 0;
      const char* basePath = nullptr;
      for (;;) {
        const ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR) {
          continue;
        }
        if (n <= 0) {
          break;
        }
        for (ssize_t i = 0; i < n; ++i) {
          if (chunk[i] == '\n') {
            mapLine(line, lineLength, frames, count, base, basePath, basePathBuf);
            lineLength = 0;
          } else if (lineLength < sizeof(line)) {
            line[lineLength++] = chunk[i];
          }
        }
      }
      ::close(fd);
    }

    inline void resetAndRaise(int sig) noexcept {
      struct sigaction fallback{};
      fallback.sa_handler = SIG_DFL;
      sigemptyset(&fallback.sa_mask);
      (void)::sigaction(sig, &fallback, nullptr);
      // The signal stays blocked while the handler runs; it arrives with the default action when the handler returns.
      ::raise(sig);
    }

    inline void onFatalSignal(int sig, siginfo_t* info, void* context) {
      const int savedErrno = errno;
      const pid_t self = ::gettid();
      pid_t expected = 0;
      if (!gClaimedBy.compare_exchange_strong(expected, self)) {
        if (expected == self) {
          // A second fatal signal inside this thread's own report: give up on the report.
          resetAndRaise(sig);
          return;
        }
        // Another thread writes the report; its re-raise ends the process.
        sigset_t none;
        sigemptyset(&none);
        for (;;) {
          ::sigsuspend(&none);
        }
      }
      void* frames[kMaxFrames];
      int count = ::backtrace(frames, kMaxFrames);
      // Drop the handler and the kernel's signal frame: start at the faulting instruction when the unwinder found it.
      if (const std::uintptr_t pc = contextPc(context); pc != 0) {
        for (int i = 0; i < count; ++i) {
          if (reinterpret_cast<std::uintptr_t>(frames[i]) == pc) {
            std::memmove(frames, frames + i, static_cast<std::size_t>(count - i) * sizeof(void*));
            count -= i;
            break;
          }
        }
      }
      resolveModules(frames, count);

      timespec now{};
      ::clock_gettime(CLOCK_REALTIME, &now);
      const auto pid = static_cast<std::uint64_t>(::getpid());

      Out pathOut{.fd = -1, .buf = {}, .used = 0};
      pathOut.str(gDir);
      pathOut.put("/", 1);
      pathOut.str(gComponent);
      pathOut.put("-", 1);
      putTime(pathOut, now.tv_sec, true);
      pathOut.put("-", 1);
      pathOut.dec(pid);
      pathOut.str(".txt");
      char path[sizeof(pathOut.buf) + 1];
      const std::size_t pathLength = pathOut.used < sizeof(pathOut.buf) ? pathOut.used : sizeof(pathOut.buf) - 1;
      std::memcpy(path, pathOut.buf, pathLength);
      path[pathLength] = '\0';

      const int fd = gDir[0] == '\0' ? -1 : ::open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
      if (fd >= 0) {
        Out out{.fd = fd, .buf = {}, .used = 0};
        out.str("component: ");
        out.str(gComponent);
        out.str("\nversion: ");
        out.str(gVersion);
        out.str("\npid: ");
        out.dec(pid);
        out.str("\nsignal: ");
        out.str(signalName(sig));
        out.str(" (");
        out.dec(static_cast<std::uint64_t>(sig));
        out.str(")\n");
        if (info != nullptr && (sig == SIGSEGV || sig == SIGBUS)) {
          out.str("fault_address: ");
          out.hex(reinterpret_cast<std::uintptr_t>(info->si_addr));
          out.str("\n");
        }
        out.str("time: ");
        putTime(out, now.tv_sec, false);
        out.str("\nbacktrace:\n");
        for (int i = 0; i < count; ++i) {
          out.str("#");
          out.dec(static_cast<std::uint64_t>(i));
          out.str(" ");
          out.hex(reinterpret_cast<std::uintptr_t>(frames[i]));
          out.str(" ");
          if (gModules[i][0] != '\0') {
            out.str(gModules[i]);
            out.str("+");
            out.hex(gOffsets[i]);
          } else {
            out.str("?");
          }
          out.str("\n");
        }
        out.str("log:\n");
        const std::uint64_t next = gNext.load(std::memory_order_acquire);
        for (std::uint64_t index = next > kRingLines ? next - kRingLines : 0; index < next; ++index) {
          const Slot& slot = gRing[index % kRingLines];
          if (slot.seq.load(std::memory_order_acquire) != index + 1) {
            continue;
          }
          // Seqlock read: a logger on another thread may reuse the slot during the copy; drop the line if it did.
          static char line[kLineBytes];
          const std::uint32_t length = slot.length < kLineBytes ? slot.length : kLineBytes;
          std::memcpy(line, slot.text, length);
          std::atomic_thread_fence(std::memory_order_acquire);
          if (slot.seq.load(std::memory_order_relaxed) != index + 1) {
            continue;
          }
          out.put(line, length);
          out.put("\n", 1);
        }
        out.flush();
        ::close(fd);

        Out err{.fd = STDERR_FILENO, .buf = {}, .used = 0};
        err.str(gComponent);
        err.str(": ");
        err.str(signalName(sig));
        err.str(", crash report: ");
        err.str(path);
        err.str("\n");
        err.flush();
      }
      errno = savedErrno;
      resetAndRaise(sig);
    }

  } // namespace detail

  // Appends one line to the ring. Called by the loggers for every line at any level; never allocates.
  inline void record(char level, const char* section, std::string_view message) noexcept {
    using namespace detail;
    const std::uint64_t index = gNext.fetch_add(1, std::memory_order_relaxed);
    Slot& slot = gRing[index % kRingLines];
    slot.seq.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);

    timespec now{};
    ::clock_gettime(CLOCK_REALTIME, &now);
    const auto secs = static_cast<unsigned>(now.tv_sec % 86400);
    const auto ms = static_cast<unsigned>(now.tv_nsec / 1'000'000);
    char* p = slot.text;
    char* const end = slot.text + kLineBytes;
    const unsigned parts[] = {secs / 3600, secs / 60 % 60, secs % 60};
    for (int i = 0; i < 3; ++i) {
      *p++ = static_cast<char>('0' + parts[i] / 10);
      *p++ = static_cast<char>('0' + parts[i] % 10);
      *p++ = i < 2 ? ':' : '.';
    }
    *p++ = static_cast<char>('0' + ms / 100);
    *p++ = static_cast<char>('0' + ms / 10 % 10);
    *p++ = static_cast<char>('0' + ms % 10);
    *p++ = ' ';
    *p++ = level;
    *p++ = ' ';
    if (section != nullptr && section[0] != '\0') {
      *p++ = '[';
      for (const char* s = section; *s != '\0' && p < end - 3; ++s) {
        *p++ = *s;
      }
      *p++ = ']';
      *p++ = ' ';
    }
    for (const char c : message) {
      if (p == end) {
        break;
      }
      *p++ = c == '\n' || c == '\r' ? ' ' : c;
    }
    slot.length = static_cast<std::uint32_t>(p - slot.text);
    slot.seq.store(index + 1, std::memory_order_release);
  }

  // The first writable of <XDG_STATE_HOME>/andromeda/crashes, ~/.local/state/andromeda/crashes and, for the greeter
  // user whose home is /, /var/lib/andromeda-greeter/crashes (session/greetd/andromeda-greeter.tmpfiles). Creates it;
  // empty when none is writable.
  inline std::string crashDirectory() {
    std::string candidates[3];
    if (const char* state = std::getenv("XDG_STATE_HOME"); state != nullptr && state[0] == '/') {
      candidates[0] = std::string(state) + "/andromeda/crashes";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] == '/' && home[1] != '\0') {
      candidates[1] = std::string(home) + "/.local/state/andromeda/crashes";
    }
    candidates[2] = "/var/lib/andromeda-greeter/crashes";
    for (const auto& dir : candidates) {
      std::error_code ec;
      if (!dir.empty() && (std::filesystem::create_directories(dir, ec), !ec) && ::access(dir.c_str(), W_OK) == 0) {
        return dir;
      }
    }
    return {};
  }

  // Gives the calling thread its own alternate signal stack, so a stack overflow on it still writes a report. Call at
  // the start of each long-lived thread; the stack is freed when the thread exits.
  inline void installThreadStack() {
#if !defined(__SANITIZE_ADDRESS__)
    struct ThreadStack {
      void* memory = nullptr;
      ThreadStack() {
        memory = ::mmap(nullptr, detail::kThreadStackBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory == MAP_FAILED) {
          memory = nullptr;
          return;
        }
        stack_t stack{};
        stack.ss_sp = memory;
        stack.ss_size = detail::kThreadStackBytes;
        (void)::sigaltstack(&stack, nullptr);
      }
      ThreadStack(const ThreadStack&) = delete;
      ThreadStack& operator=(const ThreadStack&) = delete;
      ~ThreadStack() {
        if (memory == nullptr) {
          return;
        }
        stack_t off{};
        off.ss_flags = SS_DISABLE;
        (void)::sigaltstack(&off, nullptr);
        ::munmap(memory, detail::kThreadStackBytes);
      }
    };
    thread_local ThreadStack stack;
    (void)stack;
#endif
  }

  // Installs the handler for SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGABRT. Call once from the main thread, which gets
  // the static alternate stack; other threads call installThreadStack(). A no-op under AddressSanitizer, whose own
  // handler reports better.
  inline void install(std::string_view component, std::string_view version) {
#if defined(__SANITIZE_ADDRESS__)
    (void)component;
    (void)version;
#else
    using namespace detail;
    copyString(gComponent, sizeof(gComponent), component);
    copyString(gVersion, sizeof(gVersion), version);
    copyString(gDir, sizeof(gDir), crashDirectory());

    // backtrace() loads libgcc_s on its first call, which allocates: never let that first call be the handler's.
    void* warm[2];
    (void)::backtrace(warm, 2);

    stack_t stack{};
    stack.ss_sp = gAltStack;
    stack.ss_size = sizeof(gAltStack);
    (void)::sigaltstack(&stack, nullptr);

    struct sigaction action{};
    action.sa_sigaction = &onFatalSignal;
    // No SA_RESETHAND: a second thread's fatal signal must enter the handler and wait, not kill the process mid-report.
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (const int sig : kSignals) {
      (void)::sigaction(sig, &action, nullptr);
    }
#endif
  }

} // namespace andromeda::crash
