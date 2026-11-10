#include "core/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <format>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <string>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {

  LogLevel gMinLevel = LogLevel::Info;
  FILE* gLogFile = nullptr;
  // gLogMutex: the level (callers). gFileMutex: the log file (writer thread, init). Callers never take gFileMutex,
  // so a writer blocked in I/O cannot block them.
  std::mutex gLogMutex;
  std::mutex gFileMutex;
  std::atomic<bool> gFileEnabled{false};
  std::atomic<LogSink> gLogSink{nullptr};
  std::string gLogPath;
  std::string gBackupLogPath;
  std::uintmax_t gLogSizeBytes = 0;
  std::size_t gBufferedFileLogLines = 0;
  std::chrono::steady_clock::time_point gLastFileFlushAt = std::chrono::steady_clock::now();
  bool gRegisteredExitFlush = false;

  constexpr std::size_t kMaxLogBytes = 1 * 1024 * 1024; // 1 MB
  constexpr std::size_t kMaxLogLineBytes = 8 * 1024;    // 8 KiB
  constexpr std::size_t kBufferedFileLogFlushLines = 64;
  constexpr auto kBufferedFileLogFlushInterval = std::chrono::milliseconds(500);
  constexpr const char* kLogLevelEnv = "ANDROMEDA_LOG_LEVEL";

  // Every write runs on one writer thread: a write to the journal or the log file can block for seconds under disk
  // writeback throttling or a busy journald, and on the main thread that froze the UI. Callers only queue lines;
  // past this budget they drop them and the writer reports the count.
  constexpr std::size_t kMaxQueuedBytes = 4 * 1024 * 1024;

  struct QueuedLine {
    std::string console;
    std::string file;
    bool flush = false;
  };

  std::mutex gQueueMutex;
  std::condition_variable gQueueReady;
  std::condition_variable gQueueDrained;
  bool gWriterBusy = false;
  std::deque<QueuedLine> gQueue;
  std::size_t gQueuedBytes = 0;
  std::size_t gDroppedLines = 0;
  std::size_t gSkippedConsoleLines = 0;
  bool gWriterStop = false;
  bool gWriterRunning = false;
  // Set by the exit join: later lines (static destructors, other threads) are written in place.
  bool gWriterShutDown = false;
  // A forked child has no writer thread; it writes in place.
  bool gInForkedChild = false;
  std::thread gWriter;

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

  std::string consolePrefix(const std::tm& tm, const std::timespec& ts, LogLevel level, const char* section) {
    char buffer[96];
    const int length = std::snprintf(
        buffer, sizeof(buffer), "%02d:%02d:%02d.%03ld [%s]", tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1'000'000,
        levelTagAnsi(level)
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

  std::string filePrefix(const std::tm& tm, const std::timespec& ts, LogLevel level, const char* section) {
    char buffer[128];
    const int length = std::snprintf(
        buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03ld [%s]", tm.tm_year + 1900, tm.tm_mon + 1,
        tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1'000'000, levelTagPlain(level)
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

  // Runs on the writer thread, or in place in a forked child.
  void writeQueuedLine(const QueuedLine& line) {
    if (!line.file.empty()) {
      std::scoped_lock lock(gFileMutex);
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
    // Named: the idle-thread checks count unnamed threads as on-demand workers that should have exited.
    pthread_setname_np(pthread_self(), "dsk-log");
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
      gWriterBusy = true;
      lock.unlock();
      if (dropped > 0) {
        const std::string note = std::format("[log] {} lines dropped: the log writer fell behind\n", dropped);
        writeQueuedLine({.console = note, .file = note, .flush = true});
      }
      writeQueuedLine(line);
      lock.lock();
      gWriterBusy = false;
      if (gQueue.empty()) {
        gQueueDrained.notify_all();
      }
    }
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
    gWriterShutDown = true;
  }

  void queueLine(QueuedLine line) {
    if (gInForkedChild) {
      writeQueuedLine(line);
      return;
    }
    std::unique_lock lock(gQueueMutex);
    if (gWriterShutDown) {
      lock.unlock();
      writeQueuedLine(line);
      return;
    }
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

  void flushLogFileAtExit() {
    stopWriter();
    std::scoped_lock lock(gFileMutex);
    flushLogFileUnlocked();
  }

  // gFileMutex is not taken: the writer holds it during disk I/O, and fork() must not wait for that. The child may
  // inherit it locked, so it never touches the file: it logs to the console only.
  void prepareFork() {
    gLogMutex.lock();
    gQueueMutex.lock();
  }

  void afterForkParent() {
    gQueueMutex.unlock();
    gLogMutex.unlock();
  }

  void afterForkChild() {
    gQueueMutex.unlock();
    gLogMutex.unlock();
    gInForkedChild = true;
    gFileEnabled.store(false, std::memory_order_relaxed);
  }

  [[maybe_unused]] const bool gForkHandlers = pthread_atfork(prepareFork, afterForkParent, afterForkChild) == 0;

} // namespace

std::string_view logLevelName(LogLevel level) {
  switch (level) {
  case LogLevel::Debug:
    return "debug";
  case LogLevel::Info:
    return "info";
  case LogLevel::Warn:
    return "warn";
  case LogLevel::Error:
    return "error";
  }
  return "unknown";
}

std::optional<LogLevel> parseLogLevel(std::string_view value) {
  if (value == "debug") {
    return LogLevel::Debug;
  }
  if (value == "info") {
    return LogLevel::Info;
  }
  if (value == "warn") {
    return LogLevel::Warn;
  }
  if (value == "error") {
    return LogLevel::Error;
  }
  return std::nullopt;
}

void initLogFile() {
  const char* cacheHome = std::getenv("XDG_CACHE_HOME");
  const char* home = std::getenv("HOME");

  std::string dir;
  if (cacheHome != nullptr && cacheHome[0] != '\0') {
    dir = std::string(cacheHome) + "/noctalia";
  } else if (home != nullptr && home[0] != '\0') {
    dir = std::string(home) + "/.cache/noctalia";
  } else {
    return; // no writable location available
  }

  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    return;
  }

  const std::string logPath = dir + "/noctalia.log";
  const std::string backupPath = dir + "/noctalia.log.1";

  std::scoped_lock lock(gFileMutex);
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
  gFileEnabled.store(gLogFile != nullptr, std::memory_order_release);
}

void flushLogs() {
  {
    std::unique_lock lock(gQueueMutex);
    gQueueDrained.wait(lock, [] { return !gWriterRunning || (gQueue.empty() && !gWriterBusy); });
  }
  std::scoped_lock lock(gFileMutex);
  flushLogFileUnlocked();
}

namespace detail {

  void logMessage(LogLevel level, const char* section, std::string_view msg) {
    if (LogSink sink = gLogSink.load(std::memory_order_acquire); sink != nullptr) {
      if (level >= gMinLevel) {
        sink(level, section, msg);
      }
      return;
    }
    std::timespec ts{};
    std::timespec_get(&ts, TIME_UTC);
    std::tm tm{};
    localtime_r(&ts.tv_sec, &tm);

    LogLevel minLevel = LogLevel::Info;
    {
      std::scoped_lock lock(gLogMutex);
      minLevel = gMinLevel;
    }

    QueuedLine line;
    // Console: respects gMinLevel, ANSI colours, time only
    if (level >= minLevel) {
      const std::string prefix = consolePrefix(tm, ts, level, section);
      const CappedLogMessage capped = capMessageForLine(msg, prefix.size());
      line.console = prefix;
      line.console += capped.text();
      line.console += '\n';
    }

    // File: always unfiltered, no ANSI, full date for context
    if (gFileEnabled.load(std::memory_order_acquire)) {
      const std::string prefix = filePrefix(tm, ts, level, section);
      const CappedLogMessage capped = capMessageForLine(msg, prefix.size());
      line.file = prefix;
      line.file += capped.text();
      line.file += '\n';
      line.flush = level >= LogLevel::Warn;
    }
    if (!line.console.empty() || !line.file.empty()) {
      queueLine(std::move(line));
    }
  }

} // namespace detail

void setLogSink(LogSink sink) { gLogSink.store(sink, std::memory_order_release); }

LogLevel currentLogLevel() {
  std::scoped_lock lock(gLogMutex);
  return gMinLevel;
}

void setLogLevel(LogLevel level) {
  std::scoped_lock lock(gLogMutex);
  gMinLevel = level;
}

void initLogLevelFromEnvironment() {
  const char* value = std::getenv(kLogLevelEnv);
  if (value == nullptr) {
    return;
  }

  const auto level = parseLogLevel(value);
  if (!level.has_value()) {
    logWarn("invalid {} '{}'; expected debug, info, warn, or error", kLogLevelEnv, value);
    return;
  }

  setLogLevel(*level);
  logInfo("log level set to {}", logLevelName(*level));
}
