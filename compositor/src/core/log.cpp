#include "core/log.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <string>
#include <system_error>
#include <thread>
#include <unistd.h>

namespace {

#ifdef NDEBUG
  LogLevel gMinLevel = LogLevel::Info;
#else
  LogLevel gMinLevel = LogLevel::Debug;
#endif

  FILE* gLogFile = nullptr;
  // gLogMutex: the log file (writer thread, init). gStateMutex: the callers' repeat state; the main thread never
  // takes gLogMutex, so a writer blocked in I/O cannot block it.
  std::mutex gLogMutex;
  std::mutex gStateMutex;
  bool gFileEnabled = false;
  std::string gLogPath;
  std::string gBackupLogPath;
  std::uintmax_t gLogSizeBytes = 0;
  std::size_t gBufferedFileLogLines = 0;
  std::chrono::steady_clock::time_point gLastFileFlushAt = std::chrono::steady_clock::now();
  bool gRegisteredExitFlush = false;
  bool gConsoleEnabled = true;

  constexpr std::size_t kMaxLogBytes = 1 * 1024 * 1024; // 1 MB
  constexpr std::size_t kMaxLogLineBytes = 8 * 1024;    // 8 KiB
  constexpr std::size_t kBufferedFileLogFlushLines = 64;
  constexpr auto kBufferedFileLogFlushInterval = std::chrono::milliseconds(500);

  // Collapse runs of an identical message and report the count once the run ends: a per-frame failure can emit
  // thousands of lines per second, and synchronous VT writes at that rate stall the main loop.
  constexpr auto kRepeatWindow = std::chrono::seconds(1);

  // Every write runs on one writer thread: a write to the journal or the log file can block for seconds under disk
  // writeback throttling or a busy journald, and on the main thread that froze the frame loop and the cursor. The
  // main thread only queues lines; past this budget it drops them and the writer reports the count.
  constexpr std::size_t kMaxQueuedBytes = 4 * 1024 * 1024;

  struct QueuedLine {
    std::string console;
    std::string file;
    bool flush = false;
  };

  std::mutex gQueueMutex;
  std::condition_variable gQueueReady;
  std::deque<QueuedLine> gQueue;
  std::size_t gQueuedBytes = 0;
  std::size_t gDroppedLines = 0;
  bool gWriterStop = false;
  bool gWriterRunning = false;
  // A forked child has no writer thread; it writes in place, as it did before the writer existed.
  bool gInForkedChild = false;
  std::thread gWriter;

  std::string gLastMessage;
  const char* gLastSection = nullptr;
  LogLevel gLastLevel = LogLevel::Debug;
  std::size_t gSuppressedRepeats = 0;
  std::chrono::steady_clock::time_point gRepeatWindowStart{};

  struct CappedLogMessage {
    std::string storage;
    std::string_view original;
    bool capped = false;

    [[nodiscard]] std::string_view text() const noexcept { return capped ? std::string_view(storage) : original; }
  };

