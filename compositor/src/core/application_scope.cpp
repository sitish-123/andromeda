#include "core/application_scope.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <poll.h>
#include <string>
#include <unistd.h>

namespace andromeda {

  bool isDirectSystemdService(pid_t currentPid, const char* systemdExecPid) {
    if (currentPid <= 0 || systemdExecPid == nullptr || systemdExecPid[0] == '\0') {
      return false;
    }
    pid_t parsed = 0;
    const std::string_view value(systemdExecPid);
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return error == std::errc{} && end == value.data() + value.size() && parsed == currentPid;
  }

  std::string systemdUnitFromCgroup(std::string_view cgroup) {
    std::string_view unit;
    size_t unitDepth = 0;
    size_t lineStart = 0;
    while (lineStart <= cgroup.size()) {
      const size_t lineEnd = cgroup.find('\n', lineStart);
      const std::string_view line =
          cgroup.substr(lineStart, (lineEnd == std::string_view::npos ? cgroup.size() : lineEnd) - lineStart);
      const size_t pathSeparator = line.rfind(':');
      std::string_view path = pathSeparator == std::string_view::npos ? line : line.substr(pathSeparator + 1);
      size_t componentStart = 0;
      size_t componentDepth = 0;
      while (componentStart <= path.size()) {
        const size_t componentEnd = path.find('/', componentStart);
        const std::string_view component = path.substr(
            componentStart, (componentEnd == std::string_view::npos ? path.size() : componentEnd) - componentStart
        );
        if (!component.empty()) {
          ++componentDepth;
        }
        if ((component.ends_with(".service") || component.ends_with(".scope")) && componentDepth >= unitDepth) {
          unit = component;
          unitDepth = componentDepth;
        }
        if (componentEnd == std::string_view::npos) {
          break;
        }
        componentStart = componentEnd + 1;
      }
      if (lineEnd == std::string_view::npos) {
        break;
      }
      lineStart = lineEnd + 1;
    }
    return std::string(unit);
  }

  std::string currentSystemdUnit() {
    std::ifstream stream("/proc/self/cgroup");
    if (!stream) {
      return {};
    }
    return systemdUnitFromCgroup(std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()));
  }

  SystemdControlEnvironment captureSystemdControlEnvironment() {
    SystemdControlEnvironment environment;
    for (size_t index = 0; index < kSystemdControlEnvironmentVariables.size(); ++index) {
      if (const char* value = std::getenv(kSystemdControlEnvironmentVariables[index]); value != nullptr) {
        environment[index] = value;
      }
    }
    return environment;
  }

  bool restoreSystemdControlEnvironment(const SystemdControlEnvironment& environment) {
    for (size_t index = 0; index < kSystemdControlEnvironmentVariables.size(); ++index) {
      const char* name = kSystemdControlEnvironmentVariables[index];
      if (environment[index]) {
        if (setenv(name, environment[index]->c_str(), 1) != 0) {
          return false;
        }
      } else if (unsetenv(name) != 0) {
        return false;
      }
    }
    return true;
  }

  ApplicationScopeEnvironmentArguments
  applicationScopeEnvironmentArguments(std::span<const std::pair<std::string, std::string>> configuredEnvironment) {
    ApplicationScopeEnvironmentArguments arguments;
    for (const auto& [name, value] : configuredEnvironment) {
      for (size_t index = 0; index < kSystemdControlEnvironmentVariables.size(); ++index) {
        if (name == kSystemdControlEnvironmentVariables[index]) {
          arguments[index] = "--setenv=" + name + "=" + value;
          break;
        }
      }
    }
    return arguments;
  }

  void execApplicationInScope(
      const char* systemdRun, const char* unitArgument, const char* partOfProperty,
      const char* afterProperty, const ApplicationScopeEnvironmentArguments& environmentArguments, const char* command,
      const char* cpuWeightProperty
  ) {
    std::array<char*, 22> arguments{};
    size_t argumentCount = 0;
    const auto append = [&arguments, &argumentCount](const char* argument) {
      arguments[argumentCount++] = const_cast<char*>(argument);
    };
    append("systemd-run");
    append("--user");
    append("--scope");
    append("--quiet");
    append("--no-ask-password");
    append("--collect");
    append("--expand-environment=no");
    append("--slice=app.slice");
    append("--description=Andromeda application");
    append(unitArgument);
    append(partOfProperty);
    append(afterProperty);
    if (cpuWeightProperty != nullptr) {
      append(cpuWeightProperty);
    }
    for (const std::string& argument : environmentArguments) {
      if (!argument.empty()) {
        append(argument.c_str());
      }
    }
    append("--");
    append("/bin/sh");
    append("-c");
    append(command);
    execv(systemdRun, arguments.data());
    _exit(1);
  }

  bool awaitApplicationScope(int gateFd, const char* unit) {
    constexpr long kCapMs = 100;
    timespec start{};
    clock_gettime(CLOCK_MONOTONIC, &start);
    const auto remainingMs = [&start] {
      timespec now{};
      clock_gettime(CLOCK_MONOTONIC, &now);
      return kCapMs - ((now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1'000'000);
    };
    pollfd gate{.fd = gateFd, .events = POLLIN, .revents = 0};
    int ready = 0;
    do {
      ready = poll(&gate, 1, static_cast<int>(std::max(0L, remainingMs())));
    } while (ready < 0 && errno == EINTR);
    if (ready <= 0) {
      return true;
    }
    char result = 0;
    ssize_t size = 0;
    do {
      size = read(gateFd, &result, 1);
    } while (size < 0 && errno == EINTR);
    if (size != 1 || result != '1') {
      return false;
    }
    // The reply means the job is queued; a busy user manager moves the PID later.
    constexpr timespec kStep{0, 1'000'000};
    while (remainingMs() > 0) {
      const int fd = open("/proc/self/cgroup", O_RDONLY | O_CLOEXEC);
      if (fd < 0) {
        return true;
      }
      char cgroup[1024];
      const ssize_t length = read(fd, cgroup, sizeof(cgroup) - 1);
      close(fd);
      if (length <= 0) {
        return true;
      }
      cgroup[length] = '\0';
      if (std::strstr(cgroup, unit) != nullptr) {
        return true;
      }
      nanosleep(&kStep, nullptr);
    }
    return true;
  }

  void applyApplicationScopeEnvironment(const ApplicationScopeEnvironmentArguments& arguments) {
    constexpr std::string_view kPrefix = "--setenv=";
    for (const std::string& argument : arguments) {
      if (argument.size() > kPrefix.size()) {
        putenv(const_cast<char*>(argument.c_str() + kPrefix.size()));
      }
    }
  }

} // namespace andromeda
