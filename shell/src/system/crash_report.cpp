#include "system/crash_report.h"

#include "core/process/process.h"

#include <cstdlib>
#include <format>
#include <string>
#include <system_error>
#include <vector>

namespace crash_report {

  std::filesystem::path directory() {
    const char* state = std::getenv("XDG_STATE_HOME");
    const char* home = std::getenv("HOME");
    const std::filesystem::path base = state != nullptr && *state != '\0'
        ? std::filesystem::path(state)
        : std::filesystem::path(home != nullptr ? home : "") / ".local/state";
    return base / "andromeda/crash-reports";
  }

  void save(int pid) {
    const std::string match = pid > 0 ? std::to_string(pid) : "-1 andromeda";
    (void)process::runAsync(std::format(
        "d='{}'; mkdir -p \"$d\"; f=\"$d/$(date +%Y%m%d-%H%M%S).txt\"; "
        "{{ echo \"Andromeda crash report $(date -Is)\"; coredumpctl info --no-pager {} 2>&1; echo; "
        "journalctl --user -u andromeda.service -n 300 --no-pager 2>&1; }} > \"$f\"; xdg-open \"$f\"",
        directory().string(), match
    ));
  }

  void openDirectory() {
    std::error_code ec;
    std::filesystem::create_directories(directory(), ec);
    (void)process::runAsync(std::vector<std::string>{"xdg-open", directory().string()});
  }

} // namespace crash_report