  const char* levelTagAnsi(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
      return "\033[36mDBG\033[0m";
    case LogLevel::Info:
      return "\033[32mINF\033[0m";
    case LogLevel::Warn:
      return "\033[33mWRN\033[0m";
    case LogLevel::Error:
      return "\033[31mERR\033[0m";
    }
    return "???";
  }

  const char* levelTagPlain(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
      return "DBG";
    case LogLevel::Info:
      return "INF";
    case LogLevel::Warn:
      return "WRN";
    case LogLevel::Error:
      return "ERR";
    }
    return "???";
  }

  std::size_t utf8PrefixBoundary(std::string_view text, std::size_t maxBytes) {
    if (maxBytes >= text.size()) {
      return text.size();
    }

    std::size_t end = maxBytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
      --end;
    }
    return end;
  }

  std::string truncationSuffix(std::size_t originalBytes) {
    return std::string(" ... [truncated, original=") + std::to_string(originalBytes) + " bytes]";
  }

  CappedLogMessage capMessageForLine(std::string_view msg, std::size_t prefixBytes) {
    CappedLogMessage result;
    result.original = msg;

    if (prefixBytes + 1 >= kMaxLogLineBytes) {
      result.capped = true;
      return result;
    }

    const std::size_t maxMessageBytes = kMaxLogLineBytes - prefixBytes - 1;
    if (msg.size() <= maxMessageBytes) {
      return result;
    }

    result.capped = true;

    std::string suffix = truncationSuffix(msg.size());
    if (suffix.size() > maxMessageBytes) {
      constexpr std::string_view kShortSuffix = " ... [truncated]";
      result.storage = std::string(kShortSuffix.substr(0, std::min(kShortSuffix.size(), maxMessageBytes)));
      return result;
    }

    const std::size_t bodyBytes = utf8PrefixBoundary(msg, maxMessageBytes - suffix.size());
    result.storage.reserve(bodyBytes + suffix.size());
    result.storage.append(msg.substr(0, bodyBytes));
    result.storage.append(suffix);
    return result;
  }

  std::size_t formattedPrefixLength(int length, std::size_t bufferSize) {
    if (length <= 0 || bufferSize == 0) {
      return 0;
    }
    return std::min(static_cast<std::size_t>(length), bufferSize - 1);
  }

  std::string consolePrefix(const std::tm& tm, long msec, LogLevel level, const char* section) {
    char buffer[96];
    const int length = std::snprintf(
        buffer, sizeof(buffer), "%02d:%02d:%02d.%03ld [%s]", tm.tm_hour, tm.tm_min, tm.tm_sec, msec, levelTagAnsi(level)
    );
    std::string prefix(buffer, formattedPrefixLength(length, sizeof(buffer)));
    if (section != nullptr && section[0] != '\0') {
      prefix += " [\033[34m";
      prefix += section;
      prefix += "\033[0m]";
    }
    prefix += ' ';
    return prefix;
  }

  std::string filePrefix(const std::tm& tm, long msec, LogLevel level, const char* section) {
    char buffer[128];
    const int length = std::snprintf(
        buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03ld [%s]", tm.tm_year + 1900, tm.tm_mon + 1,
        tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, msec, levelTagPlain(level)
    );
    std::string prefix(buffer, formattedPrefixLength(length, sizeof(buffer)));
    if (section != nullptr && section[0] != '\0') {
      prefix += " [";
      prefix += section;
      prefix += ']';
    }
    prefix += ' ';
    return prefix;
  }

  void writeRaw(FILE* stream, const std::string& line) {
    if (stream != nullptr && !line.empty()) {
      std::fwrite(line.data(), 1, line.size(), stream);
    }
  }

  void flushLogFileUnlocked() {
    if (gLogFile == nullptr) {
      return;
    }
    std::fflush(gLogFile);
    gBufferedFileLogLines = 0;
    gLastFileFlushAt = std::chrono::steady_clock::now();
  }

  void stopWriter();

  void flushLogFileAtExit() {
    stopWriter();
    std::scoped_lock lock(gLogMutex);
    flushLogFileUnlocked();
  }

  void closeLogFileUnlocked() {
    if (gLogFile == nullptr) {
      return;
    }

    std::fflush(gLogFile);
    std::fclose(gLogFile);
    gLogFile = nullptr;
  }

  std::uintmax_t currentLogFileSizeUnlocked() {
    if (gLogPath.empty()) {
      return 0;
    }

    std::error_code ec;
    const auto size = std::filesystem::file_size(gLogPath, ec);
    return ec ? 0 : size;
  }

  void openLogFileUnlocked() {
    if (gLogPath.empty()) {
      return;
    }

    gLogFile = std::fopen(gLogPath.c_str(), "a");
    gLogSizeBytes = gLogFile == nullptr ? 0 : currentLogFileSizeUnlocked();
    gBufferedFileLogLines = 0;
    gLastFileFlushAt = std::chrono::steady_clock::now();
  }

  void rotateLogFileUnlocked() {
    closeLogFileUnlocked();

    if (gLogPath.empty() || gBackupLogPath.empty()) {
      return;
    }

    std::error_code ec;
    std::filesystem::remove(gBackupLogPath, ec);
    ec.clear();
    std::filesystem::rename(gLogPath, gBackupLogPath, ec);
    openLogFileUnlocked();
  }

  void writeQueuedLine(const QueuedLine& line);
  void queueLine(QueuedLine line);

  void emitUnlocked(LogLevel level, const char* section, std::string_view msg, const std::tm& tm, long msec) {
    QueuedLine line;
    // Console: respects gMinLevel, ANSI colours, time only
    if (gConsoleEnabled && level >= gMinLevel) {
      const std::string prefix = consolePrefix(tm, msec, level, section);
      const CappedLogMessage capped = capMessageForLine(msg, prefix.size());
      line.console = prefix;
      line.console += capped.text();
      line.console += '\n';
    }

    // File: always unfiltered, no ANSI, full date for context
    if (gFileEnabled) {
      const std::string prefix = filePrefix(tm, msec, level, section);
      const CappedLogMessage capped = capMessageForLine(msg, prefix.size());
      line.file = prefix;
      line.file += capped.text();
      line.file += '\n';
      line.flush = level >= LogLevel::Warn;
    }
    queueLine(std::move(line));
  }

  std::size_t gSkippedConsoleLines = 0;

  // The console is the journal stream or a pipe: when it is full, skip the line rather than wait, so the log file
  // (written first) keeps going. A child shares stderr's file description, so it cannot be set O_NONBLOCK.
  void writeConsole(const std::string& line) {
    if (line.empty()) {
      return;
    }
    pollfd fd{.fd = STDERR_FILENO, .events = POLLOUT, .revents = 0};
    if (::poll(&fd, 1, 0) != 1 || (fd.revents & POLLOUT) == 0) {
      ++gSkippedConsoleLines;
      return;
    }
    if (gSkippedConsoleLines > 0) {
      const std::string note = std::format("[log] {} console lines skipped: stderr was full\n", gSkippedConsoleLines);
      gSkippedConsoleLines = 0;
      (void)!::write(STDERR_FILENO, note.data(), note.size());
    }
    (void)!::write(STDERR_FILENO, line.data(), line.size());
  }

  // Runs on the writer thread, or in place in a forked child; gLogMutex guards the file state.
  void writeQueuedLine(const QueuedLine& line) {
    if (!line.file.empty()) {
      std::scoped_lock lock(gLogMutex);
      if (gLogFile != nullptr) {
        if (gLogSizeBytes > 0 && gLogSizeBytes + line.file.size() > kMaxLogBytes) {
          rotateLogFileUnlocked();
        }
        writeRaw(gLogFile, line.file);
        gLogSizeBytes += line.file.size();
        if (line.flush) {
          flushLogFileUnlocked();
        } else {
          ++gBufferedFileLogLines;
          if (gBufferedFileLogLines >= kBufferedFileLogFlushLines
              || std::chrono::steady_clock::now() - gLastFileFlushAt >= kBufferedFileLogFlushInterval) {
            flushLogFileUnlocked();
          }
        }
      }
    }
    writeConsole(line.console);
  }

  void writerMain() {
    std::unique_lock lock(gQueueMutex);
    while (true) {
      gQueueReady.wait(lock, [] { return gWriterStop || !gQueue.empty(); });
      if (gQueue.empty()) {
        return;
      }
      QueuedLine line = std::move(gQueue.front());
      gQueue.pop_front();
      gQueuedBytes -= line.console.size() + line.file.size();
      const std::size_t dropped = std::exchange(gDroppedLines, 0);
      lock.unlock();
      if (dropped > 0) {
        const std::string note = std::format("[log] {} lines dropped: the log writer fell behind\n", dropped);
        writeQueuedLine({.console = note, .file = note, .flush = true});
      }
      writeQueuedLine(line);
      lock.lock();
    }
  }

  void queueLine(QueuedLine line) {
    if (gInForkedChild) {
      writeQueuedLine(line);
      return;
    }
    std::scoped_lock lock(gQueueMutex);
    if (!gWriterRunning) {
      gWriter = std::thread(writerMain);
      gWriterRunning = true;
      // Joins before static destruction: a joinable std::thread destructor terminates the process.
      static const bool registered = std::atexit(stopWriter) == 0;
      (void)registered;
    }
    const std::size_t bytes = line.console.size() + line.file.size();
    if (gQueuedBytes + bytes > kMaxQueuedBytes) {
      ++gDroppedLines;
      return;
    }
    gQueuedBytes += bytes;
    gQueue.push_back(std::move(line));
    gQueueReady.notify_one();
  }

  void stopWriter() {
    {
      std::scoped_lock lock(gQueueMutex);
      if (!gWriterRunning) {
        return;
      }
      gWriterStop = true;
    }
    gQueueReady.notify_one();
    gWriter.join();
    std::scoped_lock lock(gQueueMutex);
    gWriterRunning = false;
    gWriterStop = false;
  }

  void prepareFork() {
    gStateMutex.lock();
    gQueueMutex.lock();
    gLogMutex.lock();
  }

  void afterForkParent() {
    gLogMutex.unlock();
    gQueueMutex.unlock();
    gStateMutex.unlock();
  }

  void afterForkChild() {
    gLogMutex.unlock();
    gQueueMutex.unlock();
    gStateMutex.unlock();
    gInForkedChild = true;
  }

  [[maybe_unused]] const bool gForkHandlers = pthread_atfork(prepareFork, afterForkParent, afterForkChild) == 0;

  // Emits the pending "repeated N times" summary, if any.
  void flushRepeatsUnlocked(const std::tm& tm, long msec) {
    if (gSuppressedRepeats == 0) {
      return;
    }
    const std::size_t count = gSuppressedRepeats;
    gSuppressedRepeats = 0;
    emitUnlocked(gLastLevel, gLastSection, std::format("last message repeated {} times", count), tm, msec);
  }

  [[nodiscard]] std::string resolveCacheDir() {
    const char* cacheHome = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    if (cacheHome != nullptr && cacheHome[0] != '\0') {
      return std::string(cacheHome) + "/andromeda";
    }
    if (home != nullptr && home[0] != '\0') {
      return std::string(home) + "/.cache/andromeda";
    }
    return {};
  }

  // If fd 1 or fd 2 points at a TTY (greetd wires them to /dev/tty1), replace them with an append-mode file inside
  // `cacheDir` (or /dev/null if `cacheDir` is empty): synchronous VT writes on the main thread can turn a per-frame
  // wlroots error into a hard livelock.
  void redirectStdioIfTty(const std::string& cacheDir) {
    const bool stdoutIsTty = isatty(STDOUT_FILENO) != 0;
    const bool stderrIsTty = isatty(STDERR_FILENO) != 0;
    if (!stdoutIsTty && !stderrIsTty) {
      return;
    }

    int fd = -1;
    if (!cacheDir.empty()) {
      const std::string path = cacheDir + "/andromeda-stderr.log";
      fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    }
    if (fd < 0) {
      fd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    }
    if (fd < 0) {
      return;
    }

    // dup2 leaves the target fd without O_CLOEXEC, which is what we want:
    // spawned children inherit the redirected stderr instead of /dev/tty1.
    if (stdoutIsTty) {
      (void)::dup2(fd, STDOUT_FILENO);
    }
    if (stderrIsTty) {
      (void)::dup2(fd, STDERR_FILENO);
    }
    ::close(fd);
  }

} // namespace

