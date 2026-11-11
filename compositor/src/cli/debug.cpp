#include "cli/debug.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <print>
#include <regex>
#include <spawn.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace andromeda {

  namespace {

    namespace fs = std::filesystem;

    constexpr int kExitOk = 0;
    constexpr int kExitFailed = 1;
    constexpr int kExitUsage = 2;
    constexpr std::size_t kMaxLogLines = 200;
    constexpr std::size_t kMaxReportCrashes = 8;
    constexpr std::size_t kMaxMessageChars = 240;
    constexpr const char* kSystemCrashDir = "/var/lib/andromeda-system/crashes";
    constexpr const char* kGreeterCrashDir = "/var/lib/andromeda-greeter/crashes";

    // Journal identifiers (the binary names: no unit sets SyslogIdentifier) and their short names.
    struct Component {
      std::string_view id;
      std::string_view alias;
    };
    constexpr std::array<Component, 5> kComponents{{
        {"andromeda", "compositor"},
        {"andromeda-shell", "shell"},
        {"andromeda-greeter", "greeter"},
        {"andromeda-system", "system"},
        {"andromeda-linkd", "link"},
    }};

    // 0 error, 1 warn, 2 info, 3 debug.
    constexpr std::array<std::string_view, 4> kLevelNames{"error", "warn", "info", "debug"};

    void printUsage(FILE* stream) {
      std::println(stream, "Usage:");
      std::println(stream, "  andromeda debug crashes [--since 1h] [--json]");
      std::println(stream, "  andromeda debug log [component|all] [--since 15m] [--level warning] [--grep RE]");
      std::println(stream, "  andromeda debug report [--since 1h]");
      std::println(stream, "Durations: <n>s|m|h|d|w. Components: compositor, shell, greeter, system, link, all.");
      std::println(stream, "Exit: 0 ok, 1 failed (report: a failure was found), 2 usage.");
    }

    int usageError(std::string_view message) {
      std::println(stderr, "andromeda debug: {}", message);
      printUsage(stderr);
      return kExitUsage;
    }

    struct Captured {
      int status = -1;
      std::string out;
    };

    // Runs a program from PATH; nullopt when it cannot start.
    std::optional<Captured> run(const std::vector<std::string>& args, bool withStderr = false) {
      int fds[2];
      if (pipe2(fds, O_CLOEXEC) < 0) {
        return std::nullopt;
      }
      posix_spawn_file_actions_t actions;
      posix_spawn_file_actions_init(&actions);
      posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
      posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
      if (withStderr) {
        posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
      } else {
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
      }
      std::vector<char*> argv;
      argv.reserve(args.size() + 1);
      for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
      }
      argv.push_back(nullptr);
      pid_t pid = 0;
      const int spawned = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
      posix_spawn_file_actions_destroy(&actions);
      close(fds[1]);
      if (spawned != 0) {
        close(fds[0]);
        return std::nullopt;
      }
      Captured captured;
      std::array<char, 8192> buffer{};
      for (;;) {
        const ssize_t n = read(fds[0], buffer.data(), buffer.size());
        if (n < 0 && errno == EINTR) {
          continue;
        }
        if (n <= 0) {
          break;
        }
        captured.out.append(buffer.data(), static_cast<std::size_t>(n));
      }
      close(fds[0]);
      int status = 0;
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      captured.status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
      return captured;
    }

    std::string firstLine(std::string_view text) {
      const auto start = text.find_first_not_of(" \t\n");
      if (start == std::string_view::npos) {
        return {};
      }
      text.remove_prefix(start);
      return std::string(text.substr(0, text.find('\n')));
    }

    std::string clip(std::string_view text, std::size_t max = kMaxMessageChars) {
      if (text.size() <= max) {
        return std::string(text);
      }
      return std::string(text.substr(0, max)) + "…";
    }

    std::optional<std::int64_t> parseDuration(std::string_view text) {
      std::int64_t value = 0;
      std::size_t i = 0;
      while (i < text.size() && text[i] >= '0' && text[i] <= '9' && value < 100'000'000) {
        value = value * 10 + (text[i] - '0');
        ++i;
      }
      if (i == 0 || i + 1 != text.size()) {
        return std::nullopt;
      }
      switch (text[i]) {
      case 's':
        return value;
      case 'm':
        return value * 60;
      case 'h':
        return value * 3600;
      case 'd':
        return value * 86400;
      case 'w':
        return value * 7 * 86400;
      default:
        return std::nullopt;
      }
    }

    std::string localTime(std::int64_t secs, const char* format) {
      const auto t = static_cast<std::time_t>(secs);
      std::tm tm{};
      localtime_r(&t, &tm);
      std::array<char, 64> buffer{};
      const std::size_t n = std::strftime(buffer.data(), buffer.size(), format, &tm);
      return std::string(buffer.data(), n);
    }

    std::optional<std::int64_t> parseIsoUtc(const std::string& text) {
      std::tm tm{};
      if (std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2dZ", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour,
                      &tm.tm_min, &tm.tm_sec)
          != 6) {
        return std::nullopt;
      }
      tm.tm_year -= 1900;
      tm.tm_mon -= 1;
      return static_cast<std::int64_t>(timegm(&tm));
    }

    // <XDG_STATE_HOME or ~/.local/state>/andromeda, as protocol/crash_capture.h.
    fs::path stateHome() {
      if (const char* state = std::getenv("XDG_STATE_HOME"); state != nullptr && state[0] == '/') {
        return fs::path(state) / "andromeda";
      }
      if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return fs::path(home) / ".local/state/andromeda";
      }
      return {};
    }

    std::string basename(std::string_view path) {
      const auto slash = path.rfind('/');
      return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
    }

    bool isOurBinary(std::string_view name) {
      return name == "andromeda" || name.starts_with("andromeda-shell") || name.starts_with("andromeda-greeter")
          || name.starts_with("andromeda-system") || name == "andromeda-linkd";
    }

    // Frames in these are where a crash surfaced, rarely where it started: the top frame skips them.
    bool isRuntimeModule(std::string_view module) {
      const std::string name = basename(module);
      for (const std::string_view prefix : {"libc.so", "libstdc++.so", "libgcc_s.so", "ld-linux", "libm.so"}) {
        if (name.starts_with(prefix)) {
          return true;
        }
      }
      return false;
    }

    std::string symbolize(const std::string& module, std::uint64_t offset) {
      const std::string address = std::format("{:#x}", offset);
      std::error_code ec;
      if (fs::is_regular_file(module, ec)) {
        if (auto out = run({"addr2line", "-Cfpe", module, address}); out && out->status == 0) {
          const std::string line = firstLine(out->out);
          if (!line.empty() && !line.starts_with("??")) {
            return line;
          }
        }
        if (auto out = run({"llvm-symbolizer", "--obj=" + module, address}); out && out->status == 0) {
          std::istringstream lines(out->out);
          std::string function;
          std::string where;
          std::getline(lines, function);
          std::getline(lines, where);
          if (!function.empty() && function != "??") {
            return function + " at " + where;
          }
        }
      }
      return basename(module) + "+" + address;
    }

    struct Crash {
      std::int64_t time = 0;
      std::string component;
      int pid = 0;
      std::string signal;
      std::string version;
      std::string top;
      std::string file;
      bool coredump = false;
    };

    std::optional<Crash> parseCrashFile(const fs::path& path) {
      std::ifstream in(path);
      if (!in) {
        return std::nullopt;
      }
      Crash crash;
      crash.file = path.string();
      std::string message;
      std::string location;
      struct Frame {
        int index;
        std::string module;
        std::uint64_t offset;
      };
      std::optional<Frame> first;
      std::optional<Frame> top;
      bool inFrames = false;
      for (std::string line; std::getline(in, line);) {
        if (line == "log:") {
          break;
        }
        if (line == "backtrace:") {
          inFrames = true;
          continue;
        }
        if (inFrames) {
          // "#3 0x55d0c0ffee /usr/bin/andromeda+0x1a2b"
          const auto space = line.find(' ', line.find(' ') + 1);
          const auto plus = line.rfind("+0x");
          if (!line.starts_with('#') || space == std::string::npos || plus == std::string::npos || plus < space) {
            continue;
          }
          Frame frame{std::atoi(line.c_str() + 1), line.substr(space + 1, plus - space - 1),
                      std::strtoull(line.c_str() + plus + 1, nullptr, 16)};
          if (!first) {
            first = frame;
          }
          if (!top && !isRuntimeModule(frame.module)) {
            top = frame;
          }
          continue;
        }
        const auto colon = line.find(": ");
        if (colon == std::string::npos) {
          continue;
        }
        const std::string key = line.substr(0, colon);
        const std::string value = line.substr(colon + 2);
        if (key == "component") {
          crash.component = value;
        } else if (key == "version") {
          crash.version = value;
        } else if (key == "pid") {
          crash.pid = std::atoi(value.c_str());
        } else if (key == "signal") {
          crash.signal = value.substr(0, value.find(' '));
        } else if (key == "time") {
          crash.time = parseIsoUtc(value).value_or(0);
        } else if (key == "message") {
          message = value;
        } else if (key == "location") {
          location = value;
        }
      }
      if (crash.component.empty()) {
        return std::nullopt;
      }
      if (const auto& frame = top ? top : first) {
        // Frames below the first hold return addresses: the call is the byte before.
        crash.top = symbolize(frame->module, frame->offset - (frame->index > 0 ? 1 : 0));
      } else if (!location.empty()) {
        crash.top = clip(location + (message.empty() ? "" : ": " + message), 160);
      } else {
        crash.top = "-";
      }
      return crash;
    }

    std::string signalName(int sig) {
      if (const char* abbrev = sigabbrev_np(sig); abbrev != nullptr) {
        return std::string("SIG") + abbrev;
      }
      return std::to_string(sig);
    }

    // Crash files in both crash directories, merged with systemd-coredump's entries for our binaries; oldest first.
    std::vector<Crash> collectCrashes(std::int64_t since) {
      std::vector<Crash> crashes;
      const fs::path userDir = stateHome().empty() ? fs::path() : stateHome() / "crashes";
      for (const fs::path& dir : {userDir, fs::path(kSystemCrashDir), fs::path(kGreeterCrashDir)}) {
        std::error_code ec;
        if (dir.empty() || !fs::is_directory(dir, ec)) {
          continue;
        }
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
          if (entry.path().extension() != ".txt") {
            continue;
          }
          if (auto crash = parseCrashFile(entry.path()); crash && crash->time >= since) {
            crashes.push_back(std::move(*crash));
          }
        }
      }

      if (auto out = run({"coredumpctl", "--json=short", "--no-pager", std::format("--since=@{}", since)});
          out && out->status == 0) {
        const auto list = nlohmann::json::parse(out->out, nullptr, false);
        for (const auto& item : list.is_array() ? list : nlohmann::json::array()) {
          if (!item.is_object() || !item.value("exe", nlohmann::json()).is_string()) {
            continue;
          }
          const std::string name = basename(item["exe"].get<std::string>());
          const std::int64_t time = item.value("time", std::int64_t{0}) / 1'000'000;
          if (!isOurBinary(name) || time < since) {
            continue;
          }
          const int pid = item.value("pid", 0);
          const auto same = std::ranges::find_if(crashes, [&](const Crash& c) {
            return c.pid == pid && std::abs(c.time - time) <= 120;
          });
          if (same != crashes.end()) {
            same->coredump = true;
            continue;
          }
          crashes.push_back({.time = time,
                             .component = name,
                             .pid = pid,
                             .signal = signalName(item.value("sig", 0)),
                             .version = "",
                             .top = std::format("see: coredumpctl info {}", pid),
                             .file = "",
                             .coredump = true});
        }
      }
      std::ranges::sort(crashes, {}, &Crash::time);
      return crashes;
    }

    std::string crashLine(const Crash& crash) {
      std::string line = std::format("{}  {}  {}  {}  {}", localTime(crash.time, "%Y-%m-%d %H:%M:%S"), crash.component,
                                     crash.signal, crash.version.empty() ? "-" : crash.version, crash.top);
      if (!crash.file.empty()) {
        line += "  (" + crash.file + ")";
      }
      return line;
    }

    nlohmann::json crashesJson(const std::vector<Crash>& crashes) {
      auto list = nlohmann::json::array();
      for (const auto& crash : crashes) {
        list.push_back({{"time", localTime(crash.time, "%Y-%m-%dT%H:%M:%S%z")},
                        {"epoch", crash.time},
                        {"component", crash.component},
                        {"pid", crash.pid},
                        {"signal", crash.signal},
                        {"version", crash.version},
                        {"top_frame", crash.top},
                        {"file", crash.file},
                        {"coredump", crash.coredump}});
      }
      return list;
    }

    struct LogEntry {
      std::int64_t usec = 0;
      std::string component;
      int level = 2;
      std::string message;
    };

    std::optional<int> parseLevel(std::string_view name) {
      if (name == "error" || name == "err" || name == "crit") {
        return 0;
      }
      if (name == "warning" || name == "warn") {
        return 1;
      }
      if (name == "info" || name == "notice") {
        return 2;
      }
      if (name == "debug") {
        return 3;
      }
      return std::nullopt;
    }

    std::string stripAnsi(std::string_view text) {
      std::string out;
      out.reserve(text.size());
      for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\033' && i + 1 < text.size() && text[i + 1] == '[') {
          i += 2;
          while (i < text.size() && (text[i] < '@' || text[i] > '~')) {
            ++i;
          }
          continue;
        }
        out += text[i];
      }
      return out;
    }

    // Strips the logger's level prefix: C++ (compositor, libdesk-ui) "HH:MM:SS.mmm [WRN] ", Rust env_logger
    // "[<time> WARN  target] " or, on the journal, "[WARN target] ". The loggers now send the sd-daemon priority;
    // entries from before that are all priority 6, so for those (`fromText`) the prefix sets the level.
    void takeLevelFromPrefix(LogEntry& entry, bool fromText) {
      std::string_view text = entry.message;
      if (text.size() > 19 && text[2] == ':' && text[5] == ':' && text[8] == '.' && text[12] == ' ' && text[13] == '['
          && text[17] == ']' && text[18] == ' ') {
        const std::string_view tag = text.substr(14, 3);
        constexpr std::array<std::string_view, 4> kTags{"ERR", "WRN", "INF", "DBG"};
        if (const auto it = std::ranges::find(kTags, tag); it != kTags.end()) {
          if (fromText) {
            entry.level = static_cast<int>(it - kTags.begin());
          }
          entry.message.erase(0, 19);
        }
        return;
      }
      const auto close = text.find("] ");
      if (!text.starts_with('[') || close == std::string_view::npos) {
        return;
      }
      std::istringstream tokens{std::string(text.substr(1, close - 1))};
      constexpr std::array<std::string_view, 5> kRust{"ERROR", "WARN", "INFO", "DEBUG", "TRACE"};
      for (std::string token; tokens >> token;) {
        if (const auto it = std::ranges::find(kRust, token); it != kRust.end()) {
          if (fromText) {
            entry.level = std::min(static_cast<int>(it - kRust.begin()), 3);
          }
          entry.message.erase(0, close + 2);
          return;
        }
      }
    }

    std::vector<std::string> journalArgs(std::int64_t since, const std::vector<std::string_view>& ids, int maxLevel,
                                         bool json) {
      constexpr std::array<std::string_view, 4> kPriorities{"err", "warning", "info", "debug"};
      std::vector<std::string> args{"journalctl", "--no-pager"};
      if (json) {
        args.insert(args.end(), {"-o", "json", "--output-fields=SYSLOG_IDENTIFIER,PRIORITY,MESSAGE"});
      }
      args.insert(args.end(), {"-p", std::string(kPriorities[static_cast<std::size_t>(maxLevel)])});
      args.push_back(std::format("--since=@{}", since));
      for (const auto id : ids) {
        args.push_back("-t");
        args.emplace_back(id);
      }
      return args;
    }

    std::string joinArgs(const std::vector<std::string>& args) {
      std::string text;
      for (const auto& arg : args) {
        text += (text.empty() ? "" : " ") + arg;
      }
      return text;
    }

    // Oldest first; nullopt when journalctl cannot run or fails.
    std::optional<std::vector<LogEntry>> readJournal(std::int64_t since, const std::vector<std::string_view>& ids,
                                                     int maxLevel) {
      const auto out = run(journalArgs(since, ids, maxLevel, true));
      if (!out || out->status != 0) {
        return std::nullopt;
      }
      std::vector<LogEntry> entries;
      std::istringstream lines(out->out);
      for (std::string line; std::getline(lines, line);) {
        const auto record = nlohmann::json::parse(line, nullptr, false);
        if (!record.is_object()) {
          continue;
        }
        LogEntry entry;
        const auto& message = record.value("MESSAGE", nlohmann::json());
        if (message.is_string()) {
          entry.message = stripAnsi(message.get<std::string>());
        } else if (message.is_array()) {
          std::string bytes;
          for (const auto& byte : message) {
            bytes += static_cast<char>(byte.is_number_integer() ? byte.get<int>() : '?');
          }
          entry.message = stripAnsi(bytes);
        } else {
          continue;
        }
        entry.usec = std::strtoll(record.value("__REALTIME_TIMESTAMP", "0").c_str(), nullptr, 10);
        entry.component = record.value("SYSLOG_IDENTIFIER", "?");
        const int priority = std::atoi(record.value("PRIORITY", "6").c_str());
        entry.level = priority <= 3 ? 0 : priority == 4 ? 1 : priority <= 6 ? 2 : 3;
        takeLevelFromPrefix(entry, priority == 6);
        entries.push_back(std::move(entry));
      }
      std::ranges::stable_sort(entries, {}, &LogEntry::usec);
      return entries;
    }

    std::string entryLine(const LogEntry& entry, const char* timeFormat) {
      return std::format("{} {} {} {}", localTime(entry.usec / 1'000'000, timeFormat), entry.component,
                         kLevelNames[static_cast<std::size_t>(entry.level)], clip(entry.message));
    }

    std::vector<std::string_view> allIds() {
      std::vector<std::string_view> ids;
      for (const auto& component : kComponents) {
        ids.push_back(component.id);
      }
      return ids;
    }

    // --- debug crashes -------------------------------------------------------------------------------------------

    int runCrashes(int argc, char** argv) {
      std::int64_t window = 3600;
      bool json = false;
      for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--json" || arg == "-j") {
          json = true;
        } else if (arg == "--since" && i + 1 < argc) {
          const auto parsed = parseDuration(argv[++i]);
          if (!parsed) {
            return usageError(std::format("invalid duration '{}'", argv[i]));
          }
          window = *parsed;
        } else if (arg == "-h" || arg == "--help") {
          printUsage(stdout);
          return kExitOk;
        } else {
          return usageError(std::format("unknown argument '{}'", arg));
        }
      }
      const auto crashes = collectCrashes(std::time(nullptr) - window);
      if (json) {
        std::println("{}", crashesJson(crashes).dump(2));
        return kExitOk;
      }
      if (crashes.empty()) {
        std::println("no crashes in the last {}s", window);
      }
      for (const auto& crash : crashes) {
        std::println("{}", crashLine(crash));
      }
      return kExitOk;
    }

    // --- debug log -----------------------------------------------------------------------------------------------

    int runLog(int argc, char** argv) {
      std::int64_t window = 15 * 60;
      int maxLevel = 1;
      std::optional<std::regex> grep;
      std::vector<std::string_view> ids;
      for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--since" && i + 1 < argc) {
          const auto parsed = parseDuration(argv[++i]);
          if (!parsed) {
            return usageError(std::format("invalid duration '{}'", argv[i]));
          }
          window = *parsed;
        } else if (arg == "--level" && i + 1 < argc) {
          const auto parsed = parseLevel(argv[++i]);
          if (!parsed) {
            return usageError(std::format("invalid level '{}'; use error, warning, info or debug", argv[i]));
          }
          maxLevel = *parsed;
        } else if (arg == "--grep" && i + 1 < argc) {
          try {
            grep.emplace(argv[++i], std::regex::ECMAScript | std::regex::optimize);
          } catch (const std::regex_error&) {
            return usageError(std::format("invalid regular expression '{}'", argv[i]));
          }
        } else if (arg == "-h" || arg == "--help") {
          printUsage(stdout);
          return kExitOk;
        } else if (!arg.starts_with('-') && ids.empty()) {
          if (arg == "all") {
            ids = allIds();
            continue;
          }
          const auto it = std::ranges::find_if(kComponents, [&](const Component& c) {
            return c.id == arg || c.alias == arg;
          });
          if (it == kComponents.end()) {
            return usageError(std::format("unknown component '{}'", arg));
          }
          ids.push_back(it->id);
        } else {
          return usageError(std::format("unknown argument '{}'", arg));
        }
      }
      if (ids.empty()) {
        ids = allIds();
      }
      const std::int64_t since = std::time(nullptr) - window;
      const auto entries = readJournal(since, ids, maxLevel);
      if (!entries) {
        std::println(stderr, "andromeda debug: journalctl failed; run: {}", joinArgs(journalArgs(since, ids, maxLevel, false)));
        return kExitFailed;
      }
      std::vector<const LogEntry*> shown;
      for (const auto& entry : *entries) {
        if (entry.level <= maxLevel && (!grep || std::regex_search(entry.message, *grep))) {
          shown.push_back(&entry);
        }
      }
      const char* timeFormat = window <= 86400 ? "%H:%M:%S" : "%m-%d %H:%M:%S";
      const std::size_t skip = shown.size() > kMaxLogLines ? shown.size() - kMaxLogLines : 0;
      for (std::size_t i = skip; i < shown.size(); ++i) {
        std::println("{}", entryLine(*shown[i], timeFormat));
      }
      if (skip > 0) {
        std::println("{} more, see: {}", skip, joinArgs(journalArgs(since, ids, maxLevel, false)));
      }
      return kExitOk;
    }

    // --- debug report --------------------------------------------------------------------------------------------

    std::string findBinary(std::string_view name) {
      std::string dirs = std::getenv("PATH") != nullptr ? std::getenv("PATH") : "/usr/bin";
      dirs += ":/usr/lib/andromeda";
      std::istringstream parts(dirs);
      for (std::string dir; std::getline(parts, dir, ':');) {
        const fs::path candidate = fs::path(dir) / name;
        if (!dir.empty() && access(candidate.c_str(), X_OK) == 0) {
          return candidate.string();
        }
      }
      return {};
    }

    // The package that owns the file, through pacman or dpkg.
    std::string packageVersion(const std::string& path) {
      if (auto out = run({"pacman", "-Qo", path}); out && out->status == 0) {
        const std::string line = firstLine(out->out);
        if (const auto owned = line.find(" is owned by "); owned != std::string::npos) {
          return line.substr(owned + 13);
        }
      }
      if (auto out = run({"dpkg", "-S", path}); out && out->status == 0) {
        const std::string line = firstLine(out->out);
        const std::string package = line.substr(0, line.find(':'));
        if (auto version = run({"dpkg-query", "-W", "-f=${Version}", package}); version && version->status == 0) {
          return package + " " + firstLine(version->out);
        }
      }
      return "unknown";
    }

    struct Failure {
      std::int64_t time;
      std::string text;
    };

    int runReport(int argc, char** argv, std::string_view selfVersion) {
      std::int64_t window = 3600;
      std::string windowText = "1h";
      for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--since" && i + 1 < argc) {
          const auto parsed = parseDuration(argv[++i]);
          if (!parsed) {
            return usageError(std::format("invalid duration '{}'", argv[i]));
          }
          window = *parsed;
          windowText = argv[i];
        } else if (arg == "-h" || arg == "--help") {
          printUsage(stdout);
          return kExitOk;
        } else {
          return usageError(std::format("unknown argument '{}'", arg));
        }
      }
      const std::int64_t now = std::time(nullptr);
      const std::int64_t since = now - window;
      std::vector<std::string> lines;
      std::vector<Failure> failures;

      std::vector<std::string> versions{std::format("version  andromeda  {}", selfVersion)};
      for (const std::string_view name : {"andromeda-shell", "andromeda-system", "andromeda-linkd"}) {
        const std::string path = findBinary(name);
        std::string version = "not installed";
        if (!path.empty() && name == "andromeda-shell") {
          const auto out = run({path, "--version"});
          version = out && out->status == 0 ? firstLine(out->out) : packageVersion(path);
        } else if (!path.empty()) {
          version = packageVersion(path);
        }
        versions.push_back(std::format("version  {}  {}", name, version));
      }

      std::string unitsRaw;
      std::vector<std::string> failedUnits;
      int running = 0;
      for (const bool user : {true, false}) {
        std::vector<std::string> args{"systemctl"};
        if (user) {
          args.emplace_back("--user");
        }
        args.insert(args.end(), {"list-units", "--all", "--plain", "--no-legend", "--no-pager", "andromeda*"});
        const auto out = run(args);
        if (!out) {
          continue;
        }
        unitsRaw += std::format("# {}\n{}", joinArgs(args), out->out);
        std::istringstream rows(out->out);
        for (std::string row; std::getline(rows, row);) {
          std::istringstream fields(row);
          std::string unit;
          std::string load;
          std::string active;
          std::string sub;
          fields >> unit >> load >> active >> sub;
          if (active == "failed") {
            failedUnits.push_back(unit + (user ? "" : " (system)"));
          } else if (sub == "running") {
            ++running;
          }
        }
      }
      for (const auto& unit : failedUnits) {
        failures.push_back({now, "unit failed: " + unit});
      }

      const auto journal = readJournal(since, allIds(), 1);
      std::vector<std::string> errorLines;
      std::string journalText;
      if (journal) {
        std::vector<std::string> seen;
        for (const auto& entry : *journal) {
          if (entry.level > 1) {
            continue;
          }
          journalText += entryLine(entry, "%Y-%m-%d %H:%M:%S") + "\n";
          if (entry.level == 0 && std::ranges::find(seen, entry.component) == seen.end()) {
            seen.push_back(entry.component);
            errorLines.push_back("  " + entryLine(entry, "%H:%M:%S"));
            failures.push_back({entry.usec / 1'000'000, std::format("{} error: {}", entry.component, clip(entry.message, 160))});
          }
        }
      }

      const auto crashes = collectCrashes(since);
      for (const auto& crash : crashes) {
        failures.push_back({crash.time, std::format("{} {} at {}", crash.component, crash.signal, crash.top)});
      }

      const auto validate = run({"/proc/self/exe", "config", "validate"}, true);
      const bool configOk = validate && validate->status == 0;
      const std::string configLine =
          configOk ? "config: ok" : "config: invalid: " + (validate ? clip(firstLine(validate->out), 160) : "validate did not run");
      if (!configOk) {
        failures.push_back({now, configLine});
      }

      std::ranges::stable_sort(failures, {}, &Failure::time);
      lines.push_back(failures.empty() ? std::format("no failures in the last {}", windowText)
                                       : std::format("first failure: {} {}",
                                                     localTime(failures.front().time, "%H:%M:%S"), failures.front().text));
      lines.insert(lines.end(), versions.begin(), versions.end());
      std::string unitsLine = std::format("units: {} running, {} failed", running, failedUnits.size());
      for (std::size_t i = 0; i < failedUnits.size() && i < 5; ++i) {
        unitsLine += (i == 0 ? ": " : ", ") + failedUnits[i];
      }
      lines.push_back(unitsLine);
      lines.push_back(journal ? std::format("errors: {} component(s) logged errors", errorLines.size())
                              : "errors: journalctl failed");
      lines.insert(lines.end(), errorLines.begin(), errorLines.end());
      lines.push_back(std::format("crashes: {}", crashes.size()));
      const std::size_t firstCrash = crashes.size() > kMaxReportCrashes ? crashes.size() - kMaxReportCrashes : 0;
      for (std::size_t i = firstCrash; i < crashes.size(); ++i) {
        lines.push_back("  " + crashLine(crashes[i]));
      }
      lines.push_back(configLine);

      std::string summary;
      for (const auto& line : lines) {
        summary += line + "\n";
      }
      std::print("{}", summary);

      std::error_code ec;
      const fs::path dir = stateHome().empty() ? fs::path() : stateHome() / "reports" / localTime(now, "%Y%m%dT%H%M%S");
      if (!dir.empty()) {
        fs::create_directories(dir, ec);
      }
      if (dir.empty() || ec) {
        std::println("full report: not written ({})", dir.empty() ? "no HOME" : ec.message());
      } else {
        const auto write = [&dir](const char* name, const std::string& text) { std::ofstream(dir / name) << text; };
        write("summary.txt", summary);
        std::string versionsText;
        for (const auto& line : versions) {
          versionsText += line + "\n";
        }
        write("versions.txt", versionsText);
        write("units.txt", unitsRaw);
        write("journal.txt", journal ? journalText : "journalctl failed\n");
        write("crashes.json", crashesJson(crashes).dump(2) + "\n");
        write("config.txt", validate ? validate->out : "validate did not run\n");
        std::println("full report: {}", dir.string());
      }
      return failures.empty() ? kExitOk : kExitFailed;
    }

  } // namespace

  int runDebug(int argc, char** argv, std::string_view selfVersion) {
    if (argc < 2) {
      return usageError("missing subcommand");
    }
    const std::string_view command = argv[1];
    if (command == "crashes") {
      return runCrashes(argc - 1, argv + 1);
    }
    if (command == "log") {
      return runLog(argc - 1, argv + 1);
    }
    if (command == "report") {
      return runReport(argc - 1, argv + 1, selfVersion);
    }
    if (command == "-h" || command == "--help" || command == "help") {
      printUsage(stdout);
      return kExitOk;
    }
    return usageError(std::format("unknown subcommand '{}'", command));
  }

} // namespace andromeda
