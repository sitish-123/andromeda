#pragma once

#include <filesystem>

namespace crash_report {

  // Where saved crash reports go: $XDG_STATE_HOME/andromeda/crash-reports.
  [[nodiscard]] std::filesystem::path directory();

  // Writes a local text report (core dump summary and the last 300 compositor log lines) and opens it.
  // pid 0 takes the newest andromeda dump. Nothing is sent anywhere.
  void save(int pid);

  // Opens the report folder in the file manager.
  void openDirectory();

} // namespace crash_report