void initLogFile() {
  std::error_code ec;
  const std::string dir = resolveCacheDir();
  if (!dir.empty()) {
    std::filesystem::create_directories(dir, ec);
    if (ec) {
      // Directory unavailable; still redirect fd 1/2 so raw writes don't hit the VT. redirectStdioIfTty falls back to
      // /dev/null when its path is empty.
      redirectStdioIfTty({});
      return;
    }
  }

  redirectStdioIfTty(dir);

  if (dir.empty()) {
    return;
  }

  const std::string logPath = dir + "/andromeda.log";
  const std::string backupPath = dir + "/andromeda.log.1";

  std::scoped_lock lock(gLogMutex);
  closeLogFileUnlocked();
  gLogPath = logPath;
  gBackupLogPath = backupPath;

  const auto size = std::filesystem::file_size(gLogPath, ec);
  if (!ec && size > kMaxLogBytes) {
    rotateLogFileUnlocked();
  } else {
    openLogFileUnlocked();
  }

  if (gLogFile != nullptr && !gRegisteredExitFlush) {
    (void)std::atexit(flushLogFileAtExit);
    gRegisteredExitFlush = true;
  }
  std::scoped_lock stateLock(gStateMutex);
  gFileEnabled = gLogFile != nullptr;
}

namespace detail {

