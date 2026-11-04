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

  // The user's core dumps from `coredumpctl list --json=short`, newest first, at most 20.
  [[nodiscard]] std::vector<CrashRecord> parseCrashRecords(std::string_view json);

  // Problems page (.agent/DESIGN.md "Settings > Problems", direction A). `records` is empty while loading;
  // `available` is false when coredumpctl is missing or failed.
  void addSettingsProblems(
      Flex& content, float scale, const std::optional<std::vector<CrashRecord>>& records, bool available
  );

} // namespace settings
