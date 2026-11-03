#include "shell/lockscreen/lock_fail_delay.h"

#include "config/atomic_file.h"
#include "util/file_utils.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <system_error>

namespace {

  constexpr std::int64_t kFirstDelayMs = 30'000;
  constexpr std::int64_t kMaxDelayMs = 300'000;

} // namespace

LockFailDelay::LockFailDelay(std::filesystem::path stateFile) : m_path(std::move(stateFile)) {
  if (m_path.empty()) {
    return;
  }
  std::ifstream in(m_path);
  int failures = 0;
  std::int64_t untilMs = 0;
  if (in >> failures >> untilMs && failures > 0) {
    m_failures = failures;
    m_untilMs = untilMs;
  }
}

std::int64_t LockFailDelay::delayMsFor(int failures) {
  if (failures <= kFreeAttempts) {
    return 0;
  }
  const int doublings = std::min(failures - kFreeAttempts - 1, 4);
  return std::min(kFirstDelayMs << doublings, kMaxDelayMs);
}

std::filesystem::path LockFailDelay::defaultPath() {
  const std::string dir = FileUtils::stateDir();
  return dir.empty() ? std::filesystem::path{} : std::filesystem::path(dir) / "lock-fails";
}

void LockFailDelay::recordRejected(std::int64_t nowMs) {
  ++m_failures;
  m_untilMs = nowMs + delayMsFor(m_failures);
  save();
}

void LockFailDelay::reset() {
  if (m_failures == 0) {
    return;
  }
  m_failures = 0;
  m_untilMs = 0;
  if (!m_path.empty()) {
    std::error_code ec;
    std::filesystem::remove(m_path, ec);
  }
}

std::int64_t LockFailDelay::remainingMs(std::int64_t nowMs) const {
  // The cap keeps a wall clock set back from stretching the wait.
  return std::clamp<std::int64_t>(m_untilMs - nowMs, 0, delayMsFor(m_failures));
}

void LockFailDelay::save() const {
  if (m_path.empty()) {
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(m_path.parent_path(), ec);
  (void)writeTextFileAtomic(
      m_path, std::format("{} {}\n", m_failures, m_untilMs),
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write
  );
}
