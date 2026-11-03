#pragma once

#include <cstdint>
#include <filesystem>

// Wrong-password delay: after 5 rejected passwords 30 s, doubling per further reject, at most 5 min.
// The count survives a shell restart (state file) and clears on a successful unlock.
class LockFailDelay {
public:
  static constexpr int kFreeAttempts = 4;

  // An empty path keeps the count in memory only.
  explicit LockFailDelay(std::filesystem::path stateFile);

  void recordRejected(std::int64_t nowMs);
  void reset();
  [[nodiscard]] std::int64_t remainingMs(std::int64_t nowMs) const;
  [[nodiscard]] int failures() const noexcept { return m_failures; }

  [[nodiscard]] static std::int64_t delayMsFor(int failures);
  // <state dir>/lock-fails, or empty when there is no state dir.
  [[nodiscard]] static std::filesystem::path defaultPath();

private:
  void save() const;

  std::filesystem::path m_path;
  int m_failures = 0;
  std::int64_t m_untilMs = 0;
};
