#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class Flex;

namespace settings {

  struct CrashRecord {
    std::string exe;
    int pid = 0;
    std::int64_t timeUs = 0; // CLOCK_REALTIME
  };

  // Core dumps from journal JSON lines (`journalctl -o json`, systemd-coredump entries), newest first, at most 20.
  // Only processes the desktop started count: its session services and app units. A dump from a terminal or a
  // `systemd-run` scope (a test or a manual run) is not a desktop problem.
  [[nodiscard]] std::vector<CrashRecord> parseCrashRecords(std::string_view jsonLines);

  // Problems page (.agent/DESIGN.md "Settings > Problems", direction A). `records` is empty while loading;
  // `available` is false when coredumpctl is missing or failed.
  void addSettingsProblems(
      Flex& content, float scale, const std::optional<std::vector<CrashRecord>>& records, bool available
  );

} // namespace settings