  void logMessage(LogLevel level, const char* section, std::string_view msg) {
    std::timespec ts{};
    std::timespec_get(&ts, TIME_UTC);
    std::tm tm{};
    localtime_r(&ts.tv_sec, &tm);
    const long msec = ts.tv_nsec / 1'000'000;

    std::scoped_lock lock(gStateMutex);

    const auto now = std::chrono::steady_clock::now();
    if (msg == gLastMessage && section == gLastSection && level == gLastLevel) {
      // Identical to the previous line: swallow it, but let one through per
      // window so a persistent fault stays visible without stalling the caller.
      if (now - gRepeatWindowStart < kRepeatWindow) {
        ++gSuppressedRepeats;
        return;
      }
      flushRepeatsUnlocked(tm, msec);
      gRepeatWindowStart = now;
    } else {
      flushRepeatsUnlocked(tm, msec);
      gLastMessage.assign(msg);
      gLastSection = section;
      gLastLevel = level;
      gRepeatWindowStart = now;
    }

    emitUnlocked(level, section, msg, tm, msec);
  }

} // namespace detail

void setConsoleLogging(bool enabled) {
  std::scoped_lock lock(gStateMutex);
  gConsoleEnabled = enabled;
}

void wlrLogHandler(enum wlr_log_importance importance, const char* fmt, va_list args) {
  // wlroots only applies the wlr_log_init verbosity in its default stderr
  // logger; a custom callback receives every message, WLR_DEBUG included.
  if (importance > wlr_log_get_verbosity()) {
    return;
  }
  LogLevel level = LogLevel::Debug;
  switch (importance) {
  case WLR_ERROR:
    level = LogLevel::Error;
    break;
  case WLR_INFO:
    level = LogLevel::Info;
    break;
  default:
    level = LogLevel::Debug;
    break;
  }

  // wlroots hands us a printf format plus varargs; render once into a fixed
  // buffer (truncation is fine, the logger caps line length anyway).
  std::array<char, 1024> buffer{};
  va_list copy;
  va_copy(copy, args);
  const int written = std::vsnprintf(buffer.data(), buffer.size(), fmt, copy);
  va_end(copy);
  if (written < 0) {
    return;
  }

  const auto length = std::min(static_cast<std::size_t>(written), buffer.size() - 1);
  const std::string_view message(buffer.data(), length);
  // EBUSY: xe completes an unused plane's previous commit just after the page-flip event, and libliftoff puts every
  // plane in each commit. Output::commitState retries it and logs a summary when a retry gives up.
  if (level == LogLevel::Error && message.ends_with("Atomic commit failed: Device or resource busy")) {
    level = LogLevel::Debug;
  }
  detail::logMessage(level, "wlr", message);
}
